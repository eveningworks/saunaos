// mem16.c -- KERNEL's memory: the global heap (a selector a block) and
// the local heap (blocks inside the program's own DGROUP).
//
// GLOBAL: a handle IS its selector, MOVEABLE or not. Windows 3.1's
// moveable handles differ from their selector by a low bit and a program
// is meant to GlobalLock first; one that uses the handle as a selector
// works here, which is lenient and costs nothing. A block is at most
// 64 KiB: a huge (tiled) block is refused, as the selector table has no
// tiling.
//
// LOCAL: Windows' local heap lives in the DGROUP after the stack, and a
// pointer into it is a plain 16-bit offset. The block list is kept
// HOST-SIDE, not in headers inside the segment, so a program writing past
// its block corrupts its own data -- as on Windows -- but never the
// allocator's.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GMEM_MODIFY   0x0080
#define GMEM_ZEROINIT 0x0040
#define LMEM_ZEROINIT 0x0040
#define GLOBAL_OWNER  0xFFFF       // a descriptor's owner: this selector is a global block

// --- the global heap ---------------------------------------------------------

static struct cpu16_desc *global_desc(struct w16 *w, uint16_t h) {
    unsigned i = cpu16_index(h);
    if (i == 0 || i >= CPU16_MAX_DESC || (h & 7) != 7) return 0;
    struct cpu16_desc *d = &w->desc[i];
    return d->kind == D_DATA && d->owner == GLOBAL_OWNER ? d : 0;
}

uint16_t w16_global_alloc(struct w16 *w, uint32_t size, const void *init) {
    if (size > 0x10000) return 0;
    uint32_t cap = size ? size : 1;
    uint8_t *mem = calloc(1, cap);
    if (!mem) return 0;
    if (init) memcpy(mem, init, size);
    uint16_t sel = cpu16_alloc(&w->cpu, D_DATA, mem, cap - 1, GLOBAL_OWNER);
    if (!sel) free(mem);
    return sel;
}

void w16_global_free(struct w16 *w, uint16_t h) {
    struct cpu16_desc *d = global_desc(w, h);
    if (!d) return;
    free(d->base);
    memset(d, 0, sizeof *d);
}

static uint16_t global_realloc(struct w16 *w, uint16_t h, uint32_t size, uint16_t flags) {
    struct cpu16_desc *d = global_desc(w, h);
    if (!d) return 0;
    if (flags & GMEM_MODIFY) return h;
    if (size > 0x10000) return 0;
    uint32_t cap = size ? size : 1, old = d->limit + 1;
    uint8_t *mem = realloc(d->base, cap);
    if (!mem) return 0;
    if (cap > old) memset(mem + old, 0, cap - old);
    d->base = mem;
    d->limit = cap - 1;
    return h;
}

// --- the local heap -----------------------------------------------------------
//
// IT GROWS, as Windows grew a DGROUP whose heap ran out: the heap sits
// after the stack at the segment's end, and a request that fits nowhere
// raises the segment's limit, toward 64 KiB. A program's NE header asks
// for a token heap (Notepad's is 2 KiB) and counts on that.
//
// A FIXED block's handle is its offset; a MOVEABLE one's is the offset of
// a four-byte entry -- the block's offset, flags, a lock count -- in a
// table the heap holds, which is how Windows' were: the handle survives
// the block moving (LocalReAlloc), LocalLock reads the offset out of the
// entry, and a program that reads it itself finds the same. A fixed
// block is 4-aligned and an entry is not (it ends in 2), so the two are
// told apart by the handle alone.

#define LMEM_MOVEABLE    0x0002
#define LMEM_MODIFY      0x0080
#define HANDLES_PER_CHUNK 16

static struct cpu16_desc *dgroup_desc(struct w16 *w) { return &w->desc[cpu16_index(w->dgroup)]; }

static int lblock_add(struct w16 *w, int at, uint16_t off, uint16_t size, uint8_t used) {
    if (w->lheap.n == w->lheap.cap) {
        int cap = w->lheap.cap ? w->lheap.cap * 2 : 64;
        struct w16_lblock *g = realloc(w->lheap.blk, sizeof *g * (size_t)cap);
        if (!g) return -1;
        w->lheap.blk = g;
        w->lheap.cap = cap;
    }
    memmove(&w->lheap.blk[at + 1], &w->lheap.blk[at], (size_t)(w->lheap.n - at) * sizeof *w->lheap.blk);
    w->lheap.blk[at] = (struct w16_lblock){ off, size, used };
    w->lheap.n++;
    return 0;
}

void w16_local_init(struct w16 *w) {
    const struct cpu16_desc *dg = dgroup_desc(w);
    const struct ne_segment *s = &w->ne.seg[w->ne.autodata - 1];
    uint32_t data = (s->minalloc > s->length ? s->minalloc : s->length);
    uint32_t start = data > (uint32_t)w->cpu.r[R_SP] ? data : w->cpu.r[R_SP];
    start = (start + 3) & ~3u;
    uint32_t end = dg->limit + 1;
    w->lheap.n = 0;
    w->lheap.end = start;
    if (end > start + 4 && lblock_add(w, 0, (uint16_t)start, (uint16_t)(end - start), 0) == 0)
        w->lheap.end = end;
}

// The heap's end raised to fit `need` more bytes: the segment's limit
// with it, never past 64 KiB. 0 when there is no room left to grow.
static int grow(struct w16 *w, uint32_t need) {
    uint32_t end = w->lheap.end, want = end + ((need + 0x3FF) & ~0x3FFu);
    if (want > 0x10000) want = 0x10000;
    if (want <= end + 4) return 0;
    struct cpu16_desc *dg = dgroup_desc(w);
    memset(dg->base + end, 0, want - end);       // the 64 KiB buffer was there all along
    dg->limit = want - 1;
    struct w16_lblock *last = w->lheap.n ? &w->lheap.blk[w->lheap.n - 1] : 0;
    if (last && !last->used && last->off + last->size == end) last->size = (uint16_t)(last->size + (want - end));
    else if (lblock_add(w, w->lheap.n, (uint16_t)end, (uint16_t)(want - end), 0) < 0) return 0;
    w->lheap.end = want;
    return 1;
}

static int lfind(const struct w16 *w, uint16_t off) {
    for (int i = 0; i < w->lheap.n; i++)
        if (w->lheap.blk[i].used && w->lheap.blk[i].off == off) return i;
    return -1;
}

// First fit, splitting the free block it lands in, growing the heap when
// nothing fits. The block's offset, or 0.
static uint16_t block_alloc(struct w16 *w, uint16_t flags, uint32_t size) {
    uint32_t need = (size + 3) & ~3u;
    if (need == 0) need = 4;
    if (need > 0xFFF0) return 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < w->lheap.n; i++) {
            struct w16_lblock *b = &w->lheap.blk[i];
            if (b->used || b->size < need) continue;
            if (b->size - need >= 8) {
                uint16_t off = (uint16_t)(b->off + need), rest = (uint16_t)(b->size - need);
                b->size = (uint16_t)need;
                if (lblock_add(w, i + 1, off, rest, 0) < 0) b->size = (uint16_t)(b->size + rest);
                b = &w->lheap.blk[i];
            }
            b->used = 1;
            uint8_t *p = cpu16_ptr(&w->cpu, w->dgroup, b->off, b->size, 1);
            if (p && (flags & LMEM_ZEROINIT)) memset(p, 0, b->size);
            return b->off;
        }
        if (pass == 0 && !grow(w, need)) break;
    }
    return 0;
}

static void block_free(struct w16 *w, int i) {
    w->lheap.blk[i].used = 0;
    // Coalesce with the free neighbours.
    if (i + 1 < w->lheap.n && !w->lheap.blk[i + 1].used) {
        w->lheap.blk[i].size = (uint16_t)(w->lheap.blk[i].size + w->lheap.blk[i + 1].size);
        memmove(&w->lheap.blk[i + 1], &w->lheap.blk[i + 2],
                (size_t)(w->lheap.n - i - 2) * sizeof w->lheap.blk[0]);
        w->lheap.n--;
    }
    if (i > 0 && !w->lheap.blk[i - 1].used) {
        w->lheap.blk[i - 1].size = (uint16_t)(w->lheap.blk[i - 1].size + w->lheap.blk[i].size);
        memmove(&w->lheap.blk[i], &w->lheap.blk[i + 1],
                (size_t)(w->lheap.n - i - 1) * sizeof w->lheap.blk[0]);
        w->lheap.n--;
    }
}

// In place when it fits (shrinking, or the next block is free and big
// enough), else a new block and a copy. The block's offset, or 0.
static uint16_t block_realloc(struct w16 *w, uint16_t off, uint32_t size, uint16_t flags) {
    int i = lfind(w, off);
    if (i < 0) return 0;
    uint32_t need = (size + 3) & ~3u, old = w->lheap.blk[i].size;
    if (need == 0) need = 4;
    if (need > 0xFFF0) return 0;
    if (need <= old) return off;
    if (i + 1 < w->lheap.n && !w->lheap.blk[i + 1].used && old + w->lheap.blk[i + 1].size >= need) {
        struct w16_lblock *nb = &w->lheap.blk[i + 1];
        uint16_t take = (uint16_t)(need - old);
        nb->off = (uint16_t)(nb->off + take);
        nb->size = (uint16_t)(nb->size - take);
        w->lheap.blk[i].size = (uint16_t)need;
        if (!nb->size) {
            memmove(nb, nb + 1, (size_t)(w->lheap.n - i - 2) * sizeof *nb);
            w->lheap.n--;
        }
        uint8_t *p = cpu16_ptr(&w->cpu, w->dgroup, off, need, 1);
        if (p && (flags & LMEM_ZEROINIT)) memset(p + old, 0, need - old);
        return off;
    }
    uint16_t moved = block_alloc(w, flags, need);
    if (!moved) return 0;
    uint8_t *dst = cpu16_ptr(&w->cpu, w->dgroup, moved, old, 1);
    const uint8_t *src = cpu16_ptr(&w->cpu, w->dgroup, off, old, 0);
    if (dst && src) memmove(dst, src, old);
    block_free(w, lfind(w, off));
    return moved;
}

// --- moveable handles ----------------------------------------------------------

static int is_moveable(uint16_t h) { return (h & 3) == 2; }

// A handle's entry in DGROUP: the block's offset, flags, lock count.
static uint8_t *entry(struct w16 *w, uint16_t h) {
    for (int i = 0; i < w->lheap.nh; i++)
        if (w->lheap.h[i] == h) return cpu16_ptr(&w->cpu, w->dgroup, h, 4, 1);
    return 0;
}

static uint16_t entry_off(struct w16 *w, uint16_t h) {
    uint8_t *e = entry(w, h);
    return e ? (uint16_t)(e[0] | e[1] << 8) : 0;
}

static void set_entry_off(struct w16 *w, uint16_t h, uint16_t off) {
    uint8_t *e = entry(w, h);
    if (e) { e[0] = (uint8_t)off; e[1] = (uint8_t)(off >> 8); }
}

// A free entry, a new chunk of them taken from the heap when none is left.
static uint16_t new_handle(struct w16 *w) {
    for (int i = 0; i < w->lheap.nh; i++) {
        uint8_t *e = cpu16_ptr(&w->cpu, w->dgroup, w->lheap.h[i], 4, 1);
        if (e && e[2] == 0xFF) { memset(e, 0, 4); return w->lheap.h[i]; }
    }
    uint16_t chunk = block_alloc(w, LMEM_ZEROINIT, 4 * HANDLES_PER_CHUNK + 4);
    if (!chunk) return 0;
    uint16_t *g = realloc(w->lheap.h, sizeof *g * (size_t)(w->lheap.nh + HANDLES_PER_CHUNK));
    if (!g) { block_free(w, lfind(w, chunk)); return 0; }
    w->lheap.h = g;
    for (int i = 0; i < HANDLES_PER_CHUNK; i++) {
        uint16_t h = (uint16_t)(chunk + 2 + 4 * i);
        uint8_t *e = cpu16_ptr(&w->cpu, w->dgroup, h, 4, 1);
        if (e) e[2] = 0xFF;                      // free
        w->lheap.h[w->lheap.nh++] = h;
    }
    uint8_t *e = cpu16_ptr(&w->cpu, w->dgroup, (uint16_t)(chunk + 2), 4, 1);
    if (e) memset(e, 0, 4);
    return (uint16_t)(chunk + 2);
}

// --- the local heap's calls, for the program and for w16 -----------------------------

static uint16_t local_alloc(struct w16 *w, uint16_t flags, uint16_t size) {
    if (!(flags & LMEM_MOVEABLE)) return block_alloc(w, flags, size);
    uint16_t h = new_handle(w);
    if (!h) return 0;
    if (size) {
        uint16_t off = block_alloc(w, flags, size);
        if (!off) { uint8_t *e = entry(w, h); if (e) e[2] = 0xFF; return 0; }
        set_entry_off(w, h, off);
    }
    return h;
}

static uint16_t local_realloc(struct w16 *w, uint16_t h, uint16_t size, uint16_t flags) {
    if (flags & LMEM_MODIFY) return h;
    if (!is_moveable(h) || !entry(w, h)) return block_realloc(w, h, size, flags);
    uint16_t off = entry_off(w, h);
    uint16_t moved = off ? block_realloc(w, off, size, flags) : block_alloc(w, flags, size);
    if (!moved) return 0;
    set_entry_off(w, h, moved);
    return h;                                    // a moveable handle stays as it was
}

static int local_free(struct w16 *w, uint16_t h) {
    if (is_moveable(h) && entry(w, h)) {
        uint16_t off = entry_off(w, h);
        int i = off ? lfind(w, off) : -1;
        if (i >= 0) block_free(w, i);
        uint8_t *e = entry(w, h);
        memset(e, 0, 4);
        e[2] = 0xFF;
        return 0;
    }
    int i = lfind(w, h);
    if (i < 0) return -1;
    block_free(w, i);
    return 0;
}

// A handle's block: its offset (0 for none) and size.
static uint16_t local_deref(struct w16 *w, uint16_t h, unsigned *size) {
    uint16_t off = is_moveable(h) && entry(w, h) ? entry_off(w, h) : h;
    int i = off ? lfind(w, off) : -1;
    *size = i >= 0 ? w->lheap.blk[i].size : 0;
    return i >= 0 ? off : 0;
}

static uint16_t local_free_bytes(const struct w16 *w) {
    unsigned n = 0x10000 - w->lheap.end;         // what growing would still give
    for (int i = 0; i < w->lheap.n; i++)
        if (!w->lheap.blk[i].used) n += w->lheap.blk[i].size;
    return (uint16_t)(n > 0xFFFF ? 0xFFFF : n);
}

uint16_t w16_local_alloc(struct w16 *w, uint16_t flags, uint16_t size) {
    return local_alloc(w, flags, size);
}

uint16_t w16_local_realloc(struct w16 *w, uint16_t h, uint16_t size, uint16_t flags) {
    return local_realloc(w, h, size, flags);
}

void w16_local_free(struct w16 *w, uint16_t h) { local_free(w, h); }

uint8_t *w16_local_ptr(struct w16 *w, uint16_t h, unsigned *size) {
    uint16_t off = local_deref(w, h, size);
    if (!off) return 0;
    uint8_t *p = cpu16_ptr(&w->cpu, w->dgroup, off, *size, 1);
    if (!p) { w->cpu.fault = 0; *size = 0; }     // a block past the segment: no text, not a fault
    return p;
}

// --- KERNEL's memory exports ------------------------------------------------

static void set32(struct cpu16 *c, uint32_t v) {
    c->r[R_AX] = (uint16_t)v;
    c->r[R_DX] = (uint16_t)(v >> 16);
}

// LocalAlloc(flags, bytes)
static int api_LocalAlloc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t flags = w16_arg16(a), size = w16_arg16(a);
    c->r[R_AX] = local_alloc(w, flags, size);
    return 4;
}

// LocalReAlloc(h, bytes, flags)
static int api_LocalReAlloc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a), size = w16_arg16(a), flags = w16_arg16(a);
    c->r[R_AX] = local_realloc(w, h, size, flags);
    return 6;
}

// LocalFree(h): 0 on success
static int api_LocalFree(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    c->r[R_AX] = local_free(w, h) == 0 ? 0 : h;
    return 2;
}

// LocalLock(h): the block's offset -- a fixed handle is it already
static int api_LocalLock(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    unsigned size;
    uint16_t h = w16_arg16(a);
    c->r[R_AX] = local_deref(w, h, &size);
    uint8_t *e = is_moveable(h) ? entry(w, h) : 0;
    if (e && e[3] < 0xFF) e[3]++;
    return 2;
}

// LocalUnlock(h): whether it is still locked
static int api_LocalUnlock(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint8_t *e = is_moveable(h) ? entry(w, h) : 0;
    if (e && e[3]) e[3]--;
    c->r[R_AX] = e ? e[3] != 0 : 0;
    return 2;
}

// LocalSize(h)
static int api_LocalSize(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    unsigned size;
    local_deref(w, w16_arg16(a), &size);
    c->r[R_AX] = (uint16_t)size;
    return 2;
}

// LocalHandle(wMem): the handle of the block at offset wMem
static int api_LocalHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t off = w16_arg16(a);
    c->r[R_AX] = lfind(w, off) >= 0 ? off : 0;
    for (int i = 0; i < w->lheap.nh; i++)
        if (entry_off(w, w->lheap.h[i]) == off) { c->r[R_AX] = w->lheap.h[i]; break; }
    return 2;
}

// LocalFlags(h): the lock count in the low byte
static int api_LocalFlags(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint8_t *e = is_moveable(h) ? entry(w, h) : 0;
    c->r[R_AX] = e ? e[3] : 0;
    return 2;
}

// LocalCompact(min): the largest free
static int api_LocalCompact(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = local_free_bytes(w);
    return 2;
}

// GlobalAlloc(flags, dwBytes)
static int api_GlobalAlloc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = w16_global_alloc(w, w16_arg32(a), 0);
    return 6;
}

// GlobalReAlloc(h, dwBytes, flags)
static int api_GlobalReAlloc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t size = w16_arg32(a);
    uint16_t flags = w16_arg16(a);
    c->r[R_AX] = global_realloc(w, h, size, flags);
    return 8;
}

// GlobalFree(h): 0 on success
static int api_GlobalFree(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    int ok = global_desc(w, h) != 0;
    w16_global_free(w, h);
    c->r[R_AX] = ok ? 0 : h;
    return 2;
}

// GlobalLock(h) -> sel:0
static int api_GlobalLock(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    set32(c, global_desc(w, h) ? (uint32_t)h << 16 : 0);
    return 2;
}

// GlobalUnlock(h)
static int api_GlobalUnlock(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// GlobalSize(h)
static int api_GlobalSize(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    const struct cpu16_desc *d = global_desc(w, w16_arg16(a));
    set32(c, d ? d->limit + 1 : 0);
    return 2;
}

// GlobalHandle(sel) -> handle in AX, sel in DX
static int api_GlobalHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t s = w16_arg16(a);
    set32(c, global_desc(w, s) ? (uint32_t)s << 16 | s : 0);
    return 2;
}

// GlobalFlags(h)
static int api_GlobalFlags(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// LockSegment(seg): -1 is DS
static int api_LockSegment(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t s = w16_arg16(a);
    c->r[R_AX] = s == 0xFFFF ? c->sreg[S_DS] : s;
    return 2;
}

// UnlockSegment(seg)
static int api_UnlockSegment(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t s = w16_arg16(a);
    c->r[R_AX] = s == 0xFFFF ? c->sreg[S_DS] : s;
    return 2;
}

// GlobalCompact(dwMinFree): plenty
static int api_GlobalCompact(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg32(a);
    set32(c, 0x00100000);
    return 4;
}

const struct w16_api g_kernel_mem_api[] = {
    { 5, api_LocalAlloc },
    { 6, api_LocalReAlloc },
    { 7, api_LocalFree },
    { 8, api_LocalLock },
    { 9, api_LocalUnlock },
    { 10, api_LocalSize },
    { 11, api_LocalHandle },
    { 12, api_LocalFlags },
    { 13, api_LocalCompact },
    { 15, api_GlobalAlloc },
    { 16, api_GlobalReAlloc },
    { 17, api_GlobalFree },
    { 18, api_GlobalLock },
    { 19, api_GlobalUnlock },
    { 20, api_GlobalSize },
    { 21, api_GlobalHandle },
    { 22, api_GlobalFlags },
    { 23, api_LockSegment },
    { 24, api_UnlockSegment },
    { 25, api_GlobalCompact },
};
const unsigned g_kernel_mem_api_n = sizeof g_kernel_mem_api / sizeof g_kernel_mem_api[0];

int w16_kernel_mem(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_api_call(g_kernel_mem_api, sizeof g_kernel_mem_api / sizeof g_kernel_mem_api[0], w, ordinal, a);
}
