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
// LOCAL: Windows' local heap lives in the DGROUP between the data and the
// stack, and a pointer into it is a plain 16-bit offset. The block list is
// kept HOST-SIDE, not in headers inside the segment, so a program writing
// past its block corrupts its own data -- as on Windows -- but never the
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

void w16_local_init(struct w16 *w) {
    const struct cpu16_desc *dg = &w->desc[cpu16_index(w->dgroup)];
    const struct ne_segment *s = &w->ne.seg[w->ne.autodata - 1];
    uint32_t start = (s->minalloc > s->length ? s->minalloc : s->length);
    start = (start + 3) & ~3u;
    uint32_t end = start + w->ne.heap;
    uint32_t stack_floor = (uint32_t)w->cpu.r[R_SP] - w->ne.stack;
    if (end > stack_floor) end = stack_floor;
    if (end > dg->limit + 1) end = dg->limit + 1;
    w->lheap.n = 0;
    if (end > start + 4) {
        w->lheap.blk[0] = (struct w16_lblock){ (uint16_t)start, (uint16_t)(end - start), 0 };
        w->lheap.n = 1;
    }
}

static int lfind(const struct w16 *w, uint16_t h) {
    for (int i = 0; i < w->lheap.n; i++)
        if (w->lheap.blk[i].used && w->lheap.blk[i].off == h) return i;
    return -1;
}

// First fit, splitting the free block it lands in. 0 when nothing fits.
static uint16_t local_alloc(struct w16 *w, uint16_t flags, uint16_t size) {
    uint16_t need = (uint16_t)((size + 3) & ~3u);
    if (need == 0) need = 4;
    for (int i = 0; i < w->lheap.n; i++) {
        struct w16_lblock *b = &w->lheap.blk[i];
        if (b->used || b->size < need) continue;
        if (b->size - need >= 8 && w->lheap.n < W16_LBLOCKS) {
            memmove(&w->lheap.blk[i + 2], &w->lheap.blk[i + 1],
                    (size_t)(w->lheap.n - i - 1) * sizeof *b);
            w->lheap.blk[i + 1] = (struct w16_lblock){ (uint16_t)(b->off + need),
                                                       (uint16_t)(b->size - need), 0 };
            w->lheap.n++;
            b->size = need;
        }
        b->used = 1;
        uint8_t *p = cpu16_ptr(&w->cpu, w->dgroup, b->off, b->size, 1);
        if (p && (flags & LMEM_ZEROINIT)) memset(p, 0, b->size);
        return b->off;
    }
    return 0;
}

static void local_free(struct w16 *w, int i) {
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
// enough), else a new block and a copy -- a moveable block's handle then
// changes, which LocalReAlloc's caller is told by the return value.
static uint16_t local_realloc(struct w16 *w, uint16_t h, uint16_t size, uint16_t flags) {
    int i = lfind(w, h);
    if (i < 0) return 0;
    uint16_t need = (uint16_t)((size + 3) & ~3u), old = w->lheap.blk[i].size;
    if (need == 0) need = 4;
    if (need <= old) return h;
    if (i + 1 < w->lheap.n && !w->lheap.blk[i + 1].used &&
        old + w->lheap.blk[i + 1].size >= need) {
        struct w16_lblock *nb = &w->lheap.blk[i + 1];
        uint16_t take = (uint16_t)(need - old);
        nb->off = (uint16_t)(nb->off + take);
        nb->size = (uint16_t)(nb->size - take);
        w->lheap.blk[i].size = need;
        if (!nb->size) {
            memmove(nb, nb + 1, (size_t)(w->lheap.n - i - 2) * sizeof *nb);
            w->lheap.n--;
        }
        uint8_t *p = cpu16_ptr(&w->cpu, w->dgroup, h, need, 1);
        if (p && (flags & LMEM_ZEROINIT)) memset(p + old, 0, need - old);
        return h;
    }
    uint16_t nh = local_alloc(w, flags, size);
    if (!nh) return 0;
    uint8_t *dst = cpu16_ptr(&w->cpu, w->dgroup, nh, old, 1);
    const uint8_t *src = cpu16_ptr(&w->cpu, w->dgroup, h, old, 0);
    if (dst && src) memmove(dst, src, old);
    local_free(w, lfind(w, h));
    return nh;
}

static uint16_t local_free_bytes(const struct w16 *w) {
    unsigned n = 0;
    for (int i = 0; i < w->lheap.n; i++)
        if (!w->lheap.blk[i].used) n += w->lheap.blk[i].size;
    return (uint16_t)(n > 0xFFFF ? 0xFFFF : n);
}

// --- KERNEL's memory exports ------------------------------------------------

static void set32(struct cpu16 *c, uint32_t v) {
    c->r[R_AX] = (uint16_t)v;
    c->r[R_DX] = (uint16_t)(v >> 16);
}

int w16_kernel_mem(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    switch (ordinal) {
    case 5: {                                    // LocalAlloc(flags, bytes)
        uint16_t flags = w16_arg16(a), size = w16_arg16(a);
        c->r[R_AX] = local_alloc(w, flags, size);
        return 4;
    }
    case 6: {                                    // LocalReAlloc(h, bytes, flags)
        uint16_t h = w16_arg16(a), size = w16_arg16(a), flags = w16_arg16(a);
        c->r[R_AX] = local_realloc(w, h, size, flags);
        return 6;
    }
    case 7: {                                    // LocalFree(h): 0 on success
        uint16_t h = w16_arg16(a);
        int i = lfind(w, h);
        if (i >= 0) local_free(w, i);
        c->r[R_AX] = i >= 0 ? 0 : h;
        return 2;
    }
    case 8:                                      // LocalLock(h): the handle is the pointer
        c->r[R_AX] = w16_arg16(a);
        return 2;
    case 9:                                      // LocalUnlock(h)
        w16_arg16(a);
        c->r[R_AX] = 0;
        return 2;
    case 10: {                                   // LocalSize(h)
        int i = lfind(w, w16_arg16(a));
        c->r[R_AX] = i >= 0 ? w->lheap.blk[i].size : 0;
        return 2;
    }
    case 13:                                     // LocalCompact(min): the largest free
        w16_arg16(a);
        c->r[R_AX] = local_free_bytes(w);
        return 2;
    case 15: {                                   // GlobalAlloc(flags, dwBytes)
        w16_arg16(a);
        c->r[R_AX] = w16_global_alloc(w, w16_arg32(a), 0);
        return 6;
    }
    case 16: {                                   // GlobalReAlloc(h, dwBytes, flags)
        uint16_t h = w16_arg16(a);
        uint32_t size = w16_arg32(a);
        uint16_t flags = w16_arg16(a);
        c->r[R_AX] = global_realloc(w, h, size, flags);
        return 8;
    }
    case 17: {                                   // GlobalFree(h): 0 on success
        uint16_t h = w16_arg16(a);
        int ok = global_desc(w, h) != 0;
        w16_global_free(w, h);
        c->r[R_AX] = ok ? 0 : h;
        return 2;
    }
    case 18: {                                   // GlobalLock(h) -> sel:0
        uint16_t h = w16_arg16(a);
        set32(c, global_desc(w, h) ? (uint32_t)h << 16 : 0);
        return 2;
    }
    case 19:                                     // GlobalUnlock(h)
        w16_arg16(a);
        c->r[R_AX] = 0;
        return 2;
    case 20: {                                   // GlobalSize(h)
        const struct cpu16_desc *d = global_desc(w, w16_arg16(a));
        set32(c, d ? d->limit + 1 : 0);
        return 2;
    }
    case 21: {                                   // GlobalHandle(sel) -> handle in AX, sel in DX
        uint16_t s = w16_arg16(a);
        set32(c, global_desc(w, s) ? (uint32_t)s << 16 | s : 0);
        return 2;
    }
    case 22:                                     // GlobalFlags(h)
        w16_arg16(a);
        c->r[R_AX] = 0;
        return 2;
    case 23: {                                   // LockSegment(seg): -1 is DS
        uint16_t s = w16_arg16(a);
        c->r[R_AX] = s == 0xFFFF ? c->sreg[S_DS] : s;
        return 2;
    }
    case 24: {                                   // UnlockSegment(seg)
        uint16_t s = w16_arg16(a);
        c->r[R_AX] = s == 0xFFFF ? c->sreg[S_DS] : s;
        return 2;
    }
    case 25:                                     // GlobalCompact(dwMinFree): plenty
        w16_arg32(a);
        set32(c, 0x00100000);
        return 4;
    default:
        return -2;
    }
}
