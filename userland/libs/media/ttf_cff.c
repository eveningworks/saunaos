// CFF outlines: the `CFF ` table of an OpenType font whose version tag is
// 'OTTO', read for ttf.c. A glyph is a Type 2 CHARSTRING -- a small stack
// program of moves, lines and cubic curves, with subroutines -- and this
// runs it into the same scaled outline ttf.c's glyf decoder fills, the
// curves as TTF_PT_CUBIC pairs, so hinting, flattening and the fill are
// shared. Adobe's CFF spec (TN #5176) and the Type 2 charstring format
// (TN #5177) are the references; FreeType's cff driver and stb_truetype
// read it the same way.
//
// BOTH KINDS OF CFF: a NAME-KEYED font (one Private DICT, its local
// subroutines found once at open) and a CID-KEYED one -- what every CJK
// font is -- whose glyphs are split over several font DICTs, FDSelect
// naming each glyph's, each with its own Private DICT and subroutines.
//
// NOT READ, deliberately: the charstrings' stem hints (ttf_hint.c fits
// every face, FreeType's "light" mode does the same for CFF), the
// FontMatrix (unitsPerEm from `head` is the scale, as every OpenType CFF
// font sets it), seac accents (deprecated in OpenType CFF; such a glyph is
// refused), the deprecated arithmetic operators (refused), and CFF2, the
// variable-font format, which is a different table.
//
// HOSTILE INPUT, as all of ttf.c is: every read is rd_*()'s, every INDEX
// item is checked against the table's end, subroutine calls nest at most
// CS_DEPTH deep, a glyph runs at most CS_BUDGET operators -- nesting
// alone would let a font make one glyph cost billions -- and a point past
// CS_COORD ems refuses it. tools/hostchecks/cff_hostcheck.py fuzzes it.
#include "ttf.h"
#include "ttf_internal.h"

#define CS_STACK  48      // the Type 2 argument stack's limit (TN #5177)
#define CS_DEPTH  10      // subroutine nesting (the same)
#define CS_BUDGET 100000  // operators and operands one glyph may run
#define CS_COORD  4       // |coordinate| past this many ems refuses the glyph: a point
                          // scaled to pixels then stays far inside 16.16

// --- INDEX ------------------------------------------------------------

static uint32_t rd_off(const struct ttf_font *f, uint32_t off, uint32_t osz) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < osz; i++) v = (v << 8) | rd_u8(f, off + i);
    return v;
}

static uint32_t cff_end(const struct ttf_font *f) { return f->cff + f->cff_len; }

// The INDEX at `off`, and where the next structure starts (*next). An
// INDEX of no items is two bytes.
static int cff_index(const struct ttf_font *f, uint32_t off, struct ttf_cff_index *ix,
                     uint32_t *next) {
    uint32_t lim = cff_end(f);
    struct ttf_cff_index z = { 0, 0, 0, 0 };
    *ix = z;
    if (off < f->cff || off > lim || lim - off < 2) return 0;
    uint32_t count = rd_u16(f, off);
    if (count == 0) {
        if (next) *next = off + 2;
        return 1;
    }
    if (lim - off < 3) return 0;
    uint32_t osz = rd_u8(f, off + 2);
    if (osz < 1 || osz > 4) return 0;
    uint32_t offs = off + 3;
    uint64_t tab = (uint64_t)(count + 1) * osz;
    if (tab > lim - offs) return 0;
    ix->count = count;
    ix->osz = (uint8_t)osz;
    ix->offs = offs;
    ix->data = offs + (uint32_t)tab - 1;   // item offsets count from 1
    uint32_t last = rd_off(f, offs + count * osz, osz);
    if (last < 1 || last > lim - ix->data) return 0;
    if (next) *next = ix->data + last;
    return 1;
}

// Item `i`'s bytes, [*start, *end), inside the table.
static int cff_item(const struct ttf_font *f, const struct ttf_cff_index *ix, uint32_t i,
                    uint32_t *start, uint32_t *end) {
    if (i >= ix->count) return 0;
    uint32_t a = rd_off(f, ix->offs + i * ix->osz, ix->osz);
    uint32_t b = rd_off(f, ix->offs + (i + 1) * ix->osz, ix->osz);
    if (a < 1 || b < a || b > cff_end(f) - ix->data) return 0;
    *start = ix->data + a;
    *end = ix->data + b;
    return 1;
}

// --- DICT ---------------------------------------------------------------

// The DICT keys read; -1 for absent. Offsets count from the table's start
// (a Private DICT's Subrs from the Private DICT's).
struct cff_dict {
    int32_t charstrings, priv_size, priv_off, subrs, fdarray, fdselect, cstype;
    int ros;
};

#define OP2(b) (1200 + (b))   // a two-byte operator, 12 b

static int dict_parse(const struct ttf_font *f, uint32_t p, uint32_t end, struct cff_dict *d) {
    d->charstrings = d->priv_size = d->priv_off = d->subrs = d->fdarray = d->fdselect = -1;
    d->cstype = 2;
    d->ros = 0;
    int32_t v[CS_STACK];
    int n = 0;
    while (p < end) {
        uint32_t b0 = rd_u8(f, p);
        int32_t val;
        if (b0 <= 21) {
            int op = (int)b0;
            p++;
            if (b0 == 12) op = OP2(rd_u8(f, p++));
            switch (op) {
            case 17: if (n >= 1) d->charstrings = v[n - 1]; break;
            case 18: if (n >= 2) { d->priv_size = v[n - 2]; d->priv_off = v[n - 1]; } break;
            case 19: if (n >= 1) d->subrs = v[n - 1]; break;
            case OP2(6): if (n >= 1) d->cstype = v[n - 1]; break;
            case OP2(30): d->ros = 1; break;
            case OP2(36): if (n >= 1) d->fdarray = v[n - 1]; break;
            case OP2(37): if (n >= 1) d->fdselect = v[n - 1]; break;
            default: break;
            }
            n = 0;
            continue;
        }
        if (b0 == 28) { val = rd_i16(f, p + 1); p += 3; }
        else if (b0 == 29) { val = (int32_t)rd_u32(f, p + 1); p += 5; }
        else if (b0 == 30) {
            // A real number, nibble-coded to an 0xf nibble: no key read
            // here takes one, so it is skipped and counts as 0.
            for (p++; p < end; p++) {
                uint32_t b = rd_u8(f, p);
                if ((b >> 4) == 0xf || (b & 0xf) == 0xf) { p++; break; }
            }
            val = 0;
        }
        else if (b0 >= 32 && b0 <= 246) { val = (int32_t)b0 - 139; p += 1; }
        else if (b0 >= 247 && b0 <= 250) { val = ((int32_t)b0 - 247) * 256 + (int32_t)rd_u8(f, p + 1) + 108; p += 2; }
        else if (b0 >= 251 && b0 <= 254) { val = -((int32_t)b0 - 251) * 256 - (int32_t)rd_u8(f, p + 1) - 108; p += 2; }
        else return 0;   // reserved
        if (n == CS_STACK) return 0;
        v[n++] = val;
    }
    return 1;
}

// A DICT at `off` bytes into the table, `size` long; 0 if it leaves it.
static int dict_at(const struct ttf_font *f, int32_t off, int32_t size, struct cff_dict *d) {
    if (off < 0 || size < 0 || (uint32_t)off > f->cff_len || (uint32_t)size > f->cff_len - (uint32_t)off)
        return 0;
    return dict_parse(f, f->cff + (uint32_t)off, f->cff + (uint32_t)off + (uint32_t)size, d);
}

// The local subroutines of the Private DICT at (`off`, `size`): none is
// not an error -- a font that never calls one needs none.
static int private_subrs(const struct ttf_font *f, int32_t size, int32_t off,
                         struct ttf_cff_index *subrs) {
    struct ttf_cff_index z = { 0, 0, 0, 0 };
    *subrs = z;
    if (size <= 0) return 1;
    struct cff_dict pd;
    if (!dict_at(f, off, size, &pd)) return 0;
    if (pd.subrs <= 0) return 1;
    if ((uint32_t)pd.subrs > f->cff_len - (uint32_t)off) return 0;
    return cff_index(f, f->cff + (uint32_t)off + (uint32_t)pd.subrs, subrs, 0);
}

int ttf_cff_open(struct ttf_font *f) {
    uint32_t c = f->cff;
    if (f->cff_len < 4 || rd_u8(f, c) != 1) return 0;   // major version 1
    uint32_t hdr = rd_u8(f, c + 2);
    if (hdr < 4 || hdr > f->cff_len) return 0;
    struct ttf_cff_index names, top, strings;
    uint32_t p;
    if (!cff_index(f, c + hdr, &names, &p) || !cff_index(f, p, &top, &p) ||
        !cff_index(f, p, &strings, &p) || !cff_index(f, p, &f->cff_gsubrs, &p))
        return 0;
    uint32_t a, b;
    struct cff_dict d;
    if (!cff_item(f, &top, 0, &a, &b) || !dict_parse(f, a, b, &d)) return 0;
    if (d.cstype != 2 || d.charstrings <= 0 || (uint32_t)d.charstrings >= f->cff_len) return 0;
    if (!cff_index(f, c + (uint32_t)d.charstrings, &f->cff_chars, 0) || !f->cff_chars.count)
        return 0;
    // maxp and the charstrings should agree; a glyph id past either has
    // no outline.
    if (f->cff_chars.count < (uint32_t)f->num_glyphs) f->num_glyphs = (int)f->cff_chars.count;

    if (d.ros) {
        if (d.fdarray <= 0 || d.fdselect <= 0 || (uint32_t)d.fdarray >= f->cff_len ||
            (uint32_t)d.fdselect >= f->cff_len)
            return 0;
        if (!cff_index(f, c + (uint32_t)d.fdarray, &f->cff_fdarray, 0) || !f->cff_fdarray.count)
            return 0;
        f->cff_fdselect = c + (uint32_t)d.fdselect;
        uint32_t fmt = rd_u8(f, f->cff_fdselect);
        if (fmt != 0 && fmt != 3) return 0;
        return 1;
    }
    return private_subrs(f, d.priv_size, d.priv_off, &f->cff_subrs);
}

// FDSelect: which font DICT glyph `gid` belongs to; -1 for none.
static int fd_for(const struct ttf_font *f, int gid) {
    uint32_t p = f->cff_fdselect;
    if (rd_u8(f, p) == 0) {
        if ((uint32_t)gid >= cff_end(f) - (p + 1)) return -1;
        return (int)rd_u8(f, p + 1 + (uint32_t)gid);
    }
    // Format 3: ranges {first glyph, fd}, sorted, then a sentinel glyph.
    uint32_t n = rd_u16(f, p + 1);
    if (!n || !in_range(f, p + 3, n * 3 + 2)) return -1;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {   // the last range whose first glyph is <= gid
        uint32_t mid = (lo + hi) / 2;
        if (rd_u16(f, p + 3 + mid * 3) <= (uint32_t)gid) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return -1;
    uint32_t r = lo - 1, next = rd_u16(f, p + 3 + (r + 1) * 3);
    return (uint32_t)gid < next ? (int)rd_u8(f, p + 3 + r * 3 + 2) : -1;
}

// Glyph `gid`'s charstring and the local subroutines it calls.
static int glyph_program(const struct ttf_font *f, int gid, uint32_t *start, uint32_t *end,
                         struct ttf_cff_index *subrs) {
    if (gid < 0 || gid >= f->num_glyphs) return 0;
    if (!cff_item(f, &f->cff_chars, (uint32_t)gid, start, end)) return 0;
    if (!f->cff_fdselect) {
        *subrs = f->cff_subrs;
        return 1;
    }
    int fd = fd_for(f, gid);
    uint32_t a, b;
    struct cff_dict d;
    if (fd < 0 || !cff_item(f, &f->cff_fdarray, (uint32_t)fd, &a, &b) || !dict_parse(f, a, b, &d))
        return 0;
    return private_subrs(f, d.priv_size, d.priv_off, subrs);
}

// --- the charstring interpreter ------------------------------------------

struct cs {
    const struct ttf_font *f;
    struct ttf_scratch *sc;   // NULL: measuring a box instead
    fx_t scale, ox, oy;
    int64_t x, y;             // the current point, 16.16 font units
    int64_t sx, sy;           // the open contour's first point
    int open;                 // a contour is open
    int ymin, ymax, any;      // box mode, whole font units
    int bad;
};

static void cs_point(struct cs *c, int kind) {
    const int64_t lim = ((int64_t)CS_COORD * c->f->units_per_em) << 16;
    if (c->x > lim || c->x < -lim || c->y > lim || c->y < -lim) { c->bad = 1; return; }
    if (!c->sc) {
        int lo = (int)(c->y >> 16), hi = (int)((c->y + 0xFFFF) >> 16);
        if (!c->any || lo < c->ymin) c->ymin = lo;
        if (!c->any || hi > c->ymax) c->ymax = hi;
        c->any = 1;
        return;
    }
    struct ttf_scratch *sc = c->sc;
    if (sc->npoints >= TTF_MAX_POINTS) { c->bad = 1; return; }
    // y flips here, as in ttf.c's glyf decoder: font y grows up, rows down.
    sc->px[sc->npoints] = c->ox + fx_mul((fx_t)c->x, c->scale);
    sc->py[sc->npoints] = c->oy - fx_mul((fx_t)c->y, c->scale);
    sc->on[sc->npoints] = (uint8_t)kind;
    sc->npoints++;
}

// Ends the open contour. A closing curve or line that lands back on the
// first point repeats it; the copy is dropped, since flatten_contour()
// closes every contour and the hinter reads a repeated point as no
// extremum.
static void cs_close(struct cs *c) {
    if (!c->open) return;
    c->open = 0;
    if (!c->sc) return;
    struct ttf_scratch *sc = c->sc;
    int first = sc->ncontours ? sc->contour_end[sc->ncontours - 1] + 1 : 0;
    if (sc->npoints - first > 1 && sc->on[sc->npoints - 1] == TTF_PT_ON &&
        c->x == c->sx && c->y == c->sy)
        sc->npoints--;
    if (sc->npoints == first) return;
    sc->contour_end[sc->ncontours++] = sc->npoints - 1;
}

static void cs_moveto(struct cs *c, int64_t dx, int64_t dy) {
    cs_close(c);
    if (c->sc && c->sc->ncontours >= TTF_MAX_CONTOURS) { c->bad = 1; return; }
    c->x += dx;
    c->y += dy;
    c->sx = c->x;
    c->sy = c->y;
    c->open = 1;
    cs_point(c, TTF_PT_ON);
}

static void cs_line(struct cs *c, int64_t dx, int64_t dy) {
    if (!c->open) { c->bad = 1; return; }
    c->x += dx;
    c->y += dy;
    cs_point(c, TTF_PT_ON);
}

static void cs_curve(struct cs *c, int64_t dxa, int64_t dya, int64_t dxb, int64_t dyb,
                     int64_t dxc, int64_t dyc) {
    if (!c->open) { c->bad = 1; return; }
    c->x += dxa; c->y += dya; cs_point(c, TTF_PT_CUBIC);
    c->x += dxb; c->y += dyb; cs_point(c, TTF_PT_CUBIC);
    c->x += dxc; c->y += dyc; cs_point(c, TTF_PT_ON);
}

static int32_t subr_bias(uint32_t count) {
    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

// Runs one glyph's program. 1 at endchar (or the program's end), 0 for
// anything malformed, unsupported or over budget.
static int cs_run(struct cs *c, uint32_t p, uint32_t end, const struct ttf_cff_index *lsubrs) {
    const struct ttf_font *f = c->f;
    int32_t st[CS_STACK];   // 16.16
    int n = 0, nstems = 0, have_width = 0;
    struct { uint32_t p, end; } calls[CS_DEPTH];
    int depth = 0;
    long budget = CS_BUDGET;
    int32_t lbias = subr_bias(lsubrs->count), gbias = subr_bias(f->cff_gsubrs.count);

    for (;;) {
        if (c->bad || --budget < 0) return 0;
        if (p >= end) {
            if (!depth) { cs_close(c); return 1; }   // no endchar: the glyph is what was drawn
            depth--;
            p = calls[depth].p;
            end = calls[depth].end;
            continue;
        }
        uint32_t b0 = rd_u8(f, p);
        if (b0 >= 32 || b0 == 28) {
            int32_t v;
            if (b0 == 28) { v = rd_i16(f, p + 1) * 65536; p += 3; }
            else if (b0 <= 246) { v = ((int32_t)b0 - 139) * 65536; p += 1; }
            else if (b0 <= 250) { v = (((int32_t)b0 - 247) * 256 + (int32_t)rd_u8(f, p + 1) + 108) * 65536; p += 2; }
            else if (b0 <= 254) { v = (-((int32_t)b0 - 251) * 256 - (int32_t)rd_u8(f, p + 1) - 108) * 65536; p += 2; }
            else { v = (int32_t)rd_u32(f, p + 1); p += 5; }   // 255: 16.16 fixed
            if (n == CS_STACK) return 0;
            st[n++] = v;
            continue;
        }
        int op = (int)b0;
        p++;
        if (b0 == 12) op = OP2(rd_u8(f, p++));
        int base = 0;
        // dispatch-ok: Type 2's operator set is fixed by the spec (TN #5177), and
        // each case works the interpreter's own stack and point in place.
        switch (op) {
        case 1: case 3: case 18: case 23:   // hstem vstem hstemhm vstemhm
            if (!have_width && (n & 1)) base = 1;
            have_width = 1;
            nstems += (n - base) / 2;
            break;
        case 19: case 20: {                 // hintmask cntrmask: the stems' bits
            if (!have_width && (n & 1)) base = 1;
            have_width = 1;
            nstems += (n - base) / 2;       // implied vstems
            p += (uint32_t)(nstems + 7) / 8;
            break;
        }
        case 21:                            // rmoveto
            if (!have_width && n > 2) base = 1;
            have_width = 1;
            if (n - base < 2) return 0;
            cs_moveto(c, st[base], st[base + 1]);
            break;
        case 22:                            // hmoveto
            if (!have_width && n > 1) base = 1;
            have_width = 1;
            if (n - base < 1) return 0;
            cs_moveto(c, st[base], 0);
            break;
        case 4:                             // vmoveto
            if (!have_width && n > 1) base = 1;
            have_width = 1;
            if (n - base < 1) return 0;
            cs_moveto(c, 0, st[base]);
            break;
        case 14:                            // endchar
            if (!have_width && (n == 1 || n == 5)) base = 1;
            if (n - base >= 4) return 0;    // seac: an accented letter by reference -- not read
            cs_close(c);
            return !c->bad;
        case 5:                             // rlineto
            for (int i = 0; i + 1 < n; i += 2) cs_line(c, st[i], st[i + 1]);
            break;
        case 6: case 7: {                   // hlineto vlineto: alternating
            int horiz = op == 6;
            for (int i = 0; i < n; i++, horiz = !horiz)
                cs_line(c, horiz ? st[i] : 0, horiz ? 0 : st[i]);
            break;
        }
        case 8:                             // rrcurveto
            for (int i = 0; i + 5 < n; i += 6)
                cs_curve(c, st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
            break;
        case 27: {                          // hhcurveto: [dy1] {dxa dxb dyb dxc}+
            int i = 0;
            int64_t d1 = 0;
            if (n & 1) d1 = st[i++];
            for (; i + 3 < n; i += 4, d1 = 0) cs_curve(c, st[i], d1, st[i + 1], st[i + 2], st[i + 3], 0);
            break;
        }
        case 26: {                          // vvcurveto: [dx1] {dya dxb dyb dyc}+
            int i = 0;
            int64_t d1 = 0;
            if (n & 1) d1 = st[i++];
            for (; i + 3 < n; i += 4, d1 = 0) cs_curve(c, d1, st[i], st[i + 1], st[i + 2], 0, st[i + 3]);
            break;
        }
        case 30: case 31: {                 // vhcurveto hvcurveto: alternating, [df] last
            int vert = op == 30;
            for (int i = 0; i + 3 < n; i += 4, vert = !vert) {
                int64_t last = n - i == 5 ? st[i + 4] : 0;
                if (vert) cs_curve(c, 0, st[i], st[i + 1], st[i + 2], st[i + 3], last);
                else      cs_curve(c, st[i], 0, st[i + 1], st[i + 2], last, st[i + 3]);
            }
            break;
        }
        case 24: {                          // rcurveline: curves, then a line
            int i = 0;
            for (; i + 6 <= n - 2; i += 6)
                cs_curve(c, st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
            if (i + 1 < n) cs_line(c, st[i], st[i + 1]);
            break;
        }
        case 25: {                          // rlinecurve: lines, then a curve
            int i = 0;
            for (; i + 2 <= n - 6; i += 2) cs_line(c, st[i], st[i + 1]);
            if (i + 5 < n) cs_curve(c, st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
            break;
        }
        case OP2(35):                       // flex: two curves (+ a depth, ignored)
            if (n < 12) return 0;
            cs_curve(c, st[0], st[1], st[2], st[3], st[4], st[5]);
            cs_curve(c, st[6], st[7], st[8], st[9], st[10], st[11]);
            break;
        case OP2(34):                       // hflex
            if (n < 7) return 0;
            cs_curve(c, st[0], 0, st[1], st[2], st[3], 0);
            cs_curve(c, st[4], 0, st[5], -(int64_t)st[2], st[6], 0);
            break;
        case OP2(36):                       // hflex1
            if (n < 9) return 0;
            cs_curve(c, st[0], st[1], st[2], st[3], st[4], 0);
            cs_curve(c, st[5], 0, st[6], st[7], st[8], -((int64_t)st[1] + st[3] + st[7]));
            break;
        case OP2(37): {                     // flex1: the last point along the longer axis
            if (n < 11) return 0;
            int64_t dx = (int64_t)st[0] + st[2] + st[4] + st[6] + st[8];
            int64_t dy = (int64_t)st[1] + st[3] + st[5] + st[7] + st[9];
            int64_t ex = abs64(dx) > abs64(dy) ? st[10] : -dx;
            int64_t ey = abs64(dx) > abs64(dy) ? -dy : st[10];
            cs_curve(c, st[0], st[1], st[2], st[3], st[4], st[5]);
            cs_curve(c, st[6], st[7], st[8], st[9], ex, ey);
            break;
        }
        case 10: case 29: {                 // callsubr callgsubr
            if (!n || depth == CS_DEPTH) return 0;
            const struct ttf_cff_index *ix = op == 10 ? lsubrs : &f->cff_gsubrs;
            int64_t idx = (int64_t)(st[--n] >> 16) + (op == 10 ? lbias : gbias);
            uint32_t a, b;
            if (idx < 0 || !cff_item(f, ix, (uint32_t)idx, &a, &b)) return 0;
            calls[depth].p = p;
            calls[depth].end = end;
            depth++;
            p = a;
            end = b;
            continue;                       // the arguments below it stay
        }
        case 11:                            // return
            if (!depth) return 0;
            depth--;
            p = calls[depth].p;
            end = calls[depth].end;
            continue;
        default:
            return 0;                       // reserved, or a deprecated operator
        }
        n = 0;
    }
}

int ttf_cff_decode(const struct ttf_font *f, int gid, fx_t scale, fx_t ox, fx_t oy,
                   struct ttf_scratch *sc) {
    uint32_t a, b;
    struct ttf_cff_index subrs;
    if (!glyph_program(f, gid, &a, &b, &subrs)) return 0;
    struct cs c = { .f = f, .sc = sc, .scale = scale, .ox = ox, .oy = oy };
    return cs_run(&c, a, b, &subrs) && !c.bad;
}

int ttf_cff_ybox(const struct ttf_font *f, int gid, int *ymin, int *ymax) {
    uint32_t a, b;
    struct ttf_cff_index subrs;
    if (!glyph_program(f, gid, &a, &b, &subrs)) return 0;
    struct cs c = { .f = f };
    if (!cs_run(&c, a, b, &subrs) || c.bad || !c.any) return 0;
    *ymin = c.ymin;
    *ymax = c.ymax;
    return 1;
}
