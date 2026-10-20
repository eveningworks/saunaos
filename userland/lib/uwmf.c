// uwmf.c -- Windows metafile records (uwmf.h). A record's size is checked
// against the bytes left before anything in it is read, and each field
// against the record.
#include "lib/uwmf.h"

#include <stdlib.h>
#include <string.h>

#define PLACEABLE_KEY 0x9AC6CDD7u
#define ETO_OPAQUE    0x0002
#define ETO_CLIPPED   0x0004

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }

int uwmf_open(struct uwmf *m, const void *data, size_t n) {
    memset(m, 0, sizeof *m);
    const uint8_t *p = data;
    if (n >= 22 && rd32(p) == PLACEABLE_KEY) {
        m->placeable = 1;
        for (int i = 0; i < 4; i++) m->bounds[i] = (int16_t)rd16(p + 6 + 2 * i);
        m->inch = rd16(p + 14);
        p += 22;
        n -= 22;
    }
    // METAHEADER: type 1 (memory) or 2 (disk), a 9-word header, version 1.0 or 3.0
    if (n < 18 || (rd16(p) != 1 && rd16(p) != 2) || rd16(p + 2) != 9 || (rd16(p + 4) != 0x100 && rd16(p + 4) != 0x300))
        return -1;
    m->nobjects = rd16(p + 10);
    m->used = calloc(m->nobjects ? m->nobjects : 1, 1);
    if (!m->used) return -1;
    m->p = p;
    m->n = n;
    m->at = 18;
    return 0;
}

void uwmf_close(struct uwmf *m) {
    free(m->used);
    free(m->scratch);
    memset(m, 0, sizeof *m);
}

// `bytes` of room in the record's scratch, aligned; NULL when out of memory.
static void *scratch(struct uwmf *m, size_t bytes) {
    if (bytes > m->scratch_cap) {
        void *g = realloc(m->scratch, bytes);
        if (!g) return 0;
        m->scratch = g;
        m->scratch_cap = bytes;
    }
    return m->scratch;
}

// The lowest free slot, taken; -1 when the table is full (as GDI then failed the record).
static int take_slot(struct uwmf *m) {
    for (unsigned i = 0; i < m->nobjects; i++)
        if (!m->used[i]) { m->used[i] = 1; return (int)i; }
    return -1;
}

int uwmf_next(struct uwmf *m, struct uwmf_rec *r) {
    memset(r, 0, sizeof *r);
    r->slot = -1;
    if (m->at == m->n) return 0;
    if (m->n - m->at < 6) return -1;
    const uint8_t *rec = m->p + m->at;
    uint32_t size = rd32(rec);                    // in words, its own six bytes included
    unsigned func = rd16(rec + 4);
    if (func == 0) return 0;                     // META_EOF
    if (size < 3 || size > (m->n - m->at) / 2) return -1;
    m->at += (size_t)size * 2;
    const uint8_t *a = rec + 6;                  // its arguments: nw words
    uint32_t nw = size - 3;
    r->func = func;
    r->words = a;
    r->nwords = nw;
#define W(i) ((int16_t)rd16(a + 2 * (i)))
#define NEED(k) do { if (nw < (uint32_t)(k)) return -1; } while (0)
    switch (func) { // dispatch-ok: bounded by MS-WMF's record types, each laid out its own way
    // one value, perhaps a reserved word after it: the value is the FIRST word
    case UWMF_SETBKMODE: case UWMF_SETMAPMODE: case UWMF_SETROP2: case UWMF_SETRELABS:
    case UWMF_SETPOLYFILLMODE: case UWMF_SETSTRETCHBLTMODE: case UWMF_SETTEXTALIGN:
    case UWMF_SETTEXTCHAREXTRA: case UWMF_RESTOREDC: case UWMF_SELECTPALETTE: case UWMF_SELECTCLIPREGION:
        NEED(1);
        r->v[0] = W(0);
        r->nv = 1;
        return 1;
    case UWMF_SELECTOBJECT:
    case UWMF_DELETEOBJECT: {
        NEED(1);
        unsigned i = rd16(a);
        if (i >= m->nobjects) return -1;
        r->slot = (int)i;
        if (func == UWMF_DELETEOBJECT) m->used[i] = 0;
        return 1;
    }
    case UWMF_SETBKCOLOR:
    case UWMF_SETTEXTCOLOR:
        NEED(2);
        r->color = rd32(a);
        return 1;
    case UWMF_SETPIXEL:
    case UWMF_FLOODFILL:                         // ColorRef, Y, X
        NEED(4);
        r->color = rd32(a);
        r->v[0] = W(3);
        r->v[1] = W(2);
        r->nv = 2;
        return 1;
    case UWMF_PATBLT:                            // Rop, Height, Width, Y, X
        NEED(6);
        r->rop = rd32(a);
        r->v[0] = W(5); r->v[1] = W(4); r->v[2] = W(3); r->v[3] = W(2);
        r->nv = 4;
        return 1;
    case UWMF_TEXTOUT: {                         // Length, the string padded to a word, Y, X
        NEED(1);
        unsigned len = rd16(a), sw = (len + 1) / 2;
        NEED(1 + sw + 2);
        r->text = (const char *)a + 2;
        r->ntext = (int)len;
        r->v[0] = W(1 + sw + 1);
        r->v[1] = W(1 + sw);
        r->nv = 2;
        return 1;
    }
    case UWMF_EXTTEXTOUT: {                      // Y, X, Length, Options, [Rect], String, [Dx]
        NEED(4);
        unsigned len = rd16(a + 4), opts = rd16(a + 6), at = 4;
        r->v[0] = W(1);
        r->v[1] = W(0);
        r->v[2] = (int16_t)opts;
        r->nv = 3;
        if (opts & (ETO_OPAQUE | ETO_CLIPPED)) {
            NEED(8);
            for (int i = 0; i < 4; i++) r->rect[i] = W(4 + i);
            at = 8;
        }
        unsigned sw = (len + 1) / 2;
        NEED(at + sw);
        r->text = (const char *)a + 2 * at;
        r->ntext = (int)len;
        if (nw >= at + sw + len && len) {
            int16_t *dx = scratch(m, (size_t)len * 2);
            if (!dx) return -1;
            for (unsigned i = 0; i < len; i++) dx[i] = W(at + sw + i);
            r->dx = dx;
        }
        return 1;
    }
    case UWMF_POLYGON:
    case UWMF_POLYLINE: {                        // Count, then x, y pairs
        NEED(1);
        unsigned np = rd16(a);
        NEED(1 + 2ul * np);
        int16_t *pt = scratch(m, (size_t)np * 4 + 4);
        if (!pt) return -1;
        for (unsigned i = 0; i < 2 * np; i++) pt[i] = W(1 + i);
        r->pt = pt;
        r->npt = (int)np;
        return 1;
    }
    case UWMF_POLYPOLYGON: {                     // Polygons, each one's count, then every point
        NEED(1);
        unsigned npoly = rd16(a);
        NEED(1 + npoly);
        unsigned long total = 0;
        for (unsigned i = 0; i < npoly; i++) total += rd16(a + 2 + 2 * i);
        NEED(1 + npoly + 2 * total);
        uint8_t *mem = scratch(m, (size_t)npoly * 2 + (size_t)total * 4 + 4);
        if (!mem) return -1;
        uint16_t *counts = (uint16_t *)mem;
        int16_t *pt = (int16_t *)(mem + ((size_t)npoly * 2 + 3) / 4 * 4);
        for (unsigned i = 0; i < npoly; i++) counts[i] = (uint16_t)rd16(a + 2 + 2 * i);
        for (unsigned long i = 0; i < 2 * total; i++) pt[i] = W(1 + npoly + i);
        r->counts = counts;
        r->ncounts = (int)npoly;
        r->pt = pt;
        r->npt = (int)total;
        return 1;
    }
    case UWMF_CREATEPENINDIRECT:                 // Style, Width as a POINTS, ColorRef
        NEED(5);
        r->v[0] = W(0);
        r->v[1] = W(1);
        r->nv = 2;
        r->color = rd32(a + 6);
        r->slot = take_slot(m);
        return 1;
    case UWMF_CREATEBRUSHINDIRECT:               // Style, ColorRef, Hatch
        NEED(4);
        r->v[0] = W(0);
        r->v[1] = W(3);
        r->nv = 2;
        r->color = rd32(a + 2);
        r->slot = take_slot(m);
        return 1;
    case UWMF_CREATEFONTINDIRECT: {              // a LOGFONT: five words, eight bytes, the face
        NEED(9);
        for (int i = 0; i < 5; i++) r->v[i] = W(i);
        for (int i = 0; i < 8; i++) r->v[5 + i] = a[10 + i];
        r->nv = 13;
        size_t room = (size_t)nw * 2 - 18;
        for (size_t i = 0; i < room && i < sizeof r->face - 1 && a[18 + i]; i++) r->face[i] = (char)a[18 + i];
        r->slot = take_slot(m);
        return 1;
    }
    case UWMF_DIBCREATEPATTERNBRUSH:             // Style, ColorUsage, the DIB
        NEED(2);
        r->v[0] = W(0);
        r->v[1] = W(1);
        r->nv = 2;
        r->dib = a + 4;
        r->ndib = (nw - 2) * 2;
        r->slot = take_slot(m);
        return 1;
    case UWMF_CREATEPALETTE:
    case UWMF_CREATEPATTERNBRUSH:
    case UWMF_CREATEREGION:
        r->slot = take_slot(m);
        return 1;
    case UWMF_DIBBITBLT: {
        // Rop, YSrc, XSrc, [Reserved without a bitmap], Height, Width, YDest, XDest, [DIB]
        int bitmap = size > (func >> 8) + 3u;
        unsigned k = bitmap ? 8 : 9;
        NEED(k);
        r->rop = rd32(a);
        r->v[0] = W(k - 1); r->v[1] = W(k - 2); r->v[2] = W(k - 3); r->v[3] = W(k - 4);
        r->v[4] = W(3); r->v[5] = W(2);
        r->nv = 6;
        if (bitmap) { r->dib = a + 2 * k; r->ndib = (nw - k) * 2; }
        return 1;
    }
    case UWMF_DIBSTRETCHBLT: {
        // Rop, SrcHeight, SrcWidth, YSrc, XSrc, [Reserved], DestHeight, DestWidth, YDest, XDest, [DIB]
        int bitmap = size > (func >> 8) + 3u;
        unsigned k = bitmap ? 10 : 11;
        NEED(k);
        r->rop = rd32(a);
        r->v[0] = W(k - 1); r->v[1] = W(k - 2); r->v[2] = W(k - 3); r->v[3] = W(k - 4);
        r->v[4] = W(5); r->v[5] = W(4); r->v[6] = W(3); r->v[7] = W(2);
        r->nv = 8;
        if (bitmap) { r->dib = a + 2 * k; r->ndib = (nw - k) * 2; }
        return 1;
    }
    case UWMF_STRETCHDIB:
        // Rop, ColorUsage, SrcHeight, SrcWidth, YSrc, XSrc, DestHeight, DestWidth, YDest, XDest, DIB
        NEED(11);
        r->rop = rd32(a);
        r->v[0] = W(10); r->v[1] = W(9); r->v[2] = W(8); r->v[3] = W(7);
        r->v[4] = W(6); r->v[5] = W(5); r->v[6] = W(4); r->v[7] = W(3);
        r->v[8] = W(2);
        r->nv = 9;
        r->dib = a + 22;
        r->ndib = (nw - 11) * 2;
        return 1;
    case UWMF_SETDIBTODEV:
        // ColorUsage, ScanCount, StartScan, YDib, XDib, Height, Width, YDest, XDest, DIB
        NEED(9);
        r->v[0] = W(8); r->v[1] = W(7); r->v[2] = W(6); r->v[3] = W(5);
        r->v[4] = W(4); r->v[5] = W(3); r->v[6] = W(2); r->v[7] = W(1); r->v[8] = W(0);
        r->nv = 9;
        r->dib = a + 18;
        r->ndib = (nw - 9) * 2;
        return 1;
    default: {
        // The rest are plain words, stored last argument first.
        unsigned k = nw < 16 ? nw : 16;
        for (unsigned i = 0; i < k; i++) r->v[i] = W(nw - 1 - i);
        r->nv = (int)k;
        return 1;
    }
    }
#undef W
#undef NEED
}
