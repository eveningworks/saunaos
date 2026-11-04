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

#include <stdio.h>
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
#define GDI_ARC           23
#define GDI_ELLIPSE       24
#define GDI_PIE           26
#define GDI_RECTANGLE     27
#define GDI_ROUNDRECT     28
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
#define GDI_CHORD         348
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
        case UWMF_RECTANGLE: if (r.nv >= 4) G(GDI_RECTANGLE, hdc, U(0), U(1), U(2), U(3)); break;
        case UWMF_ROUNDRECT: if (r.nv >= 6) G(GDI_ROUNDRECT, hdc, U(0), U(1), U(2), U(3), U(4), U(5)); break;
        case UWMF_ARC:
        case UWMF_PIE:
        case UWMF_CHORD:
            if (r.nv >= 8)
                G(r.func == UWMF_ARC ? GDI_ARC : r.func == UWMF_PIE ? GDI_PIE : GDI_CHORD, hdc, U(0), U(1), U(2),
                  U(3), U(4), U(5), U(6), U(7));
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
            // Palettes, regions, escapes, text alignment:
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
// w16: The same, a record at a time through w16's own GDI calls: lines, rectangles, rounded
// rectangles, ellipses, arcs, pies, chords, polygons, text, bitmaps (the DIB records), the mapping
// and the clip. A palette is not realised.
// Why: w16 draws in true colour and realises no palette (CreatePalette keeps one, and SelectPalette
// only names where a DIB_PAL_COLORS table looks), so a picture's palette records are skipped.
// Notice: A picture whose colours come from its own palette -- a DIB in it naming palette indices --
// may show other colours.
// Whole: The palette records played through CreatePalette and SelectPalette.
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

// --- recording ----------------------------------------------------------------------
//
// A METAFILE DC is a memory DC this file knows the handle of: w16_gdi() hands every call whose first
// argument is one to w16_meta_record() before its own handler. A record is the call's own stack words
// -- the arguments last first, which is MS-WMF's order -- under the record's function number, so most
// calls need nothing but a row in RECORDED; a pointer argument's data goes in instead of the pointer
// (text, points, a DIB), an object as its Create record the first time it is selected and then its slot,
// and a blit's source DC as the DIB of the rectangle it was read from. A call RECORDED does not name
// runs on the memory DC beneath, as an answer (GetDeviceCaps, GetTextExtent) and draws nothing anyone
// sees.

struct mdc {
    uint16_t hdc;
    uint8_t *buf;                  // the records so far, the header and EOF added at the close
    size_t n, cap;
    uint32_t longest;              // the longest record, in words: the header's mtMaxRecord
    uint16_t *slot;                // the object table: a slot's GDI handle, 0 when free
    int nslot;
    char *path;                    // a disk metafile's DOS name, NULL for one in memory
};

static struct mdc *g_mdc;
static int g_nmdc;

static struct mdc *mdc_of(uint16_t hdc) {
    for (int i = 0; hdc && i < g_nmdc; i++)
        if (g_mdc[i].hdc == hdc) return &g_mdc[i];
    return 0;
}

int w16_meta_dc(uint16_t hdc) { return mdc_of(hdc) != 0; }

static int grow(struct mdc *m, size_t more) {
    if (m->n + more <= m->cap) return 1;
    size_t cap = m->cap ? m->cap : 256;
    while (cap < m->n + more) cap *= 2;
    uint8_t *g = realloc(m->buf, cap);
    if (!g) return 0;
    m->buf = g;
    m->cap = cap;
    return 1;
}

// One record: its words, then `extra` bytes (a string or a DIB) padded to a word.
static void emit(struct mdc *m, unsigned func, const uint16_t *words, unsigned nw, const void *extra, size_t nx) {
    size_t bytes = 6 + 2 * (size_t)nw + (nx + 1) / 2 * 2;
    if (!grow(m, bytes)) return;
    uint8_t *p = m->buf + m->n;
    uint32_t size = (uint32_t)(bytes / 2);
    memcpy(p, &size, 4);
    p[4] = (uint8_t)func;
    p[5] = (uint8_t)(func >> 8);
    memcpy(p + 6, words, 2 * (size_t)nw);
    if (nx) memcpy(p + 6 + 2 * nw, extra, nx);
    if (nx & 1) p[bytes - 1] = 0;
    m->n += bytes;
    if (size > m->longest) m->longest = size;
}

// The program's bytes at a far pointer, read only; NULL when they are not there.
static const uint8_t *far_ptr(struct w16 *w, uint32_t far, unsigned n) {
    return cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n ? n : 1, 0);
}

// The object's slot, its Create record first when it has none; -1 when it cannot be recorded.
static int slot_of(struct w16 *w, struct mdc *m, uint16_t h) {
    for (int i = 0; i < m->nslot; i++)
        if (m->slot[i] == h) return i;
    uint8_t d[50];
    uint16_t words[32];
    unsigned func = 0, nw = 0;
    uint8_t *dib = 0;
    uint32_t ndib = 0;
    int kind = w16_gdi_kind(w, h);
    if (kind == GDI_PEN && w16_gdi_describe(w, h, d)) {
        func = UWMF_CREATEPENINDIRECT;
        memcpy(words, d, 10);
        nw = 5;
    } else if (kind == GDI_BRUSH && (dib = w16_gdi_brush_dib(w, h, &ndib))) {
        func = UWMF_DIBCREATEPATTERNBRUSH;
        words[0] = 5;                            // BS_DIBPATTERN
        words[1] = 0;                            // DIB_RGB_COLORS
        nw = 2;
    } else if (kind == GDI_BRUSH && w16_gdi_describe(w, h, d)) {
        func = UWMF_CREATEBRUSHINDIRECT;
        memcpy(words, d, 8);
        nw = 4;
    } else if (kind == GDI_FONT && w16_gdi_describe(w, h, d)) {
        func = UWMF_CREATEFONTINDIRECT;
        memcpy(words, d, 50);
        nw = 25;
    } else if (kind == GDI_PALETTE || h == 0x500F) {   // a palette, or the stock DEFAULT_PALETTE
        int n;
        const uint32_t *e = w16_gdi_palette_entries(w, h, &n);
        uint16_t *pal = malloc(4 + 4 * (size_t)n);
        if (!pal) return -1;
        pal[0] = 0x300;
        pal[1] = (uint16_t)n;
        for (int i = 0; i < n; i++) {             // PALETTEENTRY: red, green, blue, flags
            pal[2 + 2 * i] = (uint16_t)((e[i] >> 16 & 0xFF) | (e[i] >> 8 & 0xFF) << 8);
            pal[3 + 2 * i] = (uint16_t)(e[i] & 0xFF);
        }
        dib = (uint8_t *)pal;
        ndib = 4 + 4 * (uint32_t)n;
        func = UWMF_CREATEPALETTE;
    } else {
        return -1;
    }
    int i = 0;
    while (i < m->nslot && m->slot[i]) i++;      // MS-WMF's rule: the lowest free slot
    if (i == m->nslot) {
        uint16_t *g = realloc(m->slot, sizeof *g * (size_t)(m->nslot + 1));
        if (!g) { free(dib); return -1; }
        m->slot = g;
        m->nslot++;
    }
    m->slot[i] = h;
    emit(m, func, words, nw, dib, ndib);
    free(dib);
    return i;
}

void w16_meta_forget(struct w16 *w, uint16_t h) {
    (void)w;
    for (int k = 0; k < g_nmdc; k++)
        for (int i = 0; i < g_mdc[k].nslot; i++)
            if (g_mdc[k].slot[i] == h) {
                uint16_t v = (uint16_t)i;
                emit(&g_mdc[k], UWMF_DELETEOBJECT, &v, 1, 0, 0);
                g_mdc[k].slot[i] = 0;
            }
}

int w16_meta_select_palette(struct w16 *w, uint16_t hdc, uint16_t hpal) {
    struct mdc *m = mdc_of(hdc);
    if (!m) return 0;
    int i = slot_of(w, m, hpal);
    if (i >= 0) {
        uint16_t v = (uint16_t)i;
        emit(m, UWMF_SELECTPALETTE, &v, 1, 0, 0);
    }
    return 1;
}

int w16_meta_realize_palette(uint16_t hdc) {
    struct mdc *m = mdc_of(hdc);
    if (m) emit(m, UWMF_REALIZEPALETTE, 0, 0, 0, 0);
    return m != 0;
}

// What each recorded GDI call is: its MS-WMF function, and how its arguments go in.
enum { R_WORDS, R_TEXT, R_EXTTEXT, R_POLY, R_POLYPOLY, R_BLT, R_STRETCHDIB, R_DIBTODEV, R_SELECT };
static const struct { uint16_t ordinal, func; uint8_t how; } RECORDED[] = {
    { 1, UWMF_SETBKCOLOR, R_WORDS },       { 2, UWMF_SETBKMODE, R_WORDS },
    { 3, UWMF_SETMAPMODE, R_WORDS },       { 4, UWMF_SETROP2, R_WORDS },
    { 5, UWMF_SETRELABS, R_WORDS },        { 6, UWMF_SETPOLYFILLMODE, R_WORDS },
    { 7, UWMF_SETSTRETCHBLTMODE, R_WORDS }, { 8, UWMF_SETTEXTCHAREXTRA, R_WORDS },
    { 9, UWMF_SETTEXTCOLOR, R_WORDS },     { 10, UWMF_SETTEXTJUSTIFICATION, R_WORDS },
    { 11, UWMF_SETWINDOWORG, R_WORDS },    { 12, UWMF_SETWINDOWEXT, R_WORDS },
    { 13, UWMF_SETVIEWPORTORG, R_WORDS },  { 14, UWMF_SETVIEWPORTEXT, R_WORDS },
    { 15, UWMF_OFFSETWINDOWORG, R_WORDS }, { 16, UWMF_SCALEWINDOWEXT, R_WORDS },
    { 17, UWMF_OFFSETVIEWPORTORG, R_WORDS }, { 18, UWMF_SCALEVIEWPORTEXT, R_WORDS },
    { 19, UWMF_LINETO, R_WORDS },          { 20, UWMF_MOVETO, R_WORDS },
    { 21, UWMF_EXCLUDECLIPRECT, R_WORDS }, { 22, UWMF_INTERSECTCLIPRECT, R_WORDS },
    { 23, UWMF_ARC, R_WORDS },             { 24, UWMF_ELLIPSE, R_WORDS },
    { 25, UWMF_FLOODFILL, R_WORDS },       { 26, UWMF_PIE, R_WORDS },
    { 27, UWMF_RECTANGLE, R_WORDS },       { 28, UWMF_ROUNDRECT, R_WORDS },
    { 29, UWMF_PATBLT, R_WORDS },          { 30, UWMF_SAVEDC, R_WORDS },
    { 31, UWMF_SETPIXEL, R_WORDS },        { 32, UWMF_OFFSETCLIPRGN, R_WORDS },
    { 33, UWMF_TEXTOUT, R_TEXT },          { 34, UWMF_DIBBITBLT, R_BLT },
    { 35, UWMF_DIBSTRETCHBLT, R_BLT },     { 36, UWMF_POLYGON, R_POLY },
    { 37, UWMF_POLYLINE, R_POLY },         { 39, UWMF_RESTOREDC, R_WORDS },
    { 45, UWMF_SELECTOBJECT, R_SELECT },   { 346, UWMF_SETTEXTALIGN, R_WORDS },
    { 348, UWMF_CHORD, R_WORDS },          { 349, UWMF_SETMAPPERFLAGS, R_WORDS },
    { 351, UWMF_EXTTEXTOUT, R_EXTTEXT },   { 372, 0x0548, R_WORDS },            // ExtFloodFill
    { 439, UWMF_STRETCHDIB, R_STRETCHDIB }, { 443, UWMF_SETDIBTODEV, R_DIBTODEV },
    { 450, UWMF_POLYPOLYGON, R_POLYPOLY },
};

// A packed DIB's bytes -- header, colour table and `rows` rows -- at the program's lpbmi and lpbits, in
// one malloc'd block; NULL when they are not readable.
static uint8_t *program_dib(struct w16 *w, uint32_t lpbmi, uint32_t lpbits, unsigned usage, unsigned rows,
                            uint32_t *len) {
    const uint8_t *h = far_ptr(w, lpbmi, 40);
    if (!h) return 0;
    uint32_t hs = (uint32_t)(h[0] | h[1] << 8), bw, bpp, used = 0;
    int32_t bh;
    if (hs == 12) {                              // BITMAPCOREHEADER
        bw = (uint32_t)(h[4] | h[5] << 8);
        bh = (int16_t)(h[6] | h[7] << 8);
        bpp = (uint32_t)(h[10] | h[11] << 8);
    } else {
        memcpy(&bw, h + 4, 4);
        memcpy(&bh, h + 8, 4);
        bpp = (uint32_t)(h[14] | h[15] << 8);
        if (hs >= 36) memcpy(&used, h + 32, 4);
    }
    if (hs < 12 || hs > 64 || !bw || bw > 32767 || !bpp || bpp > 32) return 0;
    uint32_t ncol = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    uint32_t tab = ncol * (usage == 1 ? 2 : hs == 12 ? 3 : 4);
    uint32_t stride = (bw * bpp + 31) / 32 * 4, ah = (uint32_t)(bh < 0 ? -bh : bh);
    if (!rows || rows > ah) rows = ah;
    uint32_t bits = stride * rows;
    const uint8_t *hp = far_ptr(w, lpbmi, hs + tab), *bp = far_ptr(w, lpbits, bits);
    uint8_t *out = hp && bp ? malloc(hs + tab + bits) : 0;
    if (!out) return 0;
    memcpy(out, hp, hs + tab);
    memcpy(out + hs + tab, bp, bits);
    *len = hs + tab + bits;
    return out;
}

int w16_meta_record(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = a->c;
    if (a->pos < 2) return -2;
    struct mdc *m = mdc_of(cpu16_arg(c, a->pos - 2));
    if (!m) return -2;
    int k = -1;
    for (unsigned i = 0; i < sizeof RECORDED / sizeof RECORDED[0]; i++)
        if (RECORDED[i].ordinal == ordinal) { k = (int)i; break; }
    if (k < 0) return -2;                        // an answer, not a drawing: the memory DC's
    unsigned func = RECORDED[k].func, nw = (a->pos - 2) / 2, ret = a->pos;
    uint16_t v[16];
    for (unsigned i = 0; i < nw && i < 16; i++) v[i] = cpu16_arg(c, 2 * i);   // last argument first
#define FAR(i) ((uint32_t)v[i] | (uint32_t)v[(i) + 1] << 16)
    c->r[R_AX] = 1;
    c->r[R_DX] = 0;
    switch (RECORDED[k].how) { // dispatch-ok: one case per way MS-WMF lays a call's arguments out
    case R_WORDS:
        if (nw == func >> 8) emit(m, func, v, nw, 0, 0);
        break;
    case R_TEXT: {                               // TextOut: count, lpString, y, x
        unsigned n = v[0];
        const uint8_t *t = far_ptr(w, FAR(1), n);
        if (!t) break;
        uint8_t *rec = malloc(2 + (n + 1) / 2 * 2 + 4);
        if (!rec) break;
        memcpy(rec, &v[0], 2);
        memset(rec + 2, 0, (n + 1) / 2 * 2);
        memcpy(rec + 2, t, n);
        memcpy(rec + 2 + (n + 1) / 2 * 2, &v[3], 2);
        memcpy(rec + 4 + (n + 1) / 2 * 2, &v[4], 2);
        emit(m, func, 0, 0, rec, 6 + (n + 1) / 2 * 2);
        free(rec);
        break;
    }
    case R_EXTTEXT: {                            // lpDx, count, lpString, lpRect, options, y, x
        unsigned n = v[2], opts = v[7];
        const uint8_t *t = far_ptr(w, FAR(3), n);
        const uint8_t *rc = (opts & 6) && FAR(5) ? far_ptr(w, FAR(5), 8) : 0;   // ETO_OPAQUE, ETO_CLIPPED
        const uint8_t *dx = FAR(0) ? far_ptr(w, FAR(0), 2 * n) : 0;
        if (!t || nw != 10) break;
        size_t sz = 8 + (rc ? 8 : 0) + (n + 1) / 2 * 2 + (dx ? 2 * (size_t)n : 0);
        uint8_t *rec = calloc(1, sz), *q = rec;
        if (!rec) break;
        uint16_t opt = (uint16_t)(rc ? opts : opts & ~6u);
        memcpy(q, &v[8], 2); memcpy(q + 2, &v[9], 2);            // y, x
        memcpy(q + 4, &v[2], 2); memcpy(q + 6, &opt, 2);         // count, options
        q += 8;
        if (rc) { memcpy(q, rc, 8); q += 8; }
        memcpy(q, t, n);
        q += (n + 1) / 2 * 2;
        if (dx) memcpy(q, dx, 2 * (size_t)n);
        emit(m, func, 0, 0, rec, sz);
        free(rec);
        break;
    }
    case R_POLY: {                               // count, lpPoints
        unsigned n = v[0];
        const uint8_t *pt = far_ptr(w, FAR(1), 4 * n);
        if (pt) {
            uint8_t *rec = malloc(2 + 4 * (size_t)n);
            if (rec) { memcpy(rec, &v[0], 2); memcpy(rec + 2, pt, 4 * (size_t)n); emit(m, func, 0, 0, rec, 2 + 4 * (size_t)n); }
            free(rec);
        }
        break;
    }
    case R_POLYPOLY: {                           // nPolys, lpCounts, lpPoints
        unsigned np = v[0], total = 0;
        const uint8_t *cnt = far_ptr(w, FAR(1), 2 * np);
        for (unsigned i = 0; cnt && i < np; i++) total += (unsigned)(cnt[2 * i] | cnt[2 * i + 1] << 8);
        const uint8_t *pt = cnt ? far_ptr(w, FAR(3), 4 * total) : 0;
        size_t sz = 2 + 2 * (size_t)np + 4 * (size_t)total;
        uint8_t *rec = pt ? malloc(sz) : 0;
        if (rec) {
            memcpy(rec, &v[0], 2);
            memcpy(rec + 2, cnt, 2 * (size_t)np);
            memcpy(rec + 2 + 2 * np, pt, 4 * (size_t)total);
            emit(m, func, 0, 0, rec, sz);
        }
        free(rec);
        break;
    }
    case R_BLT: {
        // BitBlt: rop, ys, xs, hdcSrc, h, w, y, x -- StretchBlt: rop, hs, ws, ys, xs, hdcSrc, h, w, y, x.
        // With no source the source DC's word is the record's Reserved one; with one, it goes and the
        // DIB of the rectangle read follows, the source origin then 0, 0.
        int stretch = ordinal == 35;
        unsigned src = stretch ? 6 : 4;
        if (nw != src + 5) break;
        if (!v[src]) { emit(m, func, v, nw, 0, 0); break; }
        int sx = (int16_t)v[src - 1], sy = (int16_t)v[src - 2];
        int sw = stretch ? (int16_t)v[3] : (int16_t)v[src + 2], sh = stretch ? (int16_t)v[2] : (int16_t)v[src + 1];
        uint32_t ndib = 0;
        uint8_t *dib = w16_gdi_dc_dib(w, v[src], sx, sy, sw < 0 ? -sw : sw, sh < 0 ? -sh : sh, &ndib);
        if (!dib) break;
        uint16_t rv[16];
        unsigned j = 0;
        for (unsigned i = 0; i < nw; i++) if (i != src) rv[j++] = v[i];
        rv[src - 1] = rv[src - 2] = 0;           // XSrc, YSrc: the DIB is that rectangle alone
        emit(m, func, rv, j, dib, ndib);
        free(dib);
        break;
    }
    case R_STRETCHDIB: {
        // rop, usage, lpbmi, lpbits, hs, ws, ys, xs, hd, wd, yd, xd -> rop, usage, hs..xd, the DIB
        if (nw != 15) break;
        uint32_t ndib = 0;
        uint8_t *dib = program_dib(w, FAR(3), FAR(5), v[2], 0, &ndib);
        if (!dib) break;
        uint16_t rv[11];
        memcpy(rv, v, 3 * 2);
        memcpy(rv + 3, v + 7, 8 * 2);
        emit(m, func, rv, 11, dib, ndib);
        free(dib);
        c->r[R_AX] = v[7];                       // the source's height: the rows drawn
        break;
    }
    case R_DIBTODEV: {
        // usage, lpbmi, lpbits, lines, start, ys, xs, h, w, yd, xd -> usage, lines, start..xd, the DIB
        if (nw != 13) break;
        uint32_t ndib = 0;
        uint8_t *dib = program_dib(w, FAR(1), FAR(3), v[0], v[5], &ndib);
        if (!dib) break;
        uint16_t rv[9];
        rv[0] = v[0];
        memcpy(rv + 1, v + 5, 8 * 2);
        emit(m, func, rv, 9, dib, ndib);
        free(dib);
        c->r[R_AX] = v[5];
        break;
    }
    case R_SELECT: {                             // the object's slot; the handle back, as a stand-in
        int i = slot_of(w, m, v[0]);
        if (i >= 0) {
            uint16_t sv = (uint16_t)i;
            emit(m, func, &sv, 1, 0, 0);
        }
        c->r[R_AX] = v[0];
        break;
    }
    }
#undef FAR
    return (int)ret;
}

// CreateMetaFile(lpFilename) -> HDC
// Windows: Starts a metafile -- a DC that records the GDI calls made on it, in memory (lpFilename NULL)
// or in a file.
// w16: The same: the drawing calls, the mapping, the clip, objects selected (pens, brushes, fonts,
// palettes), text, polygons, blits (the source's pixels as a DIB) and DIBs are recorded; questions
// asked of it (GetDeviceCaps, GetTextExtent) are answered by a memory DC.
static int api_CreateMetaFile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t name = w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    char path[128] = "";
    for (unsigned k = 0; name >> 16 && k + 1 < sizeof path; k++) {
        const uint8_t *ch = far_ptr(w, name + k, 1);
        if (!ch || !*ch) break;
        path[k] = (char)*ch;
    }
    struct mdc *g = realloc(g_mdc, sizeof *g * (size_t)(g_nmdc + 1));
    if (!g) return 4;
    g_mdc = g;
    uint16_t hdc = w16_gdi_new_memory_dc(w);
    if (!hdc) return 4;
    struct mdc *m = &g_mdc[g_nmdc++];
    memset(m, 0, sizeof *m);
    m->hdc = hdc;
    if (path[0]) m->path = strdup(path);
    w->cpu.r[R_AX] = hdc;
    return 4;
}

// CloseMetaFile(hdc) -> HMETAFILE
// Windows: Ends a metafile and returns it, for PlayMetaFile and the clipboard.
// w16: The same: the records behind a METAHEADER, in a global block -- and, for one made with a file
// name, in that file too.
static int api_CloseMetaFile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    struct mdc *m = mdc_of(hdc);
    if (!m) return 2;
    static const uint16_t eof[3] = { 3, 0, 0 };
    size_t total = 18 + m->n + sizeof eof;
    uint8_t *mf = malloc(total);
    if (mf) {
        uint32_t words = (uint32_t)(total / 2), longest = m->longest > 3 ? m->longest : 3;
        uint16_t hd[9] = { (uint16_t)(m->path ? 2 : 1), 9, 0x300, (uint16_t)words, (uint16_t)(words >> 16),
                           (uint16_t)m->nslot, (uint16_t)longest, (uint16_t)(longest >> 16), 0 };
        memcpy(mf, hd, 18);
        if (m->n) memcpy(mf + 18, m->buf, m->n);
        memcpy(mf + 18 + m->n, eof, sizeof eof);
        if (m->path) {
            char host[256];
            FILE *f = w16_dos_path(m->path, host, sizeof host, 1, 0, 0) == 0 ? fopen(host, "wb") : 0;
            if (f) { fwrite(mf, 1, total, f); fclose(f); }
        }
        w->cpu.r[R_AX] = w16_global_alloc(w, (uint32_t)total, mf);
        free(mf);
    }
    free(m->buf);
    free(m->slot);
    free(m->path);
    *m = g_mdc[--g_nmdc];
    w16_gdi_delete(w, hdc);
    return 2;
}

// DeleteMetaFile(hmf) -> BOOL
// Windows: Frees a metafile's handle (a disk metafile's file stays).
// w16: The same.
static int api_DeleteMetaFile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t size;
    int ok = w16_global_ptr(w, h, &size) != 0;
    if (ok) w16_global_free(w, h);
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 2;
}

// GetMetaFileBits(hmf) -> HGLOBAL
// Windows: A metafile as a global block of its bytes; the metafile's handle is no longer valid.
// w16: The same block: a metafile here is one already.
static int api_GetMetaFileBits(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w->cpu.r[R_AX] = w16_arg16(a);
    return 2;
}

const struct w16_api g_meta_api[] = {
    { 123, api_PlayMetaFile },
    { 125, api_CreateMetaFile },
    { 126, api_CloseMetaFile },
    { 127, api_DeleteMetaFile },
    { 159, api_GetMetaFileBits },
};
const unsigned g_meta_api_n = sizeof g_meta_api / sizeof g_meta_api[0];
