// uwmf_draw.c -- a 16-bit metafile played into a box of a ugfx surface (uwmf_draw.h). The records are
// lib/uwmf's; this keeps the device context they change -- the window's origin and extent, the pen,
// brush and font selected, the text colours and alignment -- and maps each logical point onto the box.
#include "ui/uwmf_draw.h"

#include <stdlib.h>
#include <string.h>
#include "lib/uimg.h"
#include "lib/uwmf.h"
#include "ui/ugfx.h"

enum { OBJ_NONE, OBJ_PEN, OBJ_BRUSH, OBJ_FONT, OBJ_OTHER };

struct obj {
    int kind;
    int style, width;              // a pen's PS_* and width; a brush's BS_*
    uint32_t rgb;
    int height, weight, italic;    // a font's
    char face[32];
};

struct dc {
    int ox, oy, ex, ey;            // the window: origin and extent, logical units
    struct obj pen, brush, font;   // what is selected, by value (DeleteObject leaves a selection drawn)
    uint32_t text, bk;
    int bkmode, align;             // 1 TRANSPARENT, 2 OPAQUE; TA_*
    int cx, cy;                    // the current position, logical
};

struct play {
    struct ugfx_surface *s;
    int bx, by, bw, bh;            // the box
    struct dc dc;
    struct dc *stack;              // SaveDC's copies
    int nstack, stack_cap;
    struct obj *obj;               // the object table
    unsigned nobj;
    uwmf_font_fn font;
    void *ctx;
};

static uint32_t rgb_of(uint32_t colorref) {
    return 0xFF000000u | (colorref & 0xFF) << 16 | (colorref & 0xFF00) | (colorref >> 16 & 0xFF);
}

static int map_x(const struct play *p, int lx) {
    int ex = p->dc.ex ? p->dc.ex : 1;
    return p->bx + (int)((long long)(lx - p->dc.ox) * p->bw / ex);
}

static int map_y(const struct play *p, int ly) {
    int ey = p->dc.ey ? p->dc.ey : 1;
    return p->by + (int)((long long)(ly - p->dc.oy) * p->bh / ey);
}

static int len_y(const struct play *p, int l) {
    long long v = (long long)l * p->bh / (p->dc.ey ? p->dc.ey : 1);
    return (int)(v < 0 ? -v : v);
}

static int pen_on(const struct play *p) { return p->dc.pen.style != 5; }        // PS_NULL
static int brush_on(const struct play *p) { return p->dc.brush.style != 1; }    // BS_NULL / BS_HOLLOW

static void save(struct play *p) {
    if (p->nstack == p->stack_cap) {
        int nc = p->stack_cap ? p->stack_cap * 2 : 8;
        struct dc *g = realloc(p->stack, (size_t)nc * sizeof *g);
        if (!g) return;
        p->stack = g;
        p->stack_cap = nc;
    }
    p->stack[p->nstack++] = p->dc;
}

// RestoreDC(n): a negative n counts back from the newest save, a positive one names a save.
static void restore(struct play *p, int n) {
    int to = n < 0 ? p->nstack + n : n - 1;
    if (to < 0 || to >= p->nstack) return;
    p->dc = p->stack[to];
    p->nstack = to;
}

static void outline(struct play *p, const int *xs, const int *ys, int n, int closed) {
    if (pen_on(p) && n > 1) ugfx_draw_polyline(p->s, xs, ys, n, closed, p->dc.pen.rgb, GEOM_ALIASED);
}

static void shape_points(struct play *p, const int16_t *pt, int npt, int fill) {
    if (npt < 2) return;
    int *xs = malloc((size_t)npt * 2 * sizeof *xs);
    if (!xs) return;
    int *ys = xs + npt;
    for (int i = 0; i < npt; i++) { xs[i] = map_x(p, pt[2 * i]); ys[i] = map_y(p, pt[2 * i + 1]); }
    if (fill && brush_on(p) && npt >= 3 && npt <= 256) ugfx_fill_polygon(p->s, xs, ys, npt, p->dc.brush.rgb);
    outline(p, xs, ys, npt, fill);
    free(xs);
}

static void rect_shape(struct play *p, const int16_t *v, int ellipse) {
    int x0 = map_x(p, v[0]), y0 = map_y(p, v[1]), x1 = map_x(p, v[2]), y1 = map_y(p, v[3]);
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    if (x1 <= x0 || y1 <= y0) return;
    if (ellipse) {
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
        if (brush_on(p)) ugfx_fill_ellipse(p->s, cx, cy, rx, ry, p->dc.brush.rgb);
        if (pen_on(p)) ugfx_draw_ellipse(p->s, cx, cy, rx, ry, p->dc.pen.rgb, GEOM_ALIASED);
        return;
    }
    if (brush_on(p)) ugfx_fill_rect(p->s, x0, y0, x1 - x0, y1 - y0, p->dc.brush.rgb);
    if (pen_on(p)) {
        int xs[4] = { x0, x1 - 1, x1 - 1, x0 }, ys[4] = { y0, y0, y1 - 1, y1 - 1 };
        outline(p, xs, ys, 4, 1);
    }
}

// The DIB a record carries, as a picture: a BMP file made of it, decoded by lib/uimg's BMP codec.
static int dib_image(const uint8_t *dib, uint32_t n, struct uimg *out) {
    if (!dib || n < 12) return -1;
    uint32_t hs = (uint32_t)dib[0] | (uint32_t)dib[1] << 8 | (uint32_t)dib[2] << 16 | (uint32_t)dib[3] << 24;
    if (hs != 12 && (hs < 40 || hs > n)) return -1;
    unsigned bits, used = 0, entry = hs == 12 ? 3 : 4;
    if (hs == 12) bits = dib[10] | dib[11] << 8;
    else {
        bits = dib[14] | dib[15] << 8;
        used = (unsigned)dib[32] | (unsigned)dib[33] << 8;
    }
    unsigned colours = used ? used : bits <= 8 ? 1u << bits : 0;
    size_t off = 14 + hs + (size_t)colours * entry;
    if (off - 14 > n) return -1;
    uint8_t *f = malloc(14 + (size_t)n);
    if (!f) return -1;
    size_t total = 14 + (size_t)n;
    f[0] = 'B'; f[1] = 'M';
    for (int i = 0; i < 4; i++) f[2 + i] = (uint8_t)(total >> (8 * i));
    memset(f + 6, 0, 4);
    for (int i = 0; i < 4; i++) f[10 + i] = (uint8_t)(off >> (8 * i));
    memcpy(f + 14, dib, n);
    int rc = uimg_decode(f, total, out);
    free(f);
    return rc;
}

// The source rectangle of a DIB (rows counted from the bottom, as StretchDIBits counts them) into the
// destination rectangle, logical units.
static void dib_blit(struct play *p, const struct uwmf_rec *r, int xd, int yd, int wd, int hd,
                     int xs, int ys, int ws, int hs) {
    struct uimg im = { 0 };
    if (dib_image(r->dib, r->ndib, &im) < 0) return;
    if (ws <= 0 || hs <= 0 || xs < 0 || ys < 0 || xs + ws > im.w || ys + hs > im.h) { xs = ys = 0; ws = im.w; hs = im.h; }
    int top = im.h - ys - hs;                    // lib/uimg's rows are top first
    int x0 = map_x(p, xd), y0 = map_y(p, yd), x1 = map_x(p, xd + wd), y1 = map_y(p, yd + hd);
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
        struct uimg part = { .w = ws, .h = hs, .px = im.px + (size_t)top * im.w + xs, .has_alpha = 0 };
        struct uimg crop = { 0 }, scaled = { 0 };
        // uimg_scale reads a w x h buffer: a sub-rectangle is copied out first.
        if (ws != im.w || hs != im.h) {
            crop.px = malloc((size_t)ws * hs * sizeof *crop.px);
            if (crop.px) {
                crop.w = ws;
                crop.h = hs;
                for (int y = 0; y < hs; y++) memcpy(crop.px + (size_t)y * ws, part.px + (size_t)y * im.w, (size_t)ws * 4);
            }
        }
        const struct uimg *src = crop.px ? &crop : &im;
        if (w == src->w && h == src->h) ugfx_blit(p->s, x0, y0, w, h, src->px, src->w);
        else if (uimg_scale(src, w, h, &scaled) == 0) ugfx_blit(p->s, x0, y0, w, h, scaled.px, scaled.w);
        uimg_free(&scaled);
        free(crop.px);
    }
    uimg_free(&im);
}

// A run of characters at logical (x, y), placed by the text alignment, in the selected font.
static void text(struct play *p, int lx, int ly, const char *chars, int n, const int16_t *dx, const int16_t *box,
                 int opaque) {
    if (n <= 0) return;
    char *s = malloc((size_t)n + 1);
    if (!s) return;
    memcpy(s, chars, (size_t)n);
    s[n] = 0;
    int px = p->dc.font.height ? len_y(p, p->dc.font.height) : 0;
    if (p->dc.font.height > 0) px = px * 4 / 5;           // a positive height is the cell: the character is less
    if (px < 6) px = px ? 6 : 13;
    const struct ugfx_font *f = p->font ? p->font(p->ctx, p->dc.font.face, px, p->dc.font.weight >= 600,
                                                  p->dc.font.italic) : 0;
    const struct ugfx_font *was = ugfx_font_current();
    if (f) ugfx_set_font(f);
    if (opaque && box) {
        int x0 = map_x(p, box[0]), y0 = map_y(p, box[1]), x1 = map_x(p, box[2]), y1 = map_y(p, box[3]);
        if (x1 > x0 && y1 > y0) ugfx_fill_rect(p->s, x0, y0, x1 - x0, y1 - y0, rgb_of(p->dc.bk & 0xFFFFFF));
    }
    int x = map_x(p, lx), y = map_y(p, ly), lh = ugfx_char_h(), tw = ugfx_text_width(s);
    int va = p->dc.align & 24, ha = p->dc.align & 6;
    if (va == 24) y -= lh * 4 / 5;                        // TA_BASELINE
    else if (va == 8) y -= lh;                            // TA_BOTTOM
    if (ha == 6) x -= tw / 2;                             // TA_CENTER
    else if (ha == 2) x -= tw;                            // TA_RIGHT
    uint32_t bg = p->dc.bkmode == 2 ? (p->dc.bk & 0xFFFFFF) : UGFX_TRANSPARENT;
    if (dx) {
        int at = 0, shift = x - map_x(p, lx);              // the alignment's offset, kept for every character
        for (int i = 0; i < n; i++) {
            char one[2] = { s[i], 0 };
            ugfx_draw_string(p->s, map_x(p, lx + at) + shift, y, one, rgb_of(p->dc.text), bg);
            at += dx[i];
        }
    } else {
        ugfx_draw_string(p->s, x, y, s, rgb_of(p->dc.text), bg);
    }
    ugfx_set_font(was);
    free(s);
}

static void make(struct play *p, const struct uwmf_rec *r, int kind) {
    if (r->slot < 0) return;
    if ((unsigned)r->slot >= p->nobj) {
        unsigned nn = (unsigned)r->slot + 8;
        struct obj *g = realloc(p->obj, nn * sizeof *g);
        if (!g) return;
        memset(g + p->nobj, 0, (nn - p->nobj) * sizeof *g);
        p->obj = g;
        p->nobj = nn;
    }
    struct obj *o = &p->obj[r->slot];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->rgb = rgb_of(r->color);
    if (kind == OBJ_PEN) { o->style = r->v[0] & 7; o->width = r->v[1]; }
    else if (kind == OBJ_BRUSH) o->style = r->v[0];
    else if (kind == OBJ_FONT) {
        o->height = r->v[0];
        o->weight = r->v[4];
        o->italic = r->nv > 5 && r->v[5];
        memcpy(o->face, r->face, sizeof o->face);
        o->face[sizeof o->face - 1] = 0;
    }
}

int uwmf_draw(struct ugfx_surface *s, const void *data, size_t n, int x, int y, int w, int h,
              uwmf_font_fn font, void *ctx) {
    struct uwmf m;
    if (w <= 0 || h <= 0 || uwmf_open(&m, data, n) < 0) return -1;
    struct play p = { .s = s, .bx = x, .by = y, .bw = w, .bh = h, .font = font, .ctx = ctx };
    p.dc.ex = w;
    p.dc.ey = h;
    if (m.placeable) {                                     // its bounding box is the window until a record says otherwise
        p.dc.ox = m.bounds[0];
        p.dc.oy = m.bounds[1];
        p.dc.ex = m.bounds[2] - m.bounds[0];
        p.dc.ey = m.bounds[3] - m.bounds[1];
    }
    p.dc.pen = (struct obj){ .kind = OBJ_PEN, .style = 0, .width = 1, .rgb = rgb_of(0) };
    p.dc.brush = (struct obj){ .kind = OBJ_BRUSH, .style = 0, .rgb = rgb_of(0xFFFFFF) };
    p.dc.font = (struct obj){ .kind = OBJ_FONT };
    p.dc.text = 0;
    p.dc.bk = 0xFFFFFF;
    p.dc.bkmode = 2;
    p.nobj = m.nobjects ? m.nobjects : 1;
    p.obj = calloc(p.nobj, sizeof *p.obj);
    if (!p.obj) { uwmf_close(&m); return -1; }
    struct ugfx_clip clip;
    ugfx_clip_save(s, &clip);
    ugfx_clip_intersect(s, x, y, w, h);
    struct uwmf_rec r;
    int got, played = 0;
    while ((got = uwmf_next(&m, &r)) > 0) {
        const int16_t *v = r.v;
        played++;
        switch (r.func) { // dispatch-ok: bounded by MS-WMF's record types, each a different drawing step
        case UWMF_SETWINDOWORG:    if (r.nv >= 2) { p.dc.ox = v[0]; p.dc.oy = v[1]; } break;
        case UWMF_SETWINDOWEXT:    if (r.nv >= 2 && v[0] && v[1]) { p.dc.ex = v[0]; p.dc.ey = v[1]; } break;
        case UWMF_OFFSETWINDOWORG: if (r.nv >= 2) { p.dc.ox += v[0]; p.dc.oy += v[1]; } break;
        case UWMF_SCALEWINDOWEXT:
            if (r.nv >= 4 && v[1] && v[3] && v[0] && v[2]) { p.dc.ex = p.dc.ex * v[0] / v[1]; p.dc.ey = p.dc.ey * v[2] / v[3]; }
            break;
        case UWMF_SAVEDC:       save(&p); break;
        case UWMF_RESTOREDC:    if (r.nv >= 1) restore(&p, v[0]); break;
        case UWMF_SETTEXTCOLOR: p.dc.text = r.color; break;
        case UWMF_SETBKCOLOR:   p.dc.bk = r.color; break;
        case UWMF_SETBKMODE:    if (r.nv >= 1) p.dc.bkmode = v[0]; break;
        case UWMF_SETTEXTALIGN: if (r.nv >= 1) p.dc.align = (uint16_t)v[0]; break;
        case UWMF_CREATEPENINDIRECT:   make(&p, &r, OBJ_PEN); break;
        case UWMF_CREATEBRUSHINDIRECT: make(&p, &r, OBJ_BRUSH); break;
        case UWMF_CREATEFONTINDIRECT:  make(&p, &r, OBJ_FONT); break;
        case UWMF_CREATEPALETTE: case UWMF_CREATEPATTERNBRUSH: case UWMF_DIBCREATEPATTERNBRUSH:
        case UWMF_CREATEREGION:
            make(&p, &r, OBJ_OTHER);
            break;
        case UWMF_SELECTOBJECT:
            if (r.slot >= 0 && (unsigned)r.slot < p.nobj) {
                const struct obj *o = &p.obj[r.slot];
                if (o->kind == OBJ_PEN) p.dc.pen = *o;
                else if (o->kind == OBJ_BRUSH) p.dc.brush = *o;
                else if (o->kind == OBJ_FONT) p.dc.font = *o;
            }
            break;
        case UWMF_DELETEOBJECT:
            if (r.slot >= 0 && (unsigned)r.slot < p.nobj) p.obj[r.slot].kind = OBJ_NONE;
            break;
        case UWMF_MOVETO: if (r.nv >= 2) { p.dc.cx = v[0]; p.dc.cy = v[1]; } break;
        case UWMF_LINETO:
            if (r.nv >= 2) {
                if (pen_on(&p))
                    ugfx_draw_line(s, map_x(&p, p.dc.cx), map_y(&p, p.dc.cy), map_x(&p, v[0]), map_y(&p, v[1]),
                                   p.dc.pen.rgb, GEOM_ALIASED);
                p.dc.cx = v[0];
                p.dc.cy = v[1];
            }
            break;
        case UWMF_POLYLINE: shape_points(&p, r.pt, r.npt, 0); break;
        case UWMF_POLYGON:  shape_points(&p, r.pt, r.npt, 1); break;
        case UWMF_POLYPOLYGON: {
            int at = 0;
            for (int i = 0; i < r.ncounts && at + r.counts[i] <= r.npt; at += r.counts[i], i++)
                shape_points(&p, r.pt + 2 * at, r.counts[i], 1);
            break;
        }
        case UWMF_RECTANGLE: case UWMF_ROUNDRECT: if (r.nv >= 4) rect_shape(&p, v, 0); break;
        case UWMF_ELLIPSE: if (r.nv >= 4) rect_shape(&p, v, 1); break;
        case UWMF_PATBLT:
            if (r.nv >= 4) {
                // v: x, y, width, height; PATCOPY fills with the brush, BLACKNESS and WHITENESS with themselves
                uint32_t c = r.rop == 0x00000042 ? rgb_of(0) : r.rop == 0x00FF0062 ? rgb_of(0xFFFFFF) : p.dc.brush.rgb;
                int x0 = map_x(&p, v[0]), y0 = map_y(&p, v[1]), x1 = map_x(&p, v[0] + v[2]), y1 = map_y(&p, v[1] + v[3]);
                if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
                if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
                if (r.rop != 0x00AA0029) ugfx_fill_rect(s, x0, y0, x1 - x0, y1 - y0, c);   // not DSTCOPY
            }
            break;
        case UWMF_SETPIXEL: if (r.nv >= 2) ugfx_put_pixel(s, map_x(&p, v[0]), map_y(&p, v[1]), rgb_of(r.color)); break;
        case UWMF_TEXTOUT: if (r.nv >= 2) text(&p, v[0], v[1], r.text, r.ntext, 0, 0, 0); break;
        case UWMF_EXTTEXTOUT:
            if (r.nv >= 3) text(&p, v[0], v[1], r.text, r.ntext, r.dx, (v[2] & 6) ? r.rect : 0, v[2] & 2);
            break;
        case UWMF_DIBBITBLT:    if (r.nv >= 6) dib_blit(&p, &r, v[0], v[1], v[2], v[3], v[4], v[5], v[2], v[3]); break;
        case UWMF_DIBSTRETCHBLT:
        case UWMF_STRETCHDIB:   if (r.nv >= 8) dib_blit(&p, &r, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); break;
        case UWMF_SETDIBTODEV:  if (r.nv >= 6) dib_blit(&p, &r, v[0], v[1], v[2], v[3], v[4], v[5], v[2], v[3]); break;
        default: break;          // palettes, regions, clipping, escapes, arcs: not drawn (the header says so)
        }
    }
    ugfx_clip_restore(s, &clip);
    free(p.stack);
    free(p.obj);
    uwmf_close(&m);
    return played;
}
