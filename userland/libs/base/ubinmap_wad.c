// A DOOM WAD (IWAD or PWAD): the 12-byte header, the directory of
// 16-byte entries, and each lump spanning its data -- the Unofficial
// DOOM Specs' layout, little-endian throughout.
#include "base/ubinmap_internal.h"

#include <stdio.h>
#include <string.h>

int ubinmap_wad(struct ubinmap_ctx *c) {
    uint8_t h[12];
    if (bm_read(c, 0, h, 12) || (memcmp(h, "IWAD", 4) && memcmp(h, "PWAD", 4))) return 0;
    c->be = 0;
    int iwad = h[0] == 'I';
    int32_t lumps = (int32_t)bm_get32(c, h + 4), dir = (int32_t)bm_get32(c, h + 8);
    bm_add(c, 0, 0, 12, 1, "Header", "%s", iwad ? "IWAD" : "PWAD");
    bm_add(c, 1, 0, 4, 0, "Kind", "%s", iwad ? "IWAD (a game's data)" : "PWAD (a patch)");
    bm_add(c, 1, 4, 4, 0, "Lumps", "%d", lumps);
    bm_add(c, 1, 8, 4, 0, "Directory", "0x%X", (unsigned)dir);
    if (lumps < 0 || dir < 0) {
        bm_format(c, "DOOM %s, damaged", iwad ? "IWAD" : "PWAD");
        bm_note(c, "the header's %s is negative", lumps < 0 ? "lump count" : "directory offset");
        return 1;
    }
    // AS MANY ENTRIES AS THE FILE HOLDS, whatever the count says.
    uint64_t size = c->src->size, fit = (uint64_t)dir < size ? (size - (uint64_t)dir) / 16 : 0;
    uint64_t n = (uint64_t)lumps < fit ? (uint64_t)lumps : fit;
    if ((uint64_t)lumps > fit)
        bm_note(c, "the directory claims %d lumps; the file holds %llu", lumps, (unsigned long long)fit);
    bm_format(c, "DOOM %s, %llu lumps", iwad ? "IWAD" : "PWAD", (unsigned long long)n);
    bm_add(c, 0, (uint64_t)dir, (uint64_t)lumps * 16, 2, "Directory", "%d entries", lumps);
    if (bm_add(c, 0, 12, 0, 0, "Lumps", "%llu", (unsigned long long)n) < 0) return 1;
    int lumps_node = c->m->count - 1;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint64_t i = 0; i < n && !bm_full(c); i++) {
        uint8_t e[16];
        if (bm_read(c, (uint64_t)dir + i * 16, e, 16)) break;
        uint32_t pos = bm_get32(c, e), len = bm_get32(c, e + 4);
        char name[9];
        bm_ascii(e + 8, 8, name, sizeof name);
        int k = bm_add(c, 1, pos, len, len ? 3 + (int)(i & 1) : 0, name[0] ? name : "(no name)",
                       "%u bytes", len);
        if (k >= 0 && (c->m->nodes[k].flags & UBINMAP_CLIPPED))
            bm_note(c, "lump %s runs past the end of the file", name);
        if (k >= 0 && c->m->nodes[k].len) {
            if (c->m->nodes[k].off < lo) lo = c->m->nodes[k].off;
            if (c->m->nodes[k].off + c->m->nodes[k].len > hi) hi = c->m->nodes[k].off + c->m->nodes[k].len;
        }
    }
    // "Lumps" spans the data its lumps cover, so a click on it selects them.
    if (lo < hi) {
        c->m->nodes[lumps_node].off = lo;
        c->m->nodes[lumps_node].len = hi - lo;
    }
    return 1;
}
