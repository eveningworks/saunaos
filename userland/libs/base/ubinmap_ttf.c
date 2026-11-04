// TrueType and OpenType: the offset table, its table directory and each
// table spanning its data; a collection (.ttc) lists each font's. The
// OpenType spec's layout, big-endian throughout.
#include "base/ubinmap_internal.h"

#include <stdio.h>
#include <string.h>

#define FONTS_MAX 64

static int known_version(uint32_t v) {
    return v == 0x00010000u || v == 0x4F54544Fu /* OTTO */ || v == 0x74727565u /* true */;
}

// One font's offset table at `at`. 0 when it is not one.
static int font(struct ubinmap_ctx *c, uint64_t at, int depth, int tint) {
    uint8_t h[12];
    if (bm_read(c, at, h, 12) || !known_version(bm_get32(c, h))) return 0;
    uint32_t ver = bm_get32(c, h);
    unsigned n = bm_get16(c, h + 4);
    bm_add(c, depth, at, 12, tint, "Offset table", "%u tables", n);
    bm_add(c, depth + 1, at, 4, 0, "Version", "%s", ver == 0x4F54544Fu ? "OTTO (CFF outlines)"
                                                  : ver == 0x74727565u ? "true (Apple)" : "1.0 (TrueType outlines)");
    bm_add(c, depth + 1, at + 4, 2, 0, "Tables", "%u", n);
    bm_add(c, depth + 1, at + 6, 2, 0, "Search range", "%u", bm_get16(c, h + 6));
    bm_add(c, depth + 1, at + 8, 2, 0, "Entry selector", "%u", bm_get16(c, h + 8));
    bm_add(c, depth + 1, at + 10, 2, 0, "Range shift", "%u", bm_get16(c, h + 10));
    uint64_t dir = at + 12, fit = dir < c->src->size ? (c->src->size - dir) / 16 : 0;
    if (n > fit) bm_note(c, "the table directory claims %u tables; the file holds %llu", n, (unsigned long long)fit);
    unsigned listed = n < fit ? n : (unsigned)fit;
    bm_add(c, depth, dir, (uint64_t)n * 16, tint ? tint : 2, "Table directory", "%u entries", n);
    for (unsigned i = 0; i < listed && !bm_full(c); i++) {
        uint8_t r[16];
        if (bm_read(c, dir + (uint64_t)i * 16, r, 16)) break;
        char tag[5];
        bm_ascii(r, 4, tag, sizeof tag);
        bm_add(c, depth + 1, dir + (uint64_t)i * 16, 16, 0, tag, "0x%X", (unsigned)bm_get32(c, r + 8));
    }
    if (bm_add(c, depth, dir + (uint64_t)n * 16, 0, 0, "Tables", "%u", listed) < 0) return 1;
    int tables = c->m->count - 1;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (unsigned i = 0; i < listed && !bm_full(c); i++) {
        uint8_t r[16];
        if (bm_read(c, dir + (uint64_t)i * 16, r, 16)) break;
        char tag[5];
        bm_ascii(r, 4, tag, sizeof tag);
        uint32_t off = bm_get32(c, r + 8), len = bm_get32(c, r + 12);
        int k = bm_add(c, depth + 1, off, len, 3 + (int)(i & 1), tag, "%u bytes", len);
        if (k < 0) break;
        if (c->m->nodes[k].flags & UBINMAP_CLIPPED) bm_note(c, "table %s runs past the end of the file", tag);
        bm_add(c, depth + 2, dir + (uint64_t)i * 16 + 4, 4, 0, "Checksum", "0x%08X", (unsigned)bm_get32(c, r + 4));
        const struct ubinmap_node *t = &c->m->nodes[k];
        if (t->len) {
            if (t->off < lo) lo = t->off;
            if (t->off + t->len > hi) hi = t->off + t->len;
        }
    }
    if (lo < hi) {
        c->m->nodes[tables].off = lo;
        c->m->nodes[tables].len = hi - lo;
    }
    return 1;
}

int ubinmap_ttf(struct ubinmap_ctx *c) {
    uint8_t h[12];
    if (bm_read(c, 0, h, 12)) return 0;
    c->be = 1;
    uint32_t v = bm_get32(c, h);
    if (known_version(v)) {
        unsigned n = bm_get16(c, h + 4);
        if (n == 0 || n > 512) return 0;   // the field a text file starting "true" would fail
        bm_format(c, "%s font, %u tables", v == 0x4F54544Fu ? "OpenType (CFF)" : "TrueType", n);
        return font(c, 0, 0, 1);
    }
    if (memcmp(h, "ttcf", 4)) return 0;
    uint32_t count = bm_get32(c, h + 8);
    uint64_t fit = c->src->size > 12 ? (c->src->size - 12) / 4 : 0;
    bm_format(c, "TrueType collection, %u fonts", (unsigned)count);
    bm_add(c, 0, 0, 12 + (uint64_t)count * 4, 1, "Collection header", "%u fonts", (unsigned)count);
    bm_add(c, 1, 0, 4, 0, "Tag", "ttcf");
    bm_add(c, 1, 4, 4, 0, "Version", "%u.%u", bm_get16(c, h + 4), bm_get16(c, h + 6));
    bm_add(c, 1, 8, 4, 0, "Fonts", "%u", (unsigned)count);
    if (count > fit) bm_note(c, "the collection claims %u fonts; the file holds %llu offsets", (unsigned)count,
                             (unsigned long long)fit);
    for (uint32_t i = 0; i < count && i < fit && i < FONTS_MAX && !bm_full(c); i++) {
        uint32_t off;
        if (bm_u32(c, 12 + (uint64_t)i * 4, &off)) break;
        char label[24];
        snprintf(label, sizeof label, "Font %u", (unsigned)i + 1);
        if (bm_add(c, 0, off, 12, 2 + (int)(i & 1), label, "0x%X", (unsigned)off) < 0) break;
        if (!font(c, off, 1, 0)) bm_note(c, "font %u's offset table is not one", (unsigned)i + 1);
    }
    return 1;
}
