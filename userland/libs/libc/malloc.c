// malloc, free, calloc and realloc for ring 3: a slab cache for small
// requests in front of kernel/lib/heap_core.c, which takes the rest.
//
// Small objects are most of what a program allocates, and in the shared
// heap each one pays a 48-byte header and a trip through its size-class
// lists. Here a request up to SLAB_MAX takes a slot in a 64 KiB SPAN of
// same-sized slots: one pop off the span's free list, or one bump into
// slots never used (so a span's untouched pages are never faulted in).
// It is the front-end/back-end split of glibc's tcache over ptmalloc and
// NT's Low Fragmentation Heap over its backend, kept to one lock and no
// per-thread caches.
//
// HOW free() TELLS THE TWO APART: the word before every pointer.
//
//   slab slot:   [free-list link][TAG_LIVE<<48 | span][payload ...]
//   heap block:  [...header...][prev, a canonical pointer][payload ...]
//   red-zoned:   [...][HEAP_RZ_MAGIC, 0xC0DE in the top 16 bits][payload]
//
// A ring-3 pointer has its top 16 bits clear, so the three never
// collide; heap_core.c already relies on the same fact for its
// red-zones. A freed slot's word becomes TAG_FREE, which makes a double
// free a no-op, as it is in heap_core.c.
//
// `heap debug on` belongs to heap_core.c: while it is on, every request
// goes there, red-zoned, and the slabs only drain.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <heap.h>
#include "rt/sys.h"

#define SLAB_SPAN    (64u * 1024u)
#define SLAB_MAX     1024u
#define SLAB_CLASSES (SLAB_MAX / 16u)
#define SLAB_HDR     64u                    // struct span, rounded to keep slots 16-aligned
#define SLAB_MAGIC   0x534c4142u            // 'SLAB'
#define TAG_LIVE     0x51abull
#define TAG_FREE     0x51afull
#define TAG_PTR      0x0000ffffffffffffull

struct span {
    struct span *next, *prev;   // its class's spans with a free slot
    void *free;                 // freed slots, linked through their first word
    uint32_t magic;
    uint32_t cls;               // payload is (cls + 1) * 16 bytes
    uint32_t stride, nslots;
    uint32_t bump;              // slots [bump, nslots) were never handed out
    uint32_t nfree;             // freed + never handed out
};

_Static_assert(sizeof(struct span) <= SLAB_HDR, "span header outgrew SLAB_HDR");

static struct span *g_partial[SLAB_CLASSES];
static volatile int g_lock;

// The same shape as heap_os_lock(), and a DIFFERENT lock: a span comes
// from kmalloc(), which takes that one. The order is always this lock,
// then the heap's.
static void lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        sys_yield();
}

static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static void push(struct span *s) {
    s->prev = 0;
    s->next = g_partial[s->cls];
    if (s->next) s->next->prev = s;
    g_partial[s->cls] = s;
}

static void unlink_span(struct span *s) {
    if (s->prev) s->prev->next = s->next;
    else g_partial[s->cls] = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = 0;
}

static struct span *new_span(uint32_t cls) {
    struct span *s = kmalloc(SLAB_SPAN);
    if (!s) return 0;
    s->free = 0;
    s->magic = SLAB_MAGIC;
    s->cls = cls;
    s->stride = (cls + 1) * 16 + 16;
    s->nslots = (SLAB_SPAN - SLAB_HDR) / s->stride;
    s->bump = 0;
    s->nfree = s->nslots;
    push(s);
    return s;
}

// The span a slab pointer belongs to, or NULL when it is not one.
static struct span *span_of(const void *p, uint64_t *tag_hi) {
    uint64_t w = ((const uint64_t *)p)[-1];
    *tag_hi = w >> 48;
    if (*tag_hi != TAG_LIVE && *tag_hi != TAG_FREE) return 0;
    struct span *s = (struct span *)(uintptr_t)(w & TAG_PTR);
    return s->magic == SLAB_MAGIC ? s : 0;
}

// The body of malloc() under another name: calloc() and realloc() call
// THIS, because a compiler allowed to treat malloc as the builtin folds
// `malloc(n)` + `memset(0)` into a call to calloc -- which, inside
// calloc, is a jump to itself (glibc builds with -fno-builtin-malloc).
static void *alloc(size_t n) {
    if (!n) n = 1;   // a unique block, as stdlib.h's malloc(0) -- ported code reads NULL as OOM
    if (n > SLAB_MAX || heap_debug()) return kmalloc(n);
    uint32_t cls = (uint32_t)((n - 1) / 16);
    lock();
    struct span *s = g_partial[cls];
    if (!s && !(s = new_span(cls))) {
        unlock();
        return 0;
    }
    uint8_t *slot;
    if (s->free) {
        slot = s->free;
        s->free = *(void **)slot;
    } else {
        slot = (uint8_t *)s + SLAB_HDR + (size_t)s->bump++ * s->stride;
    }
    if (--s->nfree == 0) unlink_span(s);
    ((uint64_t *)slot)[1] = TAG_LIVE << 48 | (uint64_t)(uintptr_t)s;
    unlock();
    return slot + 16;
}

void *malloc(size_t n) { return alloc(n); }

void free(void *p) {
    if (!p) return;
    uint64_t hi;
    struct span *s = span_of(p, &hi);
    if (!s) {
        kfree(p);
        return;
    }
    if (hi == TAG_FREE) return;   // a double free
    uint8_t *slot = (uint8_t *)p - 16;
    lock();
    ((uint64_t *)slot)[1] = TAG_FREE << 48 | (uint64_t)(uintptr_t)s;
    *(void **)slot = s->free;
    s->free = slot;
    if (s->nfree++ == 0) push(s);
    // An empty span goes back to the heap unless it is its class's only
    // one: a loop that allocates and frees one object would otherwise
    // take and return 64 KiB every time.
    if (s->nfree == s->nslots && (s->next || s->prev)) {
        unlink_span(s);
        s->magic = 0;
        kfree(s);
    }
    unlock();
}

void *calloc(size_t n, size_t size) {
    if (n && size && n > (size_t)-1 / size) return 0;   // n * size would wrap
    void *p = alloc(n * size);
    if (p) memset(p, 0, n * size);   // a slot is reused, never fresh
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return alloc(n);
    if (!n) {
        free(p);
        return 0;
    }
    uint64_t hi;
    struct span *s = span_of(p, &hi);
    if (!s) return krealloc(p, n);
    size_t have = ((size_t)s->cls + 1) * 16;
    if (n <= have) return p;
    void *q = alloc(n);
    if (!q) return 0;   // C: the original block is still valid
    memcpy(q, p, have);
    free(p);
    return q;
}
