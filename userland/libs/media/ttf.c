// TrueType parsing and rasterization in fixed point. See api/ttf.h for
// what this is, the three properties that shape it (no floating point,
// no allocation, every read bounds-checked) and why a font parser in
// ring 0 needs that third one in particular.
//
// RING 3 ONLY since the kernel stopped parsing fonts (f34019fc): libmedia,
// for /bin/fontd and every process's glyph cache. It still includes fixed.h
// and <stdint.h> and nothing else, so a host check compiles it as-is.
#include "ttf.h"
#include "ttf_internal.h"
#include <stddef.h>

// --- bounds-checked reads --------------------------------------------
//
// THE ONLY WAY THIS FILE TOUCHES THE FILE BUFFER. Every one of these
// takes the absolute offset and answers 0 when the read would leave the
// buffer, so a truncated or hostile table produces zeros and a refusal
// upstream rather than a read of whatever follows the font in memory.
// There is deliberately no "and I know this is in range" fast path: the
// checks are two comparisons against a value already in a register, and
// the one time somebody skips them is the one time it matters.
static uint32_t rd_u8(const struct ttf_font *f, uint32_t off) {
    if (off >= f->len) return 0;
    return f->data[off];
}

static uint32_t rd_u16(const struct ttf_font *f, uint32_t off) {
    if (off + 2 > f->len) return 0;
    return ((uint32_t)f->data[off] << 8) | f->data[off + 1];
}

static int32_t rd_i16(const struct ttf_font *f, uint32_t off) {
    return (int32_t)(int16_t)(uint16_t)rd_u16(f, off);
}

static uint32_t rd_u32(const struct ttf_font *f, uint32_t off) {
    if (off + 4 > f->len) return 0;
    return ((uint32_t)f->data[off] << 24) | ((uint32_t)f->data[off + 1] << 16)
         | ((uint32_t)f->data[off + 2] << 8) | (uint32_t)f->data[off + 3];
}

static int in_range(const struct ttf_font *f, uint32_t off, uint32_t bytes) {
    return off <= f->len && bytes <= f->len - off;
}

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) \
                       | ((uint32_t)(c) << 8) | (uint32_t)(d))

static void find_blues(struct ttf_font *f);

int ttf_open(struct ttf_font *f, const uint8_t *data, uint32_t len) {
    if (!f || !data || len < 12) return 0;
    for (uint32_t i = 0; i < sizeof(*f); i++) ((uint8_t *)f)[i] = 0;
    f->data = data;
    f->len = len;

    uint32_t ver = rd_u32(f, 0);
    // 0x00010000 is TrueType outlines; 'true' is the old Apple tag for
    // the same thing. 'OTTO' (CFF/Type2 outlines) and 'ttcf' (a
    // collection) are REFUSED rather than half-supported -- there is no
    // `glyf` in an OTTO font at all, so it would fail later and less
    // clearly. font_face.c reports the refusal by name.
    if (ver != 0x00010000u && ver != TAG('t', 'r', 'u', 'e')) return 0;

    uint32_t kern_off = 0, kern_len = 0;
    uint32_t num_tables = rd_u16(f, 4);
    if (num_tables == 0 || num_tables > 512) return 0;
    if (!in_range(f, 12, num_tables * 16)) return 0;

    for (uint32_t i = 0; i < num_tables; i++) {
        uint32_t rec = 12 + i * 16;
        uint32_t tag = rd_u32(f, rec);
        uint32_t off = rd_u32(f, rec + 8);
        uint32_t tlen = rd_u32(f, rec + 12);
        if (!in_range(f, off, tlen)) continue; // a table that does not fit is absent
        switch (tag) {
        case TAG('g', 'l', 'y', 'f'): f->glyf = off; f->glyf_len = tlen; break;
        case TAG('l', 'o', 'c', 'a'): f->loca = off; f->loca_len = tlen; break;
        case TAG('c', 'm', 'a', 'p'): f->cmap = off; break;
        case TAG('h', 'm', 't', 'x'): f->hmtx = off; break;
        case TAG('h', 'e', 'a', 'd'): f->head = off; break;
        case TAG('h', 'h', 'e', 'a'): f->hhea = off; break;
        case TAG('m', 'a', 'x', 'p'): f->maxp = off; break;
        case TAG('k', 'e', 'r', 'n'): kern_off = off; kern_len = tlen; break;
        case TAG('n', 'a', 'm', 'e'): f->name = off; f->name_len = tlen; break;
        case TAG('O', 'S', '/', '2'): f->os2 = off; f->os2_len = tlen; break;
        case TAG('V', 'D', 'M', 'X'): f->vdmx = off; f->vdmx_len = tlen; break;
        case TAG('h', 'd', 'm', 'x'): f->hdmx = off; f->hdmx_len = tlen; break;
        case TAG('C', 'O', 'L', 'R'): f->colr = off; f->colr_len = tlen; break;
        case TAG('C', 'P', 'A', 'L'): f->cpal = off; f->cpal_len = tlen; break;
        case TAG('G', 'P', 'O', 'S'): f->gpos = off; f->gpos_len = tlen; break;
        case TAG('G', 'S', 'U', 'B'): f->gsub = off; f->gsub_len = tlen; break;
        default: break;
        }
    }

    if (!f->glyf || !f->loca || !f->head || !f->hhea || !f->maxp) return 0;

    f->units_per_em = (int)rd_u16(f, f->head + 18);
    f->loca_long = (int)rd_i16(f, f->head + 50);
    f->num_glyphs = (int)rd_u16(f, f->maxp + 4);
    f->ascent = (int)rd_i16(f, f->hhea + 4);
    f->descent = -(int)rd_i16(f, f->hhea + 6); // stored negative, kept positive here
    f->line_gap = (int)rd_i16(f, f->hhea + 8);
    f->num_hmetrics = (int)rd_u16(f, f->hhea + 34);

    // unitsPerEm outside this range is not a font this can scale
    // sensibly, and 0 would divide by zero in ttf_scale_for_px().
    if (f->units_per_em < 16 || f->units_per_em > 16384) return 0;
    if (f->num_glyphs <= 0) return 0;
    if (f->descent < 0) f->descent = 0;

    // `kern` is OPTIONAL, so nothing here may fail the open -- a font
    // whose kern table is malformed still draws, unkerned. That is the
    // opposite of the required tables above, and deliberate: refusing a
    // whole face over an advisory table would lose the user their font
    // for a defect in the one part of it that only affects spacing.
    //
    // Version 0 is the Microsoft/OpenType header (u16 version, u16
    // nTables). Apple's own `kern` starts with a u32 version 0x00010000
    // and a u32 nTables, and is NOT read -- the two are distinguishable
    // by that first u16 and only the Microsoft one appears in the fonts
    // this OS ships or is likely to be handed.
    if (kern_off && kern_len >= 4 && rd_u16(f, kern_off) == 0) {
        uint32_t ntab = rd_u16(f, kern_off + 2);
        uint32_t p = kern_off + 4;
        for (uint32_t i = 0; i < ntab && i < 32; i++) {
            if (!in_range(f, p, 14)) break;
            uint32_t sub_len = rd_u16(f, p + 2);
            uint32_t coverage = rd_u16(f, p + 4);
            // Bit 0 set = horizontal; bit 2 set = cross-stream (a
            // vertical shift, not a horizontal one); high byte = format.
            int horizontal = (coverage & 0x0001) != 0;
            int cross = (coverage & 0x0004) != 0;
            int format = (int)(coverage >> 8);
            uint32_t npairs = rd_u16(f, p + 6);
            uint32_t pairs = p + 14;
            if (format == 0 && horizontal && !cross && npairs > 0
                && in_range(f, pairs, npairs * 6)) {
                f->kern_pairs_off = pairs;
                f->kern_pairs = (int)npairs;
                break; // the first usable subtable wins
            }
            if (sub_len < 14) break; // a zero-length subtable would loop forever
            p += sub_len;
        }
    }
    find_blues(f);
    return 1;
}

// --- cmap ------------------------------------------------------------

static int cmap_lookup_f4(const struct ttf_font *f, uint32_t sub, uint32_t cp) {
    if (cp > 0xFFFF) return 0;
    uint32_t segx2 = rd_u16(f, sub + 6);
    if (!segx2 || (segx2 & 1)) return 0;
    uint32_t ends = sub + 14;
    uint32_t starts = ends + segx2 + 2;
    uint32_t deltas = starts + segx2;
    uint32_t ranges = deltas + segx2;

    for (uint32_t i = 0; i < segx2; i += 2) {
        if (cp > rd_u16(f, ends + i)) continue;
        uint32_t start = rd_u16(f, starts + i);
        if (cp < start) return 0; // segments are sorted; past it means absent
        int32_t delta = rd_i16(f, deltas + i);
        uint32_t ro = rd_u16(f, ranges + i);
        if (ro == 0) return (int)((cp + (uint32_t)delta) & 0xFFFF);
        // glyphIdArray is addressed relative to the range offset's OWN
        // slot, which is the one piece of cmap format 4 that reads like
        // a typo and is not.
        uint32_t gi_off = ranges + i + ro + (cp - start) * 2;
        uint32_t gid = rd_u16(f, gi_off);
        if (!gid) return 0;
        return (int)((gid + (uint32_t)delta) & 0xFFFF);
    }
    return 0;
}

static int cmap_lookup_f12(const struct ttf_font *f, uint32_t sub, uint32_t cp) {
    uint32_t ngroups = rd_u32(f, sub + 12);
    if (ngroups > 100000) return 0;
    for (uint32_t i = 0; i < ngroups; i++) {
        uint32_t g = sub + 16 + i * 12;
        uint32_t lo = rd_u32(f, g);
        uint32_t hi = rd_u32(f, g + 4);
        if (cp < lo) return 0;
        if (cp > hi) continue;
        return (int)(rd_u32(f, g + 8) + (cp - lo));
    }
    return 0;
}

int ttf_glyph_index(const struct ttf_font *f, uint32_t codepoint) {
    if (!f->cmap) return 0;
    uint32_t n = rd_u16(f, f->cmap + 2);
    if (n > 64) return 0;

    // Prefer a Unicode subtable, and among those prefer format 12 (full
    // range) over format 4 (BMP). A (3,0) Windows-symbol subtable is
    // taken as a last resort because some monospace faces ship only
    // that -- their codepoints then live at 0xF000 + c, which is why the
    // symbol retry below exists.
    uint32_t best = 0;
    int best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t rec = f->cmap + 4 + i * 8;
        uint32_t plat = rd_u16(f, rec);
        uint32_t enc = rd_u16(f, rec + 2);
        uint32_t sub = f->cmap + rd_u32(f, rec + 4);
        if (!in_range(f, sub, 4)) continue;
        uint32_t fmt = rd_u16(f, sub);
        if (fmt != 4 && fmt != 12) continue;

        int score = -1;
        if (plat == 3 && enc == 10) score = 4;      // Windows UCS-4
        else if (plat == 0) score = 3;              // Unicode
        else if (plat == 3 && enc == 1) score = 2;   // Windows BMP
        else if (plat == 3 && enc == 0) score = 1;   // Windows symbol
        if (score > best_score) { best_score = score; best = sub; }
    }
    if (best_score < 0) return 0;

    uint32_t fmt = rd_u16(f, best);
    int gid = fmt == 12 ? cmap_lookup_f12(f, best, codepoint)
                        : cmap_lookup_f4(f, best, codepoint);
    if (!gid && best_score == 1 && codepoint < 0x100) {
        gid = fmt == 12 ? cmap_lookup_f12(f, best, 0xF000 + codepoint)
                        : cmap_lookup_f4(f, best, 0xF000 + codepoint);
    }
    if (gid < 0 || gid >= f->num_glyphs) return 0;
    return gid;
}

// --- metrics ---------------------------------------------------------

int ttf_advance_units(const struct ttf_font *f, int gid) {
    if (!f->hmtx || f->num_hmetrics <= 0) return f->units_per_em;
    int i = gid;
    // Past numberOfHMetrics every glyph shares the LAST entry's
    // advance. That is not a fallback -- it is how a monospace font
    // stores one advance for thousands of glyphs, so the common case
    // for the faces this loads goes through here.
    if (i >= f->num_hmetrics) i = f->num_hmetrics - 1;
    if (i < 0) return f->units_per_em;
    return (int)rd_u16(f, f->hmtx + (uint32_t)i * 4);
}

int ttf_advance_px(const struct ttf_font *f, int gid, int px) {
    fx_t scale = ttf_scale_for_px(f, px);
    return fx_round(fx_mul(fx_from_int(ttf_advance_units(f, gid)), scale));
}

// --- kerning ---------------------------------------------------------

static int kern_table_units(const struct ttf_font *f, int left_gid, int right_gid) {
    if (!f->kern_pairs) return 0;
    // The pair array is sorted by the 32-bit key (left << 16) | right,
    // which the spec guarantees and which is what makes this a binary
    // search over 908 entries rather than a scan per character pair.
    // A font that lied about the ordering gets a wrong-or-zero kern,
    // never an out-of-range read: every access below is through rd_u16.
    uint32_t want = ((uint32_t)left_gid << 16) | (uint32_t)right_gid;
    int lo = 0, hi = f->kern_pairs - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t e = f->kern_pairs_off + (uint32_t)mid * 6;
        uint32_t key = ((uint32_t)rd_u16(f, e) << 16) | rd_u16(f, e + 2);
        if (key == want) return (int)rd_i16(f, e + 4);
        if (key < want) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

// --- OpenType layout: GPOS pair kerning, GSUB ligatures ----------------
//
// THE SMALL SUBSET OF A SHAPER THIS OS USES (docs/decisions/gui.md,
// "Grapheme clusters, pair kerning and emoji sequences"): a PairPos lookup's
// XAdvance between two glyphs, and a ligature lookup over a few glyphs. No
// contextual lookups, no mark positioning, no lookup flags -- a lookup that
// is not type 2 (GPOS) or 4 (GSUB) is skipped, and so is a flag's "ignore
// marks", which only matters when marks sit between the glyphs, which the
// caller never passes. Every offset is read through rd_*, and every loop is
// bounded by a count read from the file AND by the table's own length.

// A script's DEFAULT LangSys -> the Feature table tagged `feature`, or 0.
// Missing script: DFLT, then latn (HarfBuzz's fallback order).
static uint32_t otl_feature(const struct ttf_font *f, uint32_t table, uint32_t tlen,
                            uint32_t script, uint32_t feature) {
    if (!table || tlen < 10) return 0;
    uint32_t sl = table + rd_u16(f, table + 4), fl = table + rd_u16(f, table + 6);
    uint32_t nscripts = rd_u16(f, sl), nfeat = rd_u16(f, fl);
    if (!in_range(f, sl + 2, nscripts * 6) || !in_range(f, fl + 2, nfeat * 6)) return 0;
    const uint32_t order[3] = { script, TAG('D', 'F', 'L', 'T'), TAG('l', 'a', 't', 'n') };
    for (int o = 0; o < 3; o++) {
        for (uint32_t i = 0; i < nscripts; i++) {
            if (rd_u32(f, sl + 2 + i * 6) != order[o]) continue;
            uint32_t so = sl + rd_u16(f, sl + 6 + i * 6);
            uint32_t def = rd_u16(f, so);
            if (!def) break;                        // no default LangSys: try the next script
            uint32_t ls = so + def, n = rd_u16(f, ls + 4);
            for (uint32_t k = 0; k < n && k < 1024; k++) {
                uint32_t fi = rd_u16(f, ls + 6 + k * 2);
                if (fi < nfeat && rd_u32(f, fl + 2 + fi * 6) == feature)
                    return fl + rd_u16(f, fl + 6 + fi * 6);
            }
            return 0;   // the script is there and does not use this feature
        }
    }
    return 0;
}

// Coverage index of `gid`, or -1.
static int otl_coverage(const struct ttf_font *f, uint32_t cov, int gid) {
    uint32_t fmt = rd_u16(f, cov), n = rd_u16(f, cov + 2);
    int lo = 0, hi = (int)n - 1;
    if (fmt == 1) {
        if (!in_range(f, cov + 4, n * 2)) return -1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            int g = (int)rd_u16(f, cov + 4 + (uint32_t)mid * 2);
            if (g == gid) return mid;
            if (g < gid) lo = mid + 1; else hi = mid - 1;
        }
    } else if (fmt == 2) {
        if (!in_range(f, cov + 4, n * 6)) return -1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint32_t r = cov + 4 + (uint32_t)mid * 6;
            int a = (int)rd_u16(f, r), z = (int)rd_u16(f, r + 2);
            if (gid < a) hi = mid - 1;
            else if (gid > z) lo = mid + 1;
            else return (int)rd_u16(f, r + 4) + gid - a;
        }
    }
    return -1;
}

static int otl_class(const struct ttf_font *f, uint32_t cd, int gid) {
    uint32_t fmt = rd_u16(f, cd);
    if (fmt == 1) {
        int start = (int)rd_u16(f, cd + 2), n = (int)rd_u16(f, cd + 4);
        if (gid < start || gid >= start + n) return 0;
        return (int)rd_u16(f, cd + 6 + (uint32_t)(gid - start) * 2);
    }
    if (fmt == 2) {
        int lo = 0, hi = (int)rd_u16(f, cd + 2) - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint32_t r = cd + 4 + (uint32_t)mid * 6;
            int a = (int)rd_u16(f, r), z = (int)rd_u16(f, r + 2);
            if (gid < a) hi = mid - 1;
            else if (gid > z) lo = mid + 1;
            else return (int)rd_u16(f, r + 4);
        }
    }
    return 0;
}

// A ValueRecord's size, and its XAdvance (format bit 0x0004) at `vr`.
static uint32_t vr_size(uint32_t fmt) {
    uint32_t n = 0;
    for (int b = 0; b < 8; b++) n += (fmt >> b) & 1;
    return n * 2;
}
static int vr_xadvance(const struct ttf_font *f, uint32_t fmt, uint32_t vr) {
    if (!(fmt & 0x0004)) return 0;
    uint32_t skip = vr_size(fmt & 0x0003);
    return (int)rd_i16(f, vr + skip);
}

// One PairPos subtable: 1 with *units set when it APPLIES (OpenType: the
// first subtable that applies ends the lookup), 0 otherwise.
static int pairpos(const struct ttf_font *f, uint32_t st, int l, int r, int *units) {
    uint32_t fmt = rd_u16(f, st);
    if (otl_coverage(f, st + rd_u16(f, st + 2), l) < 0) return 0;
    uint32_t vf1 = rd_u16(f, st + 4), vf2 = rd_u16(f, st + 6);
    uint32_t s1 = vr_size(vf1), s2 = vr_size(vf2);
    if (fmt == 1) {
        int ci = otl_coverage(f, st + rd_u16(f, st + 2), l);
        if ((uint32_t)ci >= rd_u16(f, st + 8)) return 0;
        uint32_t ps = st + rd_u16(f, st + 10 + (uint32_t)ci * 2);
        uint32_t n = rd_u16(f, ps), rec = 2 + s1 + s2;
        if (!in_range(f, ps + 2, n * rec)) return 0;
        int lo = 0, hi = (int)n - 1;
        while (lo <= hi) {                      // PairSets are sorted by the second glyph
            int mid = lo + (hi - lo) / 2;
            uint32_t pr = ps + 2 + (uint32_t)mid * rec;
            int g = (int)rd_u16(f, pr);
            if (g == r) { *units = vr_xadvance(f, vf1, pr + 2); return 1; }
            if (g < r) lo = mid + 1; else hi = mid - 1;
        }
        return 0;
    }
    if (fmt == 2) {
        uint32_t n1 = rd_u16(f, st + 12), n2 = rd_u16(f, st + 14);
        uint32_t c1 = (uint32_t)otl_class(f, st + rd_u16(f, st + 8), l);
        uint32_t c2 = (uint32_t)otl_class(f, st + rd_u16(f, st + 10), r);
        if (c1 >= n1 || c2 >= n2) return 0;
        *units = vr_xadvance(f, vf1, st + 16 + (c1 * n2 + c2) * (s1 + s2));
        return 1;
    }
    return 0;
}

// A lookup's subtable `k`, through an Extension (GPOS 9, GSUB 7) if it is
// one; *type gets the real lookup type.
static uint32_t lookup_subtable(const struct ttf_font *f, uint32_t lk, uint32_t k,
                                uint32_t ext_type, uint32_t *type) {
    uint32_t st = lk + rd_u16(f, lk + 6 + k * 2);
    *type = rd_u16(f, lk);
    if (*type == ext_type && rd_u16(f, st) == 1) {
        *type = rd_u16(f, st + 2);
        st += rd_u32(f, st + 4);
    }
    return st;
}

// Calls `fn` on each LOOKUP of `feature` (its table offset), in lookup
// order, until it answers nonzero.
typedef int (*otl_fn)(const struct ttf_font *f, uint32_t lookup, void *ctx);
static void otl_each(const struct ttf_font *f, uint32_t table, uint32_t tlen,
                     uint32_t script, uint32_t feature, otl_fn fn, void *ctx) {
    uint32_t ft = otl_feature(f, table, tlen, script, feature);
    if (!ft) return;
    uint32_t ll = table + rd_u16(f, table + 8), nl = rd_u16(f, ll);
    uint32_t n = rd_u16(f, ft + 2);
    for (uint32_t i = 0; i < n && i < 1024; i++) {
        uint32_t li = rd_u16(f, ft + 4 + i * 2);
        if (li < nl && fn(f, ll + rd_u16(f, ll + 2 + li * 2), ctx)) return;
    }
}

#define OTL_MAX_SUBTABLES 4096u   // a bound on a hostile count, not on a real font

// Every lookup of the feature applies, each adding its own adjustment; inside
// one lookup the first subtable that applies is the only one.
struct pair_ctx { int l, r, units; };
static int pair_fn(const struct ttf_font *f, uint32_t lk, void *ctx) {
    struct pair_ctx *c = ctx;
    uint32_t nsub = rd_u16(f, lk + 4);
    for (uint32_t k = 0; k < nsub && k < OTL_MAX_SUBTABLES; k++) {
        int u = 0;
        uint32_t type, st = lookup_subtable(f, lk, k, 9, &type);
        if (type == 2 && pairpos(f, st, c->l, c->r, &u)) { c->units += u; break; }
    }
    return 0;
}

static uint32_t script_tag(enum ttf_script s) {
    switch (s) {
    case TTF_SCRIPT_GREEK: return TAG('g', 'r', 'e', 'k');
    case TTF_SCRIPT_CYRILLIC: return TAG('c', 'y', 'r', 'l');
    case TTF_SCRIPT_LATIN: return TAG('l', 'a', 't', 'n');
    default: return TAG('D', 'F', 'L', 'T');
    }
}

int ttf_pair_kern_units(const struct ttf_font *f, enum ttf_script script, int left_gid, int right_gid) {
    if (!f || left_gid < 0 || right_gid < 0) return 0;
    // A face whose GPOS kerns this script is believed over its old `kern`
    // table, which a modern face keeps only for old software, if at all.
    if (otl_feature(f, f->gpos, f->gpos_len, script_tag(script), TAG('k', 'e', 'r', 'n'))) {
        struct pair_ctx c = { left_gid, right_gid, 0 };
        otl_each(f, f->gpos, f->gpos_len, script_tag(script), TAG('k', 'e', 'r', 'n'), pair_fn, &c);
        return c.units;
    }
    return kern_table_units(f, left_gid, right_gid);
}

int ttf_kern_units(const struct ttf_font *f, int left_gid, int right_gid) {
    return ttf_pair_kern_units(f, TTF_SCRIPT_LATIN, left_gid, right_gid);
}

int ttf_kern_px(const struct ttf_font *f, int left_gid, int right_gid, int px) {
    int units = ttf_kern_units(f, left_gid, right_gid);
    if (!units) return 0;
    return fx_round(fx_mul(fx_from_int(units), ttf_scale_for_px(f, px)));
}

// One ligature at position `i` from LigatureSubst subtable `st`: the
// components it consumed (0 = none matched); the ligature replaces g[i].
static int lig_at(const struct ttf_font *f, uint32_t st, uint16_t *g, int n, int i) {
    if (rd_u16(f, st) != 1) return 0;
    int ci = otl_coverage(f, st + rd_u16(f, st + 2), g[i]);
    if (ci < 0 || (uint32_t)ci >= rd_u16(f, st + 4)) return 0;
    uint32_t ls = st + rd_u16(f, st + 6 + (uint32_t)ci * 2), nl = rd_u16(f, ls);
    for (uint32_t k = 0; k < nl && k < OTL_MAX_SUBTABLES; k++) {
        uint32_t lg = ls + rd_u16(f, ls + 2 + k * 2);
        uint32_t comp = rd_u16(f, lg + 2);
        if (comp < 2 || (int)comp > n - i || !in_range(f, lg + 4, (comp - 1) * 2)) continue;
        uint32_t j = 1;
        while (j < comp && rd_u16(f, lg + 4 + (j - 1) * 2) == g[i + (int)j]) j++;
        if (j == comp) { g[i] = (uint16_t)rd_u16(f, lg); return (int)comp; }   // the set's order is the font's preference
    }
    return 0;
}

struct lig_ctx { uint16_t *g; int n; };
// One lookup over the whole run, left to right; at each position the first
// subtable that matches applies (OpenType's rule), and the run closes up.
static int lig_fn(const struct ttf_font *f, uint32_t lk, void *ctx) {
    struct lig_ctx *c = ctx;
    uint32_t nsub = rd_u16(f, lk + 4);
    int out = 0;
    for (int i = 0; i < c->n;) {
        int used = 0;
        for (uint32_t k = 0; k < nsub && k < OTL_MAX_SUBTABLES && !used; k++) {
            uint32_t type, st = lookup_subtable(f, lk, k, 7, &type);
            if (type == 4) used = lig_at(f, st, c->g, c->n, i);
        }
        c->g[out++] = c->g[i];
        i += used ? used : 1;
    }
    c->n = out;
    return 0;
}

int ttf_ligate(const struct ttf_font *f, enum ttf_script script, uint32_t feature,
               uint16_t *gids, int n) {
    if (!f || !gids || n < 2) return n < 0 ? 0 : n;
    struct lig_ctx c = { gids, n };
    otl_each(f, f->gsub, f->gsub_len, script_tag(script), feature, lig_fn, &c);
    return c.n;
}

// --- synthetic bold --------------------------------------------------

void ttf_embolden(uint8_t *cov, int w, int h, int strength) {
    if (!cov || w <= 0 || h <= 0) return;
    if (strength < 1) strength = 1;
    if (strength > 4) strength = 4;
    // Right to left, so a column already widened is never re-read as a
    // source -- doing it left to right smears the whole row to full ink
    // instead of thickening the strokes in it.
    for (int y = 0; y < h; y++) {
        uint8_t *row = cov + (int64_t)y * w;
        for (int x = w - 1; x >= 0; x--) {
            uint8_t v = row[x];
            for (int d = 1; d <= strength && x - d >= 0; d++)
                if (row[x - d] > v) v = row[x - d];
            row[x] = v;
        }
    }
}

// --- outline decoding ------------------------------------------------

static int loca_range(const struct ttf_font *f, int gid, uint32_t *start, uint32_t *end) {
    if (gid < 0 || gid >= f->num_glyphs) return 0;
    if (f->loca_long) {
        if (!in_range(f, f->loca + (uint32_t)gid * 4, 8)) return 0;
        *start = rd_u32(f, f->loca + (uint32_t)gid * 4);
        *end = rd_u32(f, f->loca + (uint32_t)gid * 4 + 4);
    } else {
        if (!in_range(f, f->loca + (uint32_t)gid * 2, 4)) return 0;
        *start = rd_u16(f, f->loca + (uint32_t)gid * 2) * 2;
        *end = rd_u16(f, f->loca + (uint32_t)gid * 2 + 2) * 2;
    }
    if (*end < *start) return 0;
    if (*end > f->glyf_len) return 0;
    return 1;
}

// --- blue zones --------------------------------------------------------

// A glyph's vertical extent from its `glyf` header -- the bounding box the
// font itself stores, so nothing is decoded. 0 for a missing or empty glyph.
static int glyph_ybox(const struct ttf_font *f, uint32_t cp, int *ymin, int *ymax) {
    int gid = ttf_glyph_index(f, cp);
    uint32_t start, end;
    if (gid <= 0 || !loca_range(f, gid, &start, &end) || start == end) return 0;
    uint32_t g = f->glyf + start;
    if (!in_range(f, g, 10)) return 0;
    *ymin = rd_i16(f, g + 4);
    *ymax = rd_i16(f, g + 8);
    return 1;
}

// The median top (or foot) of the letters in `chars` this face has, so one
// odd letter (a serif face's 'Z' with a flourish) does not move the zone.
static int median_y(const struct ttf_font *f, const char *chars, int top, int *out) {
    int v[8], n = 0;
    for (const char *c = chars; *c && n < 8; c++) {
        int lo, hi;
        if (!glyph_ybox(f, (uint8_t)*c, &lo, &hi)) continue;
        int y = top ? hi : lo, k = n++;
        while (k > 0 && v[k - 1] > y) { v[k] = v[k - 1]; k--; }
        v[k] = y;
    }
    if (!n) return 0;
    *out = v[n / 2];
    return 1;
}

// One zone: flat letters for `ref`, round ones for `shoot`. An overshoot
// on the wrong side, or past 1/20 em, is not one (a font whose round
// letters sit lower than its flat ones), and the zone keeps ref only.
static void blue(struct ttf_font *f, int zone, const char *flat, const char *round, int top) {
    int ref, shoot;
    if (!median_y(f, flat, top, &ref)) return;
    if (!round || !median_y(f, round, top, &shoot)) shoot = ref;
    int d = top ? shoot - ref : ref - shoot;
    if (d < 0 || d > f->units_per_em / 20) shoot = ref;
    f->blue_ref[zone] = (int16_t)ref;
    f->blue_shoot[zone] = (int16_t)shoot;
    f->blue_ok |= (uint8_t)(1u << zone);
}

// The letters each zone is measured from are FreeType's latin autohinter's
// choice, cut down to what a bounding box can say.
static void find_blues(struct ttf_font *f) {
    blue(f, TTF_BLUE_BASE, "HEZLxz", "OCocs", 0);
    blue(f, TTF_BLUE_DESC, "pq", 0, 0);
    blue(f, TTF_BLUE_X, "xzuvw", "oces", 1);
    blue(f, TTF_BLUE_CAP, "HEZTL", "OCQS", 1);
    blue(f, TTF_BLUE_ASC, "bdhkl", 0, 1);
    // An x-height above the cap height is not a Latin face's.
    if ((f->blue_ok & (1u << TTF_BLUE_X)) && (f->blue_ok & (1u << TTF_BLUE_CAP)) &&
        f->blue_ref[TTF_BLUE_X] >= f->blue_ref[TTF_BLUE_CAP])
        f->blue_ok = 0;
}

void ttf_set_hinting(struct ttf_font *f, int on) {
    if (f) f->no_hint = (uint8_t)(on ? 0 : 1);
}

// Glyph flags (simple outlines)
#define ON_CURVE      0x01
#define X_SHORT       0x02
#define Y_SHORT       0x04
#define REPEAT_FLAG   0x08
#define X_SAME_OR_POS 0x10
#define Y_SAME_OR_POS 0x20

// Component flags (composite outlines)
#define ARG_1_AND_2_ARE_WORDS 0x0001
#define ARGS_ARE_XY_VALUES    0x0002
#define WE_HAVE_A_SCALE       0x0008
#define MORE_COMPONENTS       0x0020
#define X_AND_Y_SCALE         0x0040
#define TWO_BY_TWO            0x0080

static int decode_outline(const struct ttf_font *f, int gid,
                          int32_t dx_fu, int32_t dy_fu, fx_t scale,
                          fx_t ox, fx_t oy, struct ttf_scratch *sc, int depth);

static int decode_simple(const struct ttf_font *f, uint32_t g, uint32_t gend,
                         int ncont, int32_t dx_fu, int32_t dy_fu, fx_t scale,
                         fx_t ox, fx_t oy, struct ttf_scratch *sc) {
    if (ncont > TTF_MAX_CONTOURS - sc->ncontours) return 0;
    uint32_t p = g + 10;
    int base = sc->npoints;

    int npts = 0;
    for (int i = 0; i < ncont; i++) {
        int e = (int)rd_u16(f, p + (uint32_t)i * 2);
        if (e < npts - 1) return 0; // contour ends must not go backwards
        npts = e + 1;
        if (npts > TTF_MAX_POINTS - base) return 0;
        sc->contour_end[sc->ncontours + i] = base + e;
    }
    p += (uint32_t)ncont * 2;
    uint32_t ilen = rd_u16(f, p);
    p += 2 + ilen; // instructions: not run -- ttf_hint.c fits the outline itself
    if (p > gend) return 0;

    // Flags, with the repeat encoding expanded. In the SCRATCH struct
    // rather than on the stack: TTF_MAX_POINTS bytes is the kernel's
    // entire 1 KiB frame budget on its own.
    uint8_t *flags = sc->flags;
    for (int i = 0; i < npts; ) {
        if (p >= gend) return 0;
        uint8_t fl = (uint8_t)rd_u8(f, p++);
        flags[i++] = fl;
        if (fl & REPEAT_FLAG) {
            uint32_t rep = rd_u8(f, p++);
            while (rep-- && i < npts) flags[i++] = fl;
        }
    }

    // x then y, each a delta chain. The three-way encoding (short and
    // signed by a flag bit, or "same as previous", or a signed short)
    // is why this cannot be a simple loop over fixed-size records.
    int32_t v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = flags[i];
        if (fl & X_SHORT) {
            uint32_t d = rd_u8(f, p++);
            v += (fl & X_SAME_OR_POS) ? (int32_t)d : -(int32_t)d;
        } else if (!(fl & X_SAME_OR_POS)) {
            v += rd_i16(f, p); p += 2;
        }
        sc->px[base + i] = ox + fx_mul(fx_from_int(v + dx_fu), scale);
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = flags[i];
        if (fl & Y_SHORT) {
            uint32_t d = rd_u8(f, p++);
            v += (fl & Y_SAME_OR_POS) ? (int32_t)d : -(int32_t)d;
        } else if (!(fl & Y_SAME_OR_POS)) {
            v += rd_i16(f, p); p += 2;
        }
        // The y flip lives here: font y grows upward from the baseline,
        // a bitmap row index grows downward from the top.
        sc->py[base + i] = oy - fx_mul(fx_from_int(v + dy_fu), scale);
        sc->on[base + i] = (uint8_t)((flags[i] & ON_CURVE) ? 1 : 0);
    }
    if (p > f->len) return 0;

    sc->npoints = base + npts;
    sc->ncontours += ncont;
    return 1;
}

static int decode_composite(const struct ttf_font *f, uint32_t g, uint32_t gend,
                            int32_t dx_fu, int32_t dy_fu, fx_t scale,
                            fx_t ox, fx_t oy, struct ttf_scratch *sc, int depth) {
    uint32_t p = g + 10;
    for (;;) {
        if (p + 4 > gend) return 0;
        uint32_t flags = rd_u16(f, p);
        int comp = (int)rd_u16(f, p + 2);
        p += 4;
        int32_t a1, a2;
        if (flags & ARG_1_AND_2_ARE_WORDS) {
            a1 = rd_i16(f, p); a2 = rd_i16(f, p + 2); p += 4;
        } else {
            a1 = (int8_t)rd_u8(f, p); a2 = (int8_t)rd_u8(f, p + 1); p += 2;
        }
        // A 2x2 transform on a component is SKIPPED, not applied: the
        // Latin coverage this loads uses composites only for accents
        // (a base letter plus a diaeresis, offset), and every one of
        // those is a pure translation. A scaled component therefore
        // renders at its natural size in the right place rather than
        // being refused outright -- wrong, but wrong in a way a reader
        // can see, and the roadmap carries applying it properly.
        if (flags & WE_HAVE_A_SCALE) p += 2;
        else if (flags & X_AND_Y_SCALE) p += 4;
        else if (flags & TWO_BY_TWO) p += 8;

        int32_t cdx = 0, cdy = 0;
        if (flags & ARGS_ARE_XY_VALUES) { cdx = a1; cdy = a2; }
        // Point-matching (the else case) is not implemented: it is
        // vanishingly rare and needs the parent's points already placed.

        if (!decode_outline(f, comp, dx_fu + cdx, dy_fu + cdy, scale, ox, oy, sc, depth + 1))
            return 0;
        if (!(flags & MORE_COMPONENTS)) break;
    }
    return 1;
}

static int decode_outline(const struct ttf_font *f, int gid,
                          int32_t dx_fu, int32_t dy_fu, fx_t scale,
                          fx_t ox, fx_t oy, struct ttf_scratch *sc, int depth) {
    if (depth > 4) return 0; // a composite cycle is a hostile font, not a deep one
    uint32_t start, end;
    if (!loca_range(f, gid, &start, &end)) return 0;
    if (start == end) return 1; // no outline: a space, legitimately

    uint32_t g = f->glyf + start;
    uint32_t gend = f->glyf + end;
    if (!in_range(f, g, 10)) return 0;
    int ncont = (int)rd_i16(f, g);
    if (ncont >= 0)
        return decode_simple(f, g, gend, ncont, dx_fu, dy_fu, scale, ox, oy, sc);
    return decode_composite(f, g, gend, dx_fu, dy_fu, scale, ox, oy, sc, depth);
}

// --- flattening ------------------------------------------------------

static void add_edge(struct ttf_scratch *sc, fx_t x0, fx_t y0, fx_t x1, fx_t y1) {
    if (y0 == y1) return; // a horizontal edge crosses no scanline
    if (sc->nedges >= TTF_MAX_EDGES) return;
    struct ttf_edge *e = &sc->edges[sc->nedges++];
    e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1;
    e->dir = y1 > y0 ? 1 : -1;
}

static fx_t fx_abs(fx_t v) { return v < 0 ? -v : v; }

// Flattens one quadratic into line segments. The count comes from the
// control polygon's length in pixels, so a small glyph costs a few
// segments and a large one gets a smooth curve -- a fixed count is
// either visibly faceted at 24px or wasteful at 8px.
static void add_quad(struct ttf_scratch *sc, fx_t x0, fx_t y0,
                     fx_t cx, fx_t cy, fx_t x1, fx_t y1) {
    fx_t d = fx_abs(cx - x0) + fx_abs(cy - y0) + fx_abs(x1 - cx) + fx_abs(y1 - cy);
    int n = 1 + fx_to_int(d) / 2;
    if (n < 2) n = 2;
    if (n > 24) n = 24;
    fx_t pxp = x0, pyp = y0;
    for (int i = 1; i <= n; i++) {
        fx_t t = fx_div(fx_from_int(i), fx_from_int(n));
        fx_t u = FX_ONE - t;
        // B(t) = u^2*p0 + 2ut*c + t^2*p1
        fx_t uu = fx_mul(u, u), tt = fx_mul(t, t), ut2 = 2 * fx_mul(u, t);
        fx_t nx = fx_mul(uu, x0) + fx_mul(ut2, cx) + fx_mul(tt, x1);
        fx_t ny = fx_mul(uu, y0) + fx_mul(ut2, cy) + fx_mul(tt, y1);
        add_edge(sc, pxp, pyp, nx, ny);
        pxp = nx; pyp = ny;
    }
}

static fx_t mid(fx_t a, fx_t b) { return (a + b) / 2; }

// Walks one contour's points into edges. The TrueType convention this
// implements: consecutive OFF-curve points have an IMPLIED on-curve
// point at their midpoint, and a contour may start off-curve (in which
// case the start is itself an implied midpoint). Getting either wrong
// produces a glyph that is recognisable but subtly wrong, which is
// worse than one that is obviously broken.
static void flatten_contour(struct ttf_scratch *sc, int first, int last) {
    int n = last - first + 1;
    if (n < 2) return;

    fx_t sx, sy;
    int i0;
    if (sc->on[first]) {
        sx = sc->px[first]; sy = sc->py[first]; i0 = first + 1;
    } else if (sc->on[last]) {
        sx = sc->px[last]; sy = sc->py[last]; i0 = first;
    } else {
        sx = mid(sc->px[last], sc->px[first]);
        sy = mid(sc->py[last], sc->py[first]);
        i0 = first;
    }

    fx_t cx = sx, cy = sy;
    for (int k = 0; k < n; k++) {
        int i = first + ((i0 - first) + k) % n;
        if (sc->on[i]) {
            add_edge(sc, cx, cy, sc->px[i], sc->py[i]);
            cx = sc->px[i]; cy = sc->py[i];
        } else {
            int j = first + ((i0 - first) + k + 1) % n;
            fx_t ex, ey;
            if (sc->on[j]) { ex = sc->px[j]; ey = sc->py[j]; k++; }
            else { ex = mid(sc->px[i], sc->px[j]); ey = mid(sc->py[i], sc->py[j]); }
            add_quad(sc, cx, cy, sc->px[i], sc->py[i], ex, ey);
            cx = ex; cy = ey;
        }
    }
    add_edge(sc, cx, cy, sx, sy); // close it: an open contour fills wrongly
}

// --- scanline fill ---------------------------------------------------

// Adds the horizontal coverage of the span [xa, xb) on one sub-scanline
// into the row accumulator, in Q16.16 units of "fraction of a pixel".
static void add_span(struct ttf_scratch *sc, int w, fx_t xa, fx_t xb) {
    if (xb <= xa) return;
    if (xa < 0) xa = 0;
    if (xb > fx_from_int(w)) xb = fx_from_int(w);
    if (xb <= xa) return;

    int pa = fx_to_int(xa);
    int pb = fx_to_int(xb - 1); // last pixel the span touches
    if (pb >= w) pb = w - 1;
    if (pa == pb) {
        sc->acc[pa] += xb - xa;
        return;
    }
    sc->acc[pa] += fx_from_int(pa + 1) - xa;
    for (int p = pa + 1; p < pb; p++) sc->acc[p] += FX_ONE;
    sc->acc[pb] += xb - fx_from_int(pb);
}

int ttf_glyph_extent(const struct ttf_font *f, int gid, int px, int *up, int *down,
                     struct ttf_scratch *sc) {
    *up = *down = 0;
    if (!f || !sc) return 0;
    fx_t scale = ttf_scale_for_px(f, px);
    if (!scale) return 0;
    sc->npoints = sc->ncontours = sc->nedges = 0;
    if (!decode_outline(f, gid, 0, 0, scale, 0, 0, sc, 0)) return 0;
    if (sc->npoints == 0) return 1;
    if (!f->no_hint) ttf_hint_outline(f, scale, 0, sc);
    fx_t lo = sc->py[0], hi = sc->py[0];
    for (int i = 1; i < sc->npoints; i++) {
        if (sc->py[i] < lo) lo = sc->py[i];
        if (sc->py[i] > hi) hi = sc->py[i];
    }
    // y grows downward from the baseline at 0: the top is -lo, rounded up.
    fx_t lo_row = lo & ~(fx_t)(FX_ONE - 1);
    fx_t hi_row = (hi + FX_ONE - 1) & ~(fx_t)(FX_ONE - 1);
    *up = lo_row < 0 ? fx_to_int(-lo_row) : 0;
    *down = hi_row > 0 ? fx_to_int(hi_row) : 0;
    return 1;
}

int ttf_render_glyph(const struct ttf_font *f, int gid, int px,
                     uint8_t *cov, int w, int h,
                     int x_origin, int baseline,
                     struct ttf_scratch *sc) {
    if (!f || !cov || !sc || w <= 0 || h <= 0 || w > TTF_MAX_CELL_W) return 0;
    fx_t scale = ttf_scale_for_px(f, px);
    if (!scale) return 0;

    sc->npoints = 0;
    sc->ncontours = 0;
    sc->nedges = 0;
    if (!decode_outline(f, gid, 0, 0, scale,
                        fx_from_int(x_origin), fx_from_int(baseline), sc, 0))
        return 0;
    if (sc->npoints == 0) return 1; // a space: nothing to fill
    if (!f->no_hint) ttf_hint_outline(f, scale, fx_from_int(baseline), sc);

    // Sub-scanlines by size: 16 where a row is a big share of a stroke,
    // fewer where rows are many and each costs a pass over every edge.
    int subs = px <= 32 ? TTF_SUBSAMPLES : px <= 64 ? TTF_SUBSAMPLES / 2 : TTF_SUBSAMPLES / 4;
    if (subs < 1) subs = 1;

    int first = 0;
    for (int c = 0; c < sc->ncontours; c++) {
        int last = sc->contour_end[c];
        if (last >= sc->npoints) break;
        flatten_contour(sc, first, last);
        first = last + 1;
    }
    if (sc->nedges == 0) return 1;

    for (int row = 0; row < h; row++) {
        for (int i = 0; i < w; i++) sc->acc[i] = 0;
        int any = 0;

        for (int s = 0; s < subs; s++) {
            fx_t sy = fx_from_int(row) + (fx_t)((2 * s + 1) * FX_ONE / (2 * subs));
            int nx = 0;
            for (int e = 0; e < sc->nedges; e++) {
                const struct ttf_edge *ed = &sc->edges[e];
                fx_t y0 = ed->y0, y1 = ed->y1;
                fx_t lo = y0 < y1 ? y0 : y1;
                fx_t hi = y0 < y1 ? y1 : y0;
                // Half-open in y so a vertex shared by two edges is
                // counted exactly once -- counting it twice puts a
                // one-pixel notch in a diagonal.
                if (sy < lo || sy >= hi) continue;
                fx_t t = fx_div(sy - y0, y1 - y0);
                fx_t x = ed->x0 + fx_mul(t, ed->x1 - ed->x0);
                // Insertion sort as we go: an outline crosses one
                // scanline a handful of times, so this beats sorting
                // afterwards and needs no second pass.
                int k = nx++;
                while (k > 0 && sc->xs[k - 1] > x) {
                    sc->xs[k] = sc->xs[k - 1];
                    sc->xdir[k] = sc->xdir[k - 1];
                    k--;
                }
                sc->xs[k] = x;
                sc->xdir[k] = ed->dir;
                if (nx >= TTF_MAX_EDGES) break;
            }

            // Nonzero winding: inside wherever the accumulated
            // direction is not zero. (TrueType is nonzero, not
            // even-odd -- with even-odd, an 'o' fills solid.)
            int wind = 0;
            fx_t span_start = 0;
            for (int i = 0; i < nx; i++) {
                int prev = wind;
                wind += sc->xdir[i];
                if (prev == 0 && wind != 0) span_start = sc->xs[i];
                else if (prev != 0 && wind == 0) { add_span(sc, w, span_start, sc->xs[i]); any = 1; }
            }
        }
        if (!any) continue;

        uint8_t *out = cov + (size_t)row * (size_t)w;
        for (int i = 0; i < w; i++) {
            if (sc->acc[i] <= 0) continue;
            int32_t a = (int32_t)(((int64_t)sc->acc[i] * 255) / ((int64_t)subs * FX_ONE));
            if (a > 255) a = 255;
            if (a > out[i]) out[i] = (uint8_t)a;
        }
    }
    return 1;
}

int ttf_family(const struct ttf_font *f, char *out, int cap) {
    if (!f->name || f->name_len < 6 || cap < 1) return -1;
    uint32_t count = rd_u16(f, f->name + 2), strings = f->name + rd_u16(f, f->name + 4);
    int best = -1, best_rank = 0;
    for (uint32_t i = 0; i < count && 6 + (i + 1) * 12 <= f->name_len; i++) {
        uint32_t r = f->name + 6 + i * 12;
        uint32_t platform = rd_u16(f, r), enc = rd_u16(f, r + 2), lang = rd_u16(f, r + 4), id = rd_u16(f, r + 6);
        if (id != 1) continue;
        // Windows' Unicode English first, then any Windows Unicode record, then a Macintosh Roman one
        int rank = platform == 3 && enc == 1 ? (lang == 0x409 ? 3 : 2) : platform == 1 && enc == 0 ? 1 : 0;
        if (rank > best_rank) { best_rank = rank; best = (int)i; }
    }
    if (best < 0) return -1;
    uint32_t r = f->name + 6 + (uint32_t)best * 12;
    uint32_t len = rd_u16(f, r + 8), at = strings + rd_u16(f, r + 10);
    if (!in_range(f, at, len)) return -1;
    int two = best_rank >= 2, n = 0;                // Windows' records are UTF-16BE
    for (uint32_t k = 0; k + (two ? 1 : 0) < len && n < cap - 1; k += two ? 2 : 1) {
        uint32_t c = two ? rd_u16(f, at + k) : rd_u8(f, at + k);
        out[n++] = (char)(c >= 0x20 && c < 0x7F ? c : '?');
    }
    out[n] = 0;
    return n;
}

int ttf_style(const struct ttf_font *f) { return (int)(rd_u16(f, f->head + 44) & (TTF_BOLD | TTF_ITALIC)); }

void ttf_win_metrics(const struct ttf_font *f, int *ascent, int *descent) {
    *ascent = f->ascent;
    *descent = f->descent;
    if (f->os2 && f->os2_len >= 78) {
        int a = (int)rd_u16(f, f->os2 + 74), d = (int)rd_u16(f, f->os2 + 76);
        if (a > 0) { *ascent = a; *descent = d; }
    }
}

int ttf_vdmx(const struct ttf_font *f, int ppem, int *ymax, int *ymin) {
    if (!f->vdmx || f->vdmx_len < 6) return 0;
    uint32_t nratios = rd_u16(f, f->vdmx + 4), base = f->vdmx + 6;
    if (6 + nratios * 6 > f->vdmx_len) return 0;
    for (uint32_t i = 0; i < nratios; i++) {
        // a square-pixel screen: the record taking every ratio (xRatio 0), or 1:1 itself
        uint32_t x = rd_u8(f, base + 4 * i + 1), y0 = rd_u8(f, base + 4 * i + 2), y1 = rd_u8(f, base + 4 * i + 3);
        if (x && !(y0 <= x && x <= y1)) continue;
        uint32_t g = f->vdmx + rd_u16(f, base + 4 * nratios + 2 * i);
        uint32_t recs = rd_u16(f, g);
        if (!in_range(f, g, 4 + recs * 6) || g + 4 + recs * 6 > f->vdmx + f->vdmx_len) return 0;
        for (uint32_t k = 0; k < recs; k++)
            if ((int)rd_u16(f, g + 4 + 6 * k) == ppem) {
                *ymax = rd_i16(f, g + 6 + 6 * k);
                *ymin = rd_i16(f, g + 8 + 6 * k);
                return 1;
            }
        return 0;
    }
    return 0;
}

int ttf_hdmx(const struct ttf_font *f, int ppem, int gid, int *advance) {
    if (!f->hdmx || f->hdmx_len < 8 || gid < 0 || gid >= f->num_glyphs) return 0;
    uint32_t n = rd_u16(f, f->hdmx + 2), size = rd_u32(f, f->hdmx + 4);
    if (size < 2 + (uint32_t)f->num_glyphs || size > f->hdmx_len) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = f->hdmx + 8 + i * size;
        if (r + size > f->hdmx + f->hdmx_len) return 0;
        if ((int)rd_u8(f, r) == ppem) { *advance = (int)rd_u8(f, r + 2 + (uint32_t)gid); return 1; }
    }
    return 0;
}

// --- colour glyphs: COLR v0 over CPAL ----------------------------------

// The layers of `gid`: *first (an index into the layer records) and the
// count. 0 when the font has no usable COLR v0 or `gid` is not a colour
// glyph. Base glyph records are sorted by glyph id, so a binary search.
static int colr_layers(const struct ttf_font *f, int gid, uint32_t *first, uint32_t *n) {
    if (!f->colr || !f->cpal || f->colr_len < 14 || rd_u16(f, f->colr) != 0) return 0;
    uint32_t nbase = rd_u16(f, f->colr + 2);
    uint32_t base_rel = rd_u32(f, f->colr + 4), layer_rel = rd_u32(f, f->colr + 8);
    uint32_t nlayers = rd_u16(f, f->colr + 12);
    // Both arrays inside COLR itself, not merely inside the file: a table
    // that points past its own end is malformed, and its colours a guess.
    if (base_rel > f->colr_len || nbase * 6 > f->colr_len - base_rel ||
        layer_rel > f->colr_len || nlayers * 4 > f->colr_len - layer_rel) return 0;
    uint32_t base_off = f->colr + base_rel;
    int lo = 0, hi = (int)nbase - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t r = base_off + (uint32_t)mid * 6;
        int g = (int)rd_u16(f, r);
        if (g == gid) {
            *first = rd_u16(f, r + 2);
            *n = rd_u16(f, r + 4);
            // A run past the layer array is a malformed font: no colour.
            return *n > 0 && *first + *n <= nlayers;
        }
        if (g < gid) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

// Layer `i`'s glyph and its colour as 0xAARRGGBB (straight alpha):
// CPAL palette 0's entry, or `fg` for the foreground index 0xFFFF.
static int colr_layer(const struct ttf_font *f, uint32_t i, uint32_t fg,
                      int *layer_gid, uint32_t *argb) {
    uint32_t layer_off = f->colr + rd_u32(f, f->colr + 8);   // checked by colr_layers()
    uint32_t r = layer_off + i * 4;
    *layer_gid = (int)rd_u16(f, r);
    uint32_t pal = rd_u16(f, r + 2);
    if (pal == 0xFFFF) { *argb = fg | 0xFF000000u; return 1; }
    // CPAL's header and palette 0's index (12 + 2 per palette), then the
    // colour records, all inside the table.
    if (f->cpal_len < 14) return 0;
    uint32_t entries = rd_u16(f, f->cpal + 2);
    uint32_t palettes = rd_u16(f, f->cpal + 4);
    uint32_t records = rd_u16(f, f->cpal + 6);
    uint32_t rec_rel = rd_u32(f, f->cpal + 8);
    if (palettes < 1 || 12 + 2 * palettes > f->cpal_len) return 0;
    if (rec_rel > f->cpal_len || records * 4 > f->cpal_len - rec_rel) return 0;
    uint32_t first = rd_u16(f, f->cpal + 12);   // palette 0's first record
    if (pal >= entries || first + pal >= records) return 0;
    uint32_t c = f->cpal + rec_rel + (first + pal) * 4;   // B, G, R, A
    *argb = rd_u8(f, c + 3) << 24 | rd_u8(f, c + 2) << 16 | rd_u8(f, c + 1) << 8 | rd_u8(f, c);
    return 1;
}

int ttf_render_color_glyph(const struct ttf_font *f, int gid, int px,
                           uint8_t *rgba, uint8_t *cov, int w, int h,
                           int x_origin, int baseline, uint32_t fg,
                           struct ttf_scratch *sc) {
    uint32_t first, n;
    if (!f || !rgba || !cov || w <= 0 || h <= 0) return -1;
    if (!colr_layers(f, gid, &first, &n)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        int lg;
        uint32_t c;
        if (!colr_layer(f, first + i, fg, &lg, &c)) return -1;
        for (int k = 0; k < w * h; k++) cov[k] = 0;
        if (!ttf_render_glyph(f, lg, px, cov, w, h, x_origin, baseline, sc)) return -1;
        uint32_t ca = c >> 24, cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
        // Each layer over the ones below it, premultiplied: what is drawn
        // is out = src + under * (1 - src alpha).
        for (int k = 0; k < w * h; k++) {
            uint32_t a = cov[k] * ca / 255;
            if (!a) continue;
            uint8_t *p = rgba + (size_t)k * 4;
            uint32_t keep = 255 - a;
            p[0] = (uint8_t)(cr * a / 255 + p[0] * keep / 255);
            p[1] = (uint8_t)(cg * a / 255 + p[1] * keep / 255);
            p[2] = (uint8_t)(cb * a / 255 + p[2] * keep / 255);
            p[3] = (uint8_t)(a + p[3] * keep / 255);
        }
    }
    return 1;
}

int ttf_color_glyph_extent(const struct ttf_font *f, int gid, int px,
                           int *up, int *down, struct ttf_scratch *sc) {
    uint32_t first, n;
    *up = *down = 0;
    if (!f || !colr_layers(f, gid, &first, &n)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        int lg, u, d;
        uint32_t c;
        if (!colr_layer(f, first + i, 0, &lg, &c)) return 0;
        if (!ttf_glyph_extent(f, lg, px, &u, &d, sc)) continue;
        if (u > *up) *up = u;
        if (d > *down) *down = d;
    }
    return 1;
}
