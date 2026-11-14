// lin16.c -- w16's LINEAR address space: one reservation every byte a program
// can reach through a selector lives in, and the allocator over it.
//
// Windows 3.1 in enhanced mode gave every selector a 32-bit LINEAR base, and
// a 386 program leans on that: DPMI hands out memory by linear address, and a
// Watcom Win386 program turns a 16:16 pointer into a flat one by subtracting
// selector bases. So w16 keeps one: linear LIN16_ORIGIN + n is byte n of the
// arena, a selector's linear base is where its host base sits in it, and a
// descriptor whose base was SET as a linear address (DPMI 0007h) reaches the
// arena modulo 2^32 -- through cpu16.c's mem(), which bounds every such access
// to the arena, so a 4 GiB flat selector still cannot touch host memory.
//
// THE ARENA NEVER MOVES: host pointers into it (a descriptor's base, a C
// caller's cpu16_ptr()) stay good for the program's life. It is RESERVED, not
// committed -- an anonymous mapping whose pages arrive on first touch -- so
// its size costs only what a program writes. The block list is HOST-SIDE, as
// mem16.c's local heap is, so a program writing past a block corrupts its own
// memory but never the allocator's.
#include "cpu16.h"
#include "lin16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Windows 3.1's own arena sat above 2 GiB; a program sees bases there, and a
// linear address below it (a NULL turned flat) is outside the arena.
#define LIN16_ORIGIN 0x80000000u
// The reservation: what a 1996 game asking for 8 MB, with room for its
// copies, never comes near. It is the one fixed bound because the arena cannot
// move; running out of it LOGS, naming the request.
#define LIN16_SIZE (256u << 20)
#define LIN16_ALIGN 16u

struct lin16_ext {
    uint32_t off, size;
    uint8_t used;
};

static struct {
    uint8_t *base;
    struct lin16_ext *ext;       // every extent, used and free, in address order, tiling the arena
    unsigned n, cap;
    int warned;
} g_lin;

int lin16_init(struct cpu16 *c) {
    if (!g_lin.base) {
        void *p = mmap(0, LIN16_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return -1;
        g_lin.ext = malloc(sizeof *g_lin.ext * 64);
        if (!g_lin.ext) { munmap(p, LIN16_SIZE); return -1; }
        g_lin.base = p;
        g_lin.cap = 64;
        g_lin.n = 1;
        // The first 64 KiB are never handed out: linear ORIGIN itself is no block's.
        g_lin.ext[0] = (struct lin16_ext){ 0x10000, LIN16_SIZE - 0x10000, 0 };
    }
    c->lin = g_lin.base;
    c->lin_size = LIN16_SIZE;
    c->lin_origin = LIN16_ORIGIN;
    return 0;
}

// Room for two more extents: what one allocation can add.
static int grow_list(void) {
    if (g_lin.n + 2 <= g_lin.cap) return 0;
    struct lin16_ext *g = realloc(g_lin.ext, sizeof *g * (size_t)g_lin.cap * 2);
    if (!g) return -1;
    g_lin.ext = g;
    g_lin.cap *= 2;
    return 0;
}

static void insert_at(unsigned i, struct lin16_ext e) {
    memmove(&g_lin.ext[i + 1], &g_lin.ext[i], sizeof *g_lin.ext * (g_lin.n - i));
    g_lin.ext[i] = e;
    g_lin.n++;
}

static void remove_at(unsigned i) {
    memmove(&g_lin.ext[i], &g_lin.ext[i + 1], sizeof *g_lin.ext * (g_lin.n - i - 1));
    g_lin.n--;
}

// The used extent starting at arena offset `off`, or -1.
static int find_used(uint32_t off) {
    unsigned lo = 0, hi = g_lin.n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_lin.ext[mid].off < off) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_lin.n && g_lin.ext[lo].off == off && g_lin.ext[lo].used ? (int)lo : -1;
}

static uint32_t round_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

void *lin16_alloc(uint32_t size, uint32_t align) {
    if (!g_lin.base || size == 0) return 0;
    if (align < LIN16_ALIGN) align = LIN16_ALIGN;
    if (size > LIN16_SIZE) goto full;
    size = round_up(size, LIN16_ALIGN);
    for (unsigned i = 0; i < g_lin.n; i++) {
        struct lin16_ext *e = &g_lin.ext[i];
        if (e->used) continue;
        uint32_t at = round_up(e->off, align), pad = at - e->off;
        if (pad > e->size || e->size - pad < size) continue;
        // Up to two new extents (the padding before, the rest after): room in the list first.
        if (grow_list() < 0) return 0;
        e = &g_lin.ext[i];
        uint32_t rest = e->size - pad - size;
        if (pad) {
            e->size = pad;
            insert_at(++i, (struct lin16_ext){ at, size, 1 });
        } else {
            *e = (struct lin16_ext){ at, size, 1 };
        }
        if (rest) insert_at(i + 1, (struct lin16_ext){ at + size, rest, 0 });
        uint8_t *p = g_lin.base + at;
        memset(p, 0, size);
        return p;
    }
full:
    if (!g_lin.warned) {
        g_lin.warned = 1;
        fprintf(stderr, "w16: the linear arena (%u MiB) has no room for %u bytes\n", LIN16_SIZE >> 20,
                (unsigned)size);
    }
    return 0;
}

static int owned(const void *p, int *at) {
    if (!p || !g_lin.base) return 0;
    const uint8_t *b = p;
    if (b < g_lin.base || b >= g_lin.base + LIN16_SIZE) return 0;
    *at = find_used((uint32_t)(b - g_lin.base));
    return *at >= 0;
}

uint32_t lin16_size_of(const void *p) {
    int i;
    return owned(p, &i) ? g_lin.ext[i].size : 0;
}

void lin16_free(void *p) {
    int i;
    if (!owned(p, &i)) return;
    g_lin.ext[i].used = 0;
    if ((unsigned)i + 1 < g_lin.n && !g_lin.ext[i + 1].used) {     // merge with the next free one
        g_lin.ext[i].size += g_lin.ext[i + 1].size;
        remove_at((unsigned)i + 1);
    }
    if (i > 0 && !g_lin.ext[i - 1].used) {                          // ...and the one before
        g_lin.ext[i - 1].size += g_lin.ext[i].size;
        remove_at((unsigned)i);
    }
}

// Grown in place when the extent after it is free and big enough, else moved;
// the new bytes are zero either way.
void *lin16_realloc(void *p, uint32_t size) {
    if (!p) return lin16_alloc(size, 0);
    int i;
    if (!owned(p, &i) || size == 0) return 0;
    uint32_t old = g_lin.ext[i].size;
    size = round_up(size, LIN16_ALIGN);
    if (size <= old) return p;
    unsigned nx = (unsigned)i + 1;
    if (nx < g_lin.n && !g_lin.ext[nx].used && old + g_lin.ext[nx].size >= size) {
        uint32_t more = size - old;
        memset((uint8_t *)p + old, 0, more);
        g_lin.ext[i].size = size;
        if (g_lin.ext[nx].size == more) remove_at(nx);
        else { g_lin.ext[nx].off += more; g_lin.ext[nx].size -= more; }
        return p;
    }
    uint8_t *n = lin16_alloc(size, 0);
    if (!n) return 0;
    memcpy(n, p, old);
    lin16_free(p);
    return n;
}

uint32_t lin16_free_bytes(void) {
    uint64_t t = 0;
    for (unsigned i = 0; i < g_lin.n; i++)
        if (!g_lin.ext[i].used) t += g_lin.ext[i].size;
    return (uint32_t)t;
}

uint32_t lin16_largest_free(void) {
    uint32_t m = 0;
    for (unsigned i = 0; i < g_lin.n; i++)
        if (!g_lin.ext[i].used && g_lin.ext[i].size > m) m = g_lin.ext[i].size;
    return m;
}
