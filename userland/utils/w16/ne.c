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

static int parse_resources(struct ne_module *m, uint32_t t);

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
    int err = pname(m, m->resident, m->name, sizeof m->name);
    if (err < 0) return err;
    // The resource table sits between the segment table and the resident
    // names; an offset equal to the latter's means there is none.
    uint32_t res_t = ne + rd16(m, ne + 0x24);
    if (res_t != m->resident) return parse_resources(m, res_t);
    return 0;
}

// A resource-table name (a type's or a resource's) at table offset `rel`,
// upper-cased; NE_E_RESOURCE if it runs past the image.
static int res_name(const struct ne_module *m, uint16_t rel, char *out, size_t cap) {
    char tmp[256];
    if (pname(m, m->resources + rel, tmp, sizeof tmp) < 0) return NE_E_RESOURCE;
    size_t n = 0;
    for (; tmp[n] && n + 1 < cap; n++)
        out[n] = tmp[n] >= 'a' && tmp[n] <= 'z' ? (char)(tmp[n] - 32) : tmp[n];
    out[n] = '\0';
    return 0;
}

// Walks the table once at parse time, so ne_resource_at() can trust every
// span it was given: a type block's entries, and each resource's bytes.
static int parse_resources(struct ne_module *m, uint32_t t) {
    if (!span(m, t, 2)) return NE_E_RESOURCE;
    m->resources = t;
    m->res_shift = rd16(m, t);
    if (m->res_shift > 16) return NE_E_RESOURCE;
    uint32_t p = t + 2;
    unsigned n = 0;
    for (unsigned blocks = 0; ; blocks++) {
        if (!span(m, p, 2) || blocks > 0x400) return NE_E_RESOURCE;
        uint16_t type = rd16(m, p);
        if (type == 0) break;
        if (!span(m, p, 8)) return NE_E_RESOURCE;
        uint16_t count = rd16(m, p + 2);
        p += 8;
        if (!span(m, p, 12u * count) || n + count > 0xFFFF) return NE_E_RESOURCE;
        for (unsigned i = 0; i < count; i++) {
            uint32_t off = (uint32_t)rd16(m, p + 12 * i) << m->res_shift;
            uint32_t len = (uint32_t)rd16(m, p + 12 * i + 2) << m->res_shift;
            if (!span(m, off, len)) return NE_E_RESOURCE;
        }
        p += 12u * count;
        n += count;
    }
    m->nres = (uint16_t)n;
    return 0;
}

int ne_resource_at(const struct ne_module *m, unsigned i, struct ne_resource *out) {
    if (i >= m->nres) return NE_E_RANGE;
    uint32_t p = m->resources + 2;
    for (;;) {
        uint16_t type = rd16(m, p), count = rd16(m, p + 2);
        if (type == 0) return NE_E_RANGE;
        if (i < count) {
            uint32_t e = p + 8 + 12 * i;
            memset(out, 0, sizeof *out);
            if (type & 0x8000) out->type = type & 0x7FFF;
            else if (res_name(m, type, out->type_name, sizeof out->type_name) < 0) return NE_E_RESOURCE;
            uint16_t id = rd16(m, e + 6);
            if (id & 0x8000) out->id = id & 0x7FFF;
            else if (res_name(m, id, out->name, sizeof out->name) < 0) return NE_E_RESOURCE;
            out->off = (uint32_t)rd16(m, e) << m->res_shift;
            out->len = (uint32_t)rd16(m, e + 2) << m->res_shift;
            return 0;
        }
        i -= count;
        p += 8 + 12u * count;
    }
}

static int same_name(const char *have, const char *want) {
    for (; *have && *want; have++, want++) {
        char w = *want >= 'a' && *want <= 'z' ? (char)(*want - 32) : *want;
        if (*have != w) return 0;
    }
    return *have == *want;
}

// A name of the form "#123" is the number 123, as Windows reads it.
static int hash_number(const char *name, uint16_t *out) {
    if (!name || name[0] != '#' || !name[1]) return 0;
    unsigned v = 0;
    for (const char *p = name + 1; *p; p++) {
        if (*p < '0' || *p > '9' || v > 0x7FFF) return 0;
        v = v * 10 + (unsigned)(*p - '0');
    }
    *out = (uint16_t)v;
    return 1;
}

int ne_resource_find(const struct ne_module *m, uint16_t type, const char *type_name,
                     uint16_t id, const char *name, struct ne_resource *out) {
    if (hash_number(type_name, &type)) type_name = 0;
    if (hash_number(name, &id)) name = 0;
    for (unsigned i = 0; i < m->nres; i++) {
        if (ne_resource_at(m, i, out) < 0) return NE_E_RESOURCE;
        int type_ok = type_name ? out->type_name[0] && same_name(out->type_name, type_name)
                                : !out->type_name[0] && out->type == type;
        int id_ok = name ? out->name[0] && same_name(out->name, name)
                         : !out->name[0] && out->id == id;
        if (type_ok && id_ok) return (int)i;
    }
    return NE_E_RANGE;
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

int ne_description(const struct ne_module *m, char *out, size_t cap) {
    out[0] = 0;
    uint32_t off = rd32(m, m->ne + 0x2C);
    uint16_t len = rd16(m, m->ne + 0x20);
    if (!off || !len) return 0;
    if (!span(m, off, len)) return NE_E_NAME;
    return pname(m, off, out, cap);
}

int ne_imported_name(const struct ne_module *m, uint16_t off, char *out, size_t cap) {
    return pname(m, m->imported + off, out, cap);
}

int ne_module_name(const struct ne_module *m, unsigned idx, char *out, size_t cap) {
    if (idx < 1 || idx > m->nmod) return NE_E_RANGE;
    return pname(m, m->imported + m->modref[idx - 1], out, cap);
}

int ne_entry(const struct ne_module *m, unsigned ordinal, uint16_t *seg, uint16_t *off) {
    uint8_t flags;
    return ne_entry_flags(m, ordinal, seg, off, &flags);
}

unsigned ne_entry_count(const struct ne_module *m) {
    uint32_t p = m->entry_table, end = m->entry_table + m->entry_len;
    unsigned n = 0;
    while (p + 2 <= end) {
        unsigned count = rd8(m, p), type = rd8(m, p + 1);
        if (count == 0) break;
        unsigned size = type == 0 ? 0 : type == 0xFF ? 6 : 3;
        p += 2;
        if (size && (end - p) / size < count) break;
        p += size * count;
        n += count;
    }
    return n;
}

// A name table (resident or non-resident) from `p` to `end`: the
// ordinal of `name`, compared without case; 0 when it is not there.
static unsigned names_find(const struct ne_module *m, uint32_t p, uint32_t end, const char *name) {
    while (p < end && span(m, p, 1)) {
        unsigned n = rd8(m, p);
        if (n == 0 || !span(m, p + 1, n + 2u) || p + 1 + n + 2 > end) break;
        size_t k = 0;
        for (; k < n && name[k]; k++) {
            char a = (char)m->img[p + 1 + k], b = name[k];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
        }
        uint16_t ord = rd16(m, p + 1 + n);
        if (k == n && !name[k] && ord) return ord;
        p += 1 + n + 2;
    }
    return 0;
}

unsigned ne_export_find(const struct ne_module *m, const char *name) {
    // The resident names run to the module-reference table; the
    // non-resident ones are their own table, its length in the header.
    uint32_t res_end = m->ne + rd16(m, m->ne + 0x28);
    unsigned ord = res_end > m->resident ? names_find(m, m->resident, res_end, name) : 0;
    if (ord) return ord;
    uint32_t off = rd32(m, m->ne + 0x2C);
    uint16_t len = rd16(m, m->ne + 0x20);
    if (!off || !len || !span(m, off, len)) return 0;
    return names_find(m, off, off + len, name);
}

int ne_entry_flags(const struct ne_module *m, unsigned ordinal, uint16_t *seg, uint16_t *off, uint8_t *flags) {
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
            *flags = rd8(m, e);
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

const char *ne_what(const uint8_t *img, uint32_t size) {
    if (size < 2 || img[0] != 'M' || img[1] != 'Z') return "not a program";
    uint32_t at = size >= 0x40 ? (uint32_t)img[0x3C] | img[0x3D] << 8 | img[0x3E] << 16 | (uint32_t)img[0x3F] << 24 : 0;
    if (!at || at > size - 2) return "a DOS program";
    const uint8_t *sig = img + at;
    if (sig[0] == 'N' && sig[1] == 'E') return 0;
    if (sig[0] == 'P' && sig[1] == 'E') return "a 32-bit or 64-bit Windows program";
    if (sig[0] == 'L' && (sig[1] == 'E' || sig[1] == 'X')) return "an OS/2 program or a Windows driver";
    return "a DOS program";
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
    case NE_E_RESOURCE: return "the resource table is malformed";
    default:            return "unknown error";
    }
}
