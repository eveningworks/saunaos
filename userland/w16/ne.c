// ne.c -- the NE executable parser (ne.h). Every read goes through
// rd8/rd16/rd32 or a span check first, so an offset from the file can
// never take a read outside the image; the header's fields are trusted
// only after they have been checked against the size.
#include "ne.h"

#include <string.h>

static int span(const struct ne_module *m, uint32_t off, uint32_t len) {
    return off <= m->size && len <= m->size - off;
}

static uint8_t rd8(const struct ne_module *m, uint32_t off) { return m->img[off]; }

static uint16_t rd16(const struct ne_module *m, uint32_t off) {
    return (uint16_t)(m->img[off] | m->img[off + 1] << 8);
}

static uint32_t rd32(const struct ne_module *m, uint32_t off) {
    return (uint32_t)rd16(m, off) | (uint32_t)rd16(m, off + 2) << 16;
}

// A length-prefixed name at `off` into out; NE_E_NAME if it runs past the image.
static int pname(const struct ne_module *m, uint32_t off, char *out, size_t cap) {
    if (!span(m, off, 1)) return NE_E_NAME;
    uint32_t n = rd8(m, off);
    if (!span(m, off + 1, n) || n + 1 > cap) return NE_E_NAME;
    memcpy(out, m->img + off + 1, n);
    out[n] = '\0';
    return 0;
}

int ne_parse(const uint8_t *img, uint32_t size, struct ne_module *m) {
    memset(m, 0, sizeof *m);
    m->img = img;
    m->size = size;
    if (!span(m, 0, 0x40)) return NE_E_SHORT;
    if (img[0] != 'M' || img[1] != 'Z') return NE_E_NOT_MZ;
    uint32_t ne = rd32(m, 0x3C);
    if (!span(m, ne, 0x40)) return NE_E_SHORT;
    if (img[ne] != 'N' || img[ne + 1] != 'E') return NE_E_NOT_NE;
    m->ne = ne;

    m->flags = rd16(m, ne + 0x0C);
    m->autodata = rd16(m, ne + 0x0E);
    m->heap = rd16(m, ne + 0x10);
    m->stack = rd16(m, ne + 0x12);
    m->entry_ip = rd16(m, ne + 0x14);
    m->entry_cs = rd16(m, ne + 0x16);
    m->sp = rd16(m, ne + 0x18);
    m->ss = rd16(m, ne + 0x1A);
    m->nseg = rd16(m, ne + 0x1C);
    m->nmod = rd16(m, ne + 0x1E);
    uint32_t seg_t = ne + rd16(m, ne + 0x22);
    m->resident = ne + rd16(m, ne + 0x26);
    uint32_t modref_t = ne + rd16(m, ne + 0x28);
    m->imported = ne + rd16(m, ne + 0x2A);
    m->entry_table = ne + rd16(m, ne + 0x04);
    m->entry_len = rd16(m, ne + 0x06);
    m->align_shift = rd16(m, ne + 0x32);
    m->expected_win = rd16(m, ne + 0x3E);
    if (m->align_shift == 0) m->align_shift = 9;   // the format's default
    if (m->align_shift > 16) return NE_E_TABLE;

    if (m->nseg > NE_MAX_SEGMENTS || !span(m, seg_t, 8u * m->nseg)) return NE_E_SEGMENTS;
    if (m->nmod > NE_MAX_MODULES || !span(m, modref_t, 2u * m->nmod)) return NE_E_MODULES;
    if (!span(m, m->entry_table, m->entry_len)) return NE_E_TABLE;
    if (!span(m, m->imported, 1)) return NE_E_TABLE;
    if (m->autodata > m->nseg || m->entry_cs > m->nseg || m->ss > m->nseg) return NE_E_RANGE;

    for (unsigned i = 0; i < m->nseg; i++) {
        struct ne_segment *s = &m->seg[i];
        uint32_t e = seg_t + 8 * i;
        s->file_off = (uint32_t)rd16(m, e) << m->align_shift;
        s->length = rd16(m, e + 2);
        s->flags = rd16(m, e + 4);
        s->minalloc = rd16(m, e + 6);
        if (s->length == 0 && s->file_off) s->length = 0x10000;
        if (s->minalloc == 0) s->minalloc = 0x10000;
        if (s->file_off && !span(m, s->file_off, s->length)) return NE_E_SEGMENTS;
        if (s->flags & NE_SEG_RELOCS) {
            if (!s->file_off) return NE_E_RELOC;
            uint32_t r = s->file_off + s->length;
            if (!span(m, r, 2)) return NE_E_RELOC;
            s->nrelocs = rd16(m, r);
            s->reloc_off = r + 2;
            if (!span(m, s->reloc_off, 8u * s->nrelocs)) return NE_E_RELOC;
        }
    }
    for (unsigned i = 0; i < m->nmod; i++) m->modref[i] = rd16(m, modref_t + 2 * i);
    return pname(m, m->resident, m->name, sizeof m->name);
}

int ne_reloc_at(const struct ne_module *m, unsigned seg, unsigned i, struct ne_reloc *out) {
    if (seg < 1 || seg > m->nseg) return NE_E_RANGE;
    const struct ne_segment *s = &m->seg[seg - 1];
    if (i >= s->nrelocs) return NE_E_RANGE;
    uint32_t r = s->reloc_off + 8 * i;
    out->src = rd8(m, r);
    out->flags = rd8(m, r + 1);
    out->site = rd16(m, r + 2);
    out->a = rd16(m, r + 4);
    out->b = rd16(m, r + 6);
    return 0;
}

int ne_module_name(const struct ne_module *m, unsigned idx, char *out, size_t cap) {
    if (idx < 1 || idx > m->nmod) return NE_E_RANGE;
    return pname(m, m->imported + m->modref[idx - 1], out, cap);
}

int ne_entry(const struct ne_module *m, unsigned ordinal, uint16_t *seg, uint16_t *off) {
    uint32_t p = m->entry_table, end = m->entry_table + m->entry_len;
    unsigned ord = 1;
    while (p < end) {
        unsigned count = rd8(m, p);
        if (count == 0) break;
        if (p + 2 > end) return NE_E_ENTRY;
        unsigned type = rd8(m, p + 1);
        p += 2;
        unsigned size = type == 0 ? 0 : type == 0xFF ? 6 : 3;
        if (size && (end - p) / size < count) return NE_E_ENTRY;
        if (ordinal >= ord && ordinal < ord + count) {
            if (type == 0) return NE_E_ENTRY;
            uint32_t e = p + (ordinal - ord) * size;
            if (type == 0xFF) {
                *seg = rd8(m, e + 3);
                *off = rd16(m, e + 4);
            } else {
                *seg = (uint16_t)type;
                *off = rd16(m, e + 1);
            }
            return *seg >= 1 && *seg <= m->nseg ? 0 : NE_E_ENTRY;
        }
        p += size * count;
        ord += count;
    }
    return NE_E_ENTRY;
}

const char *ne_strerror(int err) {
    switch (err) {
    case NE_E_SHORT:    return "the file is shorter than its headers say";
    case NE_E_NOT_MZ:   return "not an MZ executable";
    case NE_E_NOT_NE:   return "an MZ executable but not NE (a DOS or PE program?)";
    case NE_E_TABLE:    return "a header table lies outside the file";
    case NE_E_SEGMENTS: return "a segment lies outside the file, or too many segments";
    case NE_E_MODULES:  return "the module reference table is malformed";
    case NE_E_RELOC:    return "a relocation table lies outside the file";
    case NE_E_NAME:     return "a name runs past its table";
    case NE_E_RANGE:    return "an index is out of range";
    case NE_E_ENTRY:    return "the entry table is malformed or lacks the ordinal";
    default:            return "unknown error";
    }
}
