// meta16.c -- metafiles: GDI's PlayMetaFile, and the pictures an OLE
// object shows (ole16.c), drawn record by record into a DC.
//
// lib/uwmf decodes and checks the records; each becomes the GDI call it
// recorded, MADE THROUGH w16's OWN EXPORT -- its arguments laid out below
// the program's stack as the program would have pushed them -- so a
// metafile draws exactly as the program's own calls do, mapping and clip
// included. Strings, points and structures go in the sysdata scratch.
// The metafile's objects are deleted and the DC's state restored after.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "lib/uwmf.h"

#define GDI_SETBKCOLOR    1
#define GDI_SETBKMODE     2
#define GDI_SETMAPMODE    3
#define GDI_SETROP2       4
#define GDI_SETPOLYFILL   6
#define GDI_SETSTRETCH    7
#define GDI_SETTEXTCOLOR  9
#define GDI_SETJUSTIFY    10
#define GDI_SETWINDOWORG  11
#define GDI_SETWINDOWEXT  12
#define GDI_SETVPORG      13
#define GDI_SETVPEXT      14
#define GDI_LINETO        19
#define GDI_MOVETO        20
#define GDI_INTERSECTCLIP 22
#define GDI_ELLIPSE       24
#define GDI_RECTANGLE     27
#define GDI_PATBLT        29
#define GDI_SAVEDC        30
#define GDI_SETPIXEL      31
#define GDI_TEXTOUT       33
#define GDI_STRETCHBLT    35
#define GDI_POLYGON       36
#define GDI_POLYLINE      37
#define GDI_RESTOREDC     39
#define GDI_SELECTOBJECT  45
#define GDI_COMPATDC      52
#define GDI_FONTINDIRECT  57
#define GDI_HATCHBRUSH    58
#define GDI_PATTERNBRUSH  60
#define GDI_CREATEPEN     61
#define GDI_SOLIDBRUSH    66
#define GDI_DELETEDC      68
#define GDI_DELETEOBJECT  69
#define GDI_STOCKOBJECT   87
#define GDI_WINDOWEXT     96
#define GDI_WINDOWORG     97
#define GDI_VPEXT         94
#define GDI_VPORG         95
#define GDI_POLYPOLYGON   450

// GDI export `ordinal` called with `words` as the program pushes them, first
// pushed first (a long: high word, then low); DX:AX back.
static uint32_t gdi(struct w16 *w, unsigned ordinal, const uint16_t *words, unsigned n) {
    struct cpu16 *c = &w->cpu;
    uint16_t sp = c->r[R_SP], frame = (uint16_t)(sp - 2 * n - 4);
    for (unsigned i = 0; i < n; i++) {
        uint8_t *p = cpu16_ptr(c, c->sreg[S_SS], (uint16_t)(frame + 4 + 2 * (n - 1 - i)), 2, 1);
        if (!p) { c->fault = 0; return 0; }      // no room below the stack: the record is skipped
        p[0] = (uint8_t)words[i];
        p[1] = (uint8_t)(words[i] >> 8);
    }
    c->r[R_SP] = frame;
    struct w16_args a = { c, 2 * n };
    int r = w16_gdi(w, ordinal, &a);
    c->r[R_SP] = sp;
    if (r < 0) return 0;
    return (uint32_t)c->r[R_DX] << 16 | c->r[R_AX];
}

#define G(ord, ...) gdi(w, (ord), (const uint16_t[]){ __VA_ARGS__ }, \
                        sizeof (const uint16_t[]){ __VA_ARGS__ } / sizeof(uint16_t))
#define HI(v) (uint16_t)((uint32_t)(v) >> 16)
#define LO(v) (uint16_t)(v)

// `n` bytes into the sysdata scratch: its offset, 0 when it will not fit.
static uint16_t put(struct w16 *w, const void *p, unsigned n) {
    uint16_t at = w16_sys_push(w, n ? n : 1);
    if (at) memcpy(w->sysmem + at, p, n);
    else w->cpu.fault = 0;
    return at;
}

// A DIB's picture into a rectangle of the DC: through a memory DC holding it
// as a bitmap, then StretchBlt -- as Windows' StretchDIBits behaved.
static void dib_blit(struct w16 *w, uint16_t hdc, const struct uwmf_rec *r, int xd, int yd, int wd, int hd,
                     int xs, int ys, int ws, int hs) {
    if (!r->dib) return;
    uint16_t bm = w16_gdi_bitmap_of_dib(w, r->dib, r->ndib);
    if (!bm) return;
    uint16_t mdc = (uint16_t)G(GDI_COMPATDC, hdc);
    if (mdc) {
        uint16_t old = (uint16_t)G(GDI_SELECTOBJECT, mdc, bm);
        G(GDI_STRETCHBLT, hdc, (uint16_t)xd, (uint16_t)yd, (uint16_t)wd, (uint16_t)hd, mdc, (uint16_t)xs,
          (uint16_t)ys, (uint16_t)ws, (uint16_t)hs, HI(r->rop), LO(r->rop));
        G(GDI_SELECTOBJECT, mdc, old);
        G(GDI_DELETEDC, mdc);
    }
    G(GDI_DELETEOBJECT, bm);
}

static uint16_t make_brush(struct w16 *w, const struct uwmf_rec *r) {
    switch (r->v[0]) {
    case 1: return (uint16_t)G(GDI_STOCKOBJECT, 5);                       // BS_NULL: NULL_BRUSH
    case 2: return (uint16_t)G(GDI_HATCHBRUSH, (uint16_t)r->v[1], HI(r->color), LO(r->color));
    default: return (uint16_t)G(GDI_SOLIDBRUSH, HI(r->color), LO(r->color));
    }
}

static uint16_t make_font(struct w16 *w, const struct uwmf_rec *r) {
    uint8_t lf[50] = { 0 };
    for (int i = 0; i < 5; i++) { lf[2 * i] = (uint8_t)r->v[i]; lf[2 * i + 1] = (uint8_t)((uint16_t)r->v[i] >> 8); }
    for (int i = 0; i < 8; i++) lf[10 + i] = (uint8_t)r->v[5 + i];
    memcpy(lf + 18, r->face, strlen(r->face));
    uint16_t at = put(w, lf, sizeof lf);
    uint16_t h = at ? (uint16_t)G(GDI_FONTINDIRECT, w->sysdata, at) : 0;
    if (at) w16_sys_pop(w, at);
    return h;
}

static uint16_t make_pattern(struct w16 *w, const struct uwmf_rec *r) {
    uint16_t bm = r->dib ? w16_gdi_bitmap_of_dib(w, r->dib, r->ndib) : 0;
    if (!bm) return 0;
    uint16_t h = (uint16_t)G(GDI_PATTERNBRUSH, bm);
    G(GDI_DELETEOBJECT, bm);                     // the brush keeps its own copy of the 8 x 8
    return h;
}

static void text(struct w16 *w, uint16_t hdc, int x, int y, const char *s, int n) {
    if (n <= 0) return;
    uint16_t at = put(w, s, (unsigned)n);
    if (!at) return;
    G(GDI_TEXTOUT, hdc, (uint16_t)x, (uint16_t)y, w->sysdata, at, (uint16_t)n);
    w16_sys_pop(w, at);
}

static void points(struct w16 *w, uint16_t hdc, unsigned ord, const struct uwmf_rec *r) {
    uint16_t at = put(w, r->pt, (unsigned)r->npt * 4);
    if (!at) return;
    if (ord == GDI_POLYPOLYGON) {
        uint16_t cnt = put(w, r->counts, (unsigned)r->ncounts * 2);
        if (cnt) G(GDI_POLYPOLYGON, hdc, w->sysdata, at, w->sysdata, cnt, (uint16_t)r->ncounts);
    } else {
        G(ord, hdc, w->sysdata, at, (uint16_t)r->npt);
    }
    w16_sys_pop(w, at);
}

// Offset or scale a mapping pair through its Get and Set exports.
static void adjust(struct w16 *w, uint16_t hdc, unsigned get, unsigned set, const struct uwmf_rec *r, int scale) {
    uint32_t v = G(get, hdc);
    int x = (int16_t)LO(v), y = (int16_t)HI(v);
    if (scale) {
        if (!r->v[1] || !r->v[3]) return;
        x = x * r->v[0] / r->v[1];
        y = y * r->v[2] / r->v[3];
    } else {
        x += r->v[0];
        y += r->v[1];
    }
    G(set, hdc, (uint16_t)x, (uint16_t)y);
}

int w16_play_wmf(struct w16 *w, uint16_t hdc, const uint8_t *bytes, uint32_t len) {
    struct uwmf m;
    if (uwmf_open(&m, bytes, len) < 0) return 0;
    uint16_t *obj = calloc(m.nobjects ? m.nobjects : 1, sizeof *obj);
    if (!obj) { uwmf_close(&m); return 0; }
    uint16_t ax = w->cpu.r[R_AX], dx = w->cpu.r[R_DX];
    G(GDI_SAVEDC, hdc);
    struct uwmf_rec r;
    int ok = 1, got = 0;
    while (!w->cpu.fault && (got = uwmf_next(&m, &r)) > 0) {
        const int16_t *v = r.v;
#define U(i) ((uint16_t)v[i])
        switch (r.func) { // dispatch-ok: bounded by MS-WMF's record types, each its own GDI call
        case UWMF_SETBKCOLOR:   G(GDI_SETBKCOLOR, hdc, HI(r.color), LO(r.color)); break;
        case UWMF_SETTEXTCOLOR: G(GDI_SETTEXTCOLOR, hdc, HI(r.color), LO(r.color)); break;
        case UWMF_SETBKMODE:    G(GDI_SETBKMODE, hdc, U(0)); break;
        case UWMF_SETMAPMODE:   G(GDI_SETMAPMODE, hdc, U(0)); break;
        case UWMF_SETROP2:      G(GDI_SETROP2, hdc, U(0)); break;
        case UWMF_SETPOLYFILLMODE: G(GDI_SETPOLYFILL, hdc, U(0)); break;
        case UWMF_SETSTRETCHBLTMODE: G(GDI_SETSTRETCH, hdc, U(0)); break;
        case UWMF_SETTEXTJUSTIFICATION: if (r.nv >= 2) G(GDI_SETJUSTIFY, hdc, U(0), U(1)); break;
        case UWMF_SETWINDOWORG:   if (r.nv >= 2) G(GDI_SETWINDOWORG, hdc, U(0), U(1)); break;
        case UWMF_SETWINDOWEXT:   if (r.nv >= 2) G(GDI_SETWINDOWEXT, hdc, U(0), U(1)); break;
        case UWMF_SETVIEWPORTORG: if (r.nv >= 2) G(GDI_SETVPORG, hdc, U(0), U(1)); break;
        case UWMF_SETVIEWPORTEXT: if (r.nv >= 2) G(GDI_SETVPEXT, hdc, U(0), U(1)); break;
        case UWMF_OFFSETWINDOWORG:   if (r.nv >= 2) adjust(w, hdc, GDI_WINDOWORG, GDI_SETWINDOWORG, &r, 0); break;
        case UWMF_OFFSETVIEWPORTORG: if (r.nv >= 2) adjust(w, hdc, GDI_VPORG, GDI_SETVPORG, &r, 0); break;
        case UWMF_SCALEWINDOWEXT:    if (r.nv >= 4) adjust(w, hdc, GDI_WINDOWEXT, GDI_SETWINDOWEXT, &r, 1); break;
        case UWMF_SCALEVIEWPORTEXT:  if (r.nv >= 4) adjust(w, hdc, GDI_VPEXT, GDI_SETVPEXT, &r, 1); break;
        case UWMF_MOVETO:  if (r.nv >= 2) G(GDI_MOVETO, hdc, U(0), U(1)); break;
        case UWMF_LINETO:  if (r.nv >= 2) G(GDI_LINETO, hdc, U(0), U(1)); break;
        case UWMF_INTERSECTCLIPRECT: if (r.nv >= 4) G(GDI_INTERSECTCLIP, hdc, U(0), U(1), U(2), U(3)); break;
        case UWMF_RECTANGLE:
        case UWMF_ROUNDRECT:                     // its corners drawn square
            if (r.nv >= 4) G(GDI_RECTANGLE, hdc, U(0), U(1), U(2), U(3));
            break;
        case UWMF_ELLIPSE: if (r.nv >= 4) G(GDI_ELLIPSE, hdc, U(0), U(1), U(2), U(3)); break;
        case UWMF_PATBLT:  G(GDI_PATBLT, hdc, U(0), U(1), U(2), U(3), HI(r.rop), LO(r.rop)); break;
        case UWMF_SETPIXEL: G(GDI_SETPIXEL, hdc, U(0), U(1), HI(r.color), LO(r.color)); break;
        case UWMF_SAVEDC:  G(GDI_SAVEDC, hdc); break;
        case UWMF_RESTOREDC: G(GDI_RESTOREDC, hdc, U(0)); break;
        case UWMF_TEXTOUT: text(w, hdc, v[0], v[1], r.text, r.ntext); break;
        case UWMF_EXTTEXTOUT: {
            if (v[2] & 2) {                      // ETO_OPAQUE: the rectangle in the background colour
                uint16_t br = 0;                 // a brush of it, made and dropped
                uint32_t bk = G(GDI_SETBKCOLOR, hdc, 0, 0);
                G(GDI_SETBKCOLOR, hdc, HI(bk), LO(bk));
                br = (uint16_t)G(GDI_SOLIDBRUSH, HI(bk), LO(bk));
                uint16_t was = (uint16_t)G(GDI_SELECTOBJECT, hdc, br);
                G(GDI_PATBLT, hdc, (uint16_t)r.rect[0], (uint16_t)r.rect[1], (uint16_t)(r.rect[2] - r.rect[0]),
                  (uint16_t)(r.rect[3] - r.rect[1]), 0x00F0, 0x0021);
                G(GDI_SELECTOBJECT, hdc, was);
                G(GDI_DELETEOBJECT, br);
            }
            if (r.dx) {                          // each character at the advance recorded for it
                int x = v[0];
                for (int i = 0; i < r.ntext; x += r.dx[i], i++) text(w, hdc, x, v[1], r.text + i, 1);
            } else {
                text(w, hdc, v[0], v[1], r.text, r.ntext);
            }
            break;
        }
        case UWMF_POLYGON:     points(w, hdc, GDI_POLYGON, &r); break;
        case UWMF_POLYLINE:    points(w, hdc, GDI_POLYLINE, &r); break;
        case UWMF_POLYPOLYGON: points(w, hdc, GDI_POLYPOLYGON, &r); break;
        case UWMF_CREATEPENINDIRECT:
            if (r.slot >= 0) obj[r.slot] = (uint16_t)G(GDI_CREATEPEN, U(0), U(1), HI(r.color), LO(r.color));
            break;
        case UWMF_CREATEBRUSHINDIRECT: if (r.slot >= 0) obj[r.slot] = make_brush(w, &r); break;
        case UWMF_CREATEFONTINDIRECT:  if (r.slot >= 0) obj[r.slot] = make_font(w, &r); break;
        case UWMF_DIBCREATEPATTERNBRUSH: if (r.slot >= 0) obj[r.slot] = make_pattern(w, &r); break;
        case UWMF_SELECTOBJECT: if (obj[r.slot]) G(GDI_SELECTOBJECT, hdc, obj[r.slot]); break;
        case UWMF_DELETEOBJECT:
            if (obj[r.slot]) G(GDI_DELETEOBJECT, obj[r.slot]);   // refused while selected, as on Windows
            obj[r.slot] = 0;
            break;
        case UWMF_DIBBITBLT:   dib_blit(w, hdc, &r, v[0], v[1], v[2], v[3], v[4], v[5], v[2], v[3]); break;
        case UWMF_DIBSTRETCHBLT:
        case UWMF_STRETCHDIB:  dib_blit(w, hdc, &r, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); break;
        case UWMF_SETDIBTODEV: {
            struct uwmf_rec k = r;
            k.rop = 0x00CC0020;                  // SRCCOPY, at the DIB's own size
            dib_blit(w, hdc, &k, v[0], v[1], v[2], v[3], v[4], v[5], v[2], v[3]);
            break;
        }
        default:
            // Arcs, pies and chords, palettes, regions, escapes, text alignment:
            // not drawn (w16_play_wmf's caller's note says so).
            break;
        }
#undef U
    }
    if (got < 0) ok = 0;
    G(GDI_RESTOREDC, hdc, (uint16_t)-1);
    for (unsigned i = 0; i < m.nobjects; i++)
        if (obj[i]) G(GDI_DELETEOBJECT, obj[i]);
    free(obj);
    uwmf_close(&m);
    w->cpu.r[R_AX] = ax;
    w->cpu.r[R_DX] = dx;
    return ok;
}

int w16_gdi_play_into(struct w16 *w, uint16_t hdc, const int16_t rc[4], int32_t pw, int32_t ph,
                      const uint8_t *bytes, uint32_t len) {
    uint16_t ax = w->cpu.r[R_AX], dx = w->cpu.r[R_DX];
    // the rectangle in pixels, as the viewport is
    int16_t pt[4] = { rc[0], rc[1], rc[2], rc[3] };
    uint16_t at = put(w, pt, sizeof pt);
    if (!at) return 0;
    G(99, hdc, w->sysdata, at, 2);               // LPtoDP
    memcpy(pt, w->sysmem + at, sizeof pt);
    w16_sys_pop(w, at);
    G(GDI_SAVEDC, hdc);
    G(GDI_SETMAPMODE, hdc, 8);                   // MM_ANISOTROPIC
    G(GDI_SETWINDOWORG, hdc, 0, 0);
    G(GDI_SETWINDOWEXT, hdc, (uint16_t)(pw ? pw : 1), (uint16_t)(ph ? ph : 1));
    G(GDI_SETVPORG, hdc, (uint16_t)pt[0], (uint16_t)pt[1]);
    G(GDI_SETVPEXT, hdc, (uint16_t)(pt[2] - pt[0] ? pt[2] - pt[0] : 1), (uint16_t)(pt[3] - pt[1] ? pt[3] - pt[1] : 1));
    int ok = w16_play_wmf(w, hdc, bytes, len);
    G(GDI_RESTOREDC, hdc, (uint16_t)-1);
    w->cpu.r[R_AX] = ax;
    w->cpu.r[R_DX] = dx;
    return ok;
}

void w16_gdi_stretch_bitmap(struct w16 *w, uint16_t hdc, int x, int y, int cw, int ch, uint16_t bm) {
    int bw, bh;
    if (!w16_gdi_bitmap_pixels(w, bm, &bw, &bh)) return;
    uint16_t ax = w->cpu.r[R_AX], dx = w->cpu.r[R_DX];
    uint16_t mdc = (uint16_t)G(GDI_COMPATDC, hdc);
    if (mdc) {
        uint16_t old = (uint16_t)G(GDI_SELECTOBJECT, mdc, bm);
        G(GDI_STRETCHBLT, hdc, (uint16_t)x, (uint16_t)y, (uint16_t)cw, (uint16_t)ch, mdc, 0, 0, (uint16_t)bw,
          (uint16_t)bh, 0x00CC, 0x0020);
        G(GDI_SELECTOBJECT, mdc, old);
        G(GDI_DELETEDC, mdc);
    }
    w->cpu.r[R_AX] = ax;
    w->cpu.r[R_DX] = dx;
}

uint16_t w16_render_metafile(struct w16 *w, const uint8_t *bytes, uint32_t len, int xext, int yext) {
    // its size: a METAFILEPICT's extents in HIMETRIC (negative: a suggestion), else 3 inches
    int pw = xext > 0 ? (int)((long)xext * 96 / 2540) : 288, ph = yext > 0 ? (int)((long)yext * 96 / 2540) : 288;
    if (pw < 1 || ph < 1 || pw > 4096 || ph > 4096) return 0;
    uint32_t *white = malloc((size_t)pw * ph * 4);
    if (!white) return 0;
    for (long i = 0; i < (long)pw * ph; i++) white[i] = 0xFFFFFF;
    uint16_t bm = w16_gdi_bitmap_from(w, white, pw, ph);
    free(white);
    uint16_t ax = w->cpu.r[R_AX], dx = w->cpu.r[R_DX];
    uint16_t mdc = bm ? (uint16_t)G(GDI_COMPATDC, 0) : 0;
    if (mdc) {
        uint16_t old = (uint16_t)G(GDI_SELECTOBJECT, mdc, bm);
        const int16_t rc[4] = { 0, 0, (int16_t)pw, (int16_t)ph };
        w16_gdi_play_into(w, mdc, rc, xext > 0 ? xext : pw, yext > 0 ? yext : ph, bytes, len);
        G(GDI_SELECTOBJECT, mdc, old);
        G(GDI_DELETEDC, mdc);
    }
    w->cpu.r[R_AX] = ax;
    w->cpu.r[R_DX] = dx;
    return bm;
}

// PlayMetaFile(hdc, hmf) -> BOOL
// Windows: Draws a metafile -- the GDI calls it recorded -- into a DC, through the DC's mapping;
// the metafile's own pens, brushes and fonts are made and deleted as it goes.
// w16: The same, a record at a time through w16's own GDI calls: lines, rectangles, ellipses,
// polygons, text, bitmaps (the DIB records), the mapping and the clip. Arcs, pies and chords draw
// nothing, a rounded rectangle draws square, and a palette is not realised.
// Why: These are the records the pictures met so far do not use; each is one more GDI call w16
// does not have yet (Arc, Pie, Chord, RoundRect).
// Notice: A chart or drawing made of arcs shows without them.
// Whole: Arc, Pie, Chord and RoundRect in GDI, which the player would then call.
static int api_PlayMetaFile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a), hmf = w16_arg16(a);
    uint32_t size;
    const uint8_t *p = w16_global_ptr(w, hmf, &size);
    // The program's block may move under the playing (it is the program's
    // memory): played from a copy.
    uint8_t *copy = p ? malloc(size) : 0;
    if (copy) memcpy(copy, p, size);
    int ok = copy && w16_play_wmf(w, hdc, copy, size);
    free(copy);
    w->cpu.r[R_AX] = (uint16_t)ok;
    return w->cpu.fault ? -1 : 4;
}

const struct w16_api g_meta_api[] = {
    { 123, api_PlayMetaFile },
};
const unsigned g_meta_api_n = sizeof g_meta_api / sizeof g_meta_api[0];
