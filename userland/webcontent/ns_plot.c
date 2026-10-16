// NetSurf's draw calls, onto a ugfx surface (ns_plot.h).
//
// **EVERY CLIP NETSURF ASKS FOR IS INTERSECTED WITH THE VIEW'S.** A page
// clips to its own boxes, which may lie anywhere on the page; the view's
// rectangle is what keeps those boxes off the toolbar and the window's
// chrome, so it is the ceiling every clip call is cut to.
#include <math.h>
#include <stdlib.h>

#include "utils/errors.h"

#include "lib/upixel.h"
#include "webcontent/ns_bitmap.h"
#include "webcontent/ns_font.h"
#include "webcontent/ns_plot.h"

#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"

// NetSurf's colour is 0x00BBGGRR; ugfx's 0xAARRGGBB.
static uint32_t rgb(colour c) {
    return ugfx_rgb((uint8_t)(c & 0xff), (uint8_t)((c >> 8) & 0xff), (uint8_t)((c >> 16) & 0xff));
}

static struct ns_target *T(const struct redraw_context *ctx) { return ctx->priv; }

static nserror p_clip(const struct redraw_context *ctx, const struct rect *r) {
    struct ns_target *t = T(ctx);
    int x0 = r->x0 > t->cx0 ? r->x0 : t->cx0;
    int y0 = r->y0 > t->cy0 ? r->y0 : t->cy0;
    int x1 = r->x1 < t->cx1 ? r->x1 : t->cx1;
    int y1 = r->y1 < t->cy1 ? r->y1 : t->cy1;
    // An empty intersection is a clip that admits nothing -- which
    // ugfx_set_clip_rect() means by a non-positive size.
    ugfx_set_clip_rect(t->s, x0, y0, x1 - x0, y1 - y0);
    return NSERROR_OK;
}

static int stroke_w(const plot_style_t *st) {
    int w = plot_style_fixed_to_int(st->stroke_width);
    return w < 1 ? 1 : w;
}

static nserror p_rectangle(const struct redraw_context *ctx, const plot_style_t *st,
                           const struct rect *r) {
    struct ugfx_surface *s = T(ctx)->s;
    int w = r->x1 - r->x0, h = r->y1 - r->y0;
    if (st->fill_type != PLOT_OP_TYPE_NONE && w > 0 && h > 0)
        ugfx_fill_rect(s, r->x0, r->y0, w, h, rgb(st->fill_colour));
    if (st->stroke_type != PLOT_OP_TYPE_NONE) {
        uint32_t c = rgb(st->stroke_colour);
        for (int i = 0, n = stroke_w(st); i < n && w - 2 * i > 0 && h - 2 * i > 0; i++)
            ugfx_draw_rect(s, r->x0 + i, r->y0 + i, w - 2 * i, h - 2 * i, c);
    }
    return NSERROR_OK;
}

static nserror p_line(const struct redraw_context *ctx, const plot_style_t *st, const struct rect *l) {
    struct ugfx_surface *s = T(ctx)->s;
    uint32_t c = rgb(st->stroke_colour);
    int w = stroke_w(st);
    // Borders are the common case and are always straight: a thick one
    // is a filled rectangle, centred on the line as NetSurf means it.
    if (l->y0 == l->y1) {
        int x0 = l->x0 < l->x1 ? l->x0 : l->x1, x1 = l->x0 < l->x1 ? l->x1 : l->x0;
        ugfx_fill_rect(s, x0, l->y0 - w / 2, x1 - x0 + 1, w, c);
    } else if (l->x0 == l->x1) {
        int y0 = l->y0 < l->y1 ? l->y0 : l->y1, y1 = l->y0 < l->y1 ? l->y1 : l->y0;
        ugfx_fill_rect(s, l->x0 - w / 2, y0, w, y1 - y0 + 1, c);
    } else {
        for (int i = 0; i < w; i++)
            ugfx_draw_line(s, l->x0 + i - w / 2, l->y0, l->x1 + i - w / 2, l->y1, c, GEOM_AA);
    }
    return NSERROR_OK;
}

static nserror p_disc(const struct redraw_context *ctx, const plot_style_t *st, int x, int y, int r) {
    struct ugfx_surface *s = T(ctx)->s;
    if (st->fill_type != PLOT_OP_TYPE_NONE) ugfx_fill_circle(s, x, y, r, rgb(st->fill_colour));
    if (st->stroke_type != PLOT_OP_TYPE_NONE) ugfx_draw_circle(s, x, y, r, rgb(st->stroke_colour), GEOM_AA);
    return NSERROR_OK;
}

// An arc, as segments: angles in degrees, anticlockwise from 3 o'clock,
// y down the screen as everywhere here.
static nserror p_arc(const struct redraw_context *ctx, const plot_style_t *st,
                     int x, int y, int r, int a1, int a2) {
    struct ugfx_surface *s = T(ctx)->s;
    if (a2 < a1) a2 += 360;
    int steps = (a2 - a1) / 10 + 1;
    int px = 0, py = 0;
    for (int i = 0; i <= steps; i++) {
        double a = (a1 + (double)(a2 - a1) * i / steps) * 3.14159265358979 / 180.0;
        int nx = x + (int)round(r * cos(a)), ny = y - (int)round(r * sin(a));
        if (i) ugfx_draw_line(s, px, py, nx, ny, rgb(st->stroke_colour), GEOM_AA);
        px = nx;
        py = ny;
    }
    return NSERROR_OK;
}

#define POLY_MAX 512

static nserror p_polygon(const struct redraw_context *ctx, const plot_style_t *st,
                         const int *p, unsigned int n) {
    static int xs[POLY_MAX], ys[POLY_MAX];
    if (n > POLY_MAX) n = POLY_MAX;
    for (unsigned i = 0; i < n; i++) { xs[i] = p[2 * i]; ys[i] = p[2 * i + 1]; }
    struct ugfx_surface *s = T(ctx)->s;
    if (st->fill_type != PLOT_OP_TYPE_NONE) ugfx_fill_polygon(s, xs, ys, (int)n, rgb(st->fill_colour));
    if (st->stroke_type != PLOT_OP_TYPE_NONE)
        ugfx_draw_polyline(s, xs, ys, (int)n, 1, rgb(st->stroke_colour), GEOM_AA);
    return NSERROR_OK;
}

// A path's points, flattened and transformed, one contour after another
// (ends[i] is one past contour i's last point).
struct flat_path {
    struct upix_pt *p;
    int *ends;
    unsigned char *closed;   // per contour
    int n, cap, nc, ccap;
};

// A path holding more points than this is drawn as far as it got: an SVG
// that flattens to millions of points is an attack on the page's memory,
// not a picture.
#define PATH_MAX_POINTS (1 << 20)

static int fp_point(struct flat_path *f, float x, float y) {
    if (f->n == f->cap) {
        if (f->cap >= PATH_MAX_POINTS) return -1;
        int cap = f->cap ? f->cap * 2 : 256;
        struct upix_pt *p = realloc(f->p, sizeof *p * (size_t)cap);
        if (!p) return -1;
        f->p = p;
        f->cap = cap;
    }
    f->p[f->n++] = (struct upix_pt){ x, y };
    return 0;
}

// Ends the contour in progress; a lone point is no contour.
static int fp_end(struct flat_path *f, int start, int closed) {
    if (f->n - start < 2) { f->n = start; return 0; }
    if (f->nc == f->ccap) {
        int cap = f->ccap ? f->ccap * 2 : 16;
        int *e = realloc(f->ends, sizeof *e * (size_t)cap);
        if (!e) return -1;
        f->ends = e;
        unsigned char *c = realloc(f->closed, (size_t)cap);
        if (!c) return -1;
        f->closed = c;
        f->ccap = cap;
    }
    f->ends[f->nc] = f->n;
    f->closed[f->nc++] = (unsigned char)closed;
    return 0;
}

static void fp_free(struct flat_path *f) {
    free(f->p);
    free(f->ends);
    free(f->closed);
}

static float dist(struct upix_pt a, struct upix_pt b) {
    return sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

// NetSurf's path -- MOVE x y, LINE x y, BEZIER c1 c2 end, CLOSE -- in
// page space through `tr`, curves flattened to chords no longer than a
// few pixels.
static void flatten(struct flat_path *f, const float *p, unsigned int n, const float tr[6]) {
#define TX(x, y) (tr[0] * (x) + tr[2] * (y) + tr[4])
#define TY(x, y) (tr[1] * (x) + tr[3] * (y) + tr[5])
    int start = 0;
    struct upix_pt cur = { 0, 0 }, first = { 0, 0 };
    for (unsigned i = 0; i < n;) {
        int cmd = (int)p[i];
        if (cmd == PLOTTER_PATH_MOVE && i + 2 < n) {
            if (fp_end(f, start, 0)) return;
            start = f->n;
            cur = first = (struct upix_pt){ TX(p[i + 1], p[i + 2]), TY(p[i + 1], p[i + 2]) };
            if (fp_point(f, cur.x, cur.y)) return;
            i += 3;
        } else if (cmd == PLOTTER_PATH_LINE && i + 2 < n) {
            cur = (struct upix_pt){ TX(p[i + 1], p[i + 2]), TY(p[i + 1], p[i + 2]) };
            if (f->n == start && fp_point(f, cur.x, cur.y)) return;   // no MOVE first
            if (fp_point(f, cur.x, cur.y)) return;
            i += 3;
        } else if (cmd == PLOTTER_PATH_BEZIER && i + 6 < n) {
            struct upix_pt c1 = { TX(p[i + 1], p[i + 2]), TY(p[i + 1], p[i + 2]) };
            struct upix_pt c2 = { TX(p[i + 3], p[i + 4]), TY(p[i + 3], p[i + 4]) };
            struct upix_pt e = { TX(p[i + 5], p[i + 6]), TY(p[i + 5], p[i + 6]) };
            // The control polygon's length bounds the curve's.
            float len = dist(cur, c1) + dist(c1, c2) + dist(c2, e);
            int segs = (int)(len / 3) + 2;
            if (segs > 64) segs = 64;
            struct upix_pt out[65];
            upix_bezier(cur, c1, c2, e, out, segs);
            if (f->n == start && fp_point(f, cur.x, cur.y)) return;
            for (int k = 1; k <= segs; k++)
                if (fp_point(f, out[k].x, out[k].y)) return;
            cur = e;
            i += 7;
        } else if (cmd == PLOTTER_PATH_CLOSE) {
            if (fp_end(f, start, 1)) return;
            start = f->n;
            cur = first;
            i += 1;
        } else {
            break;   // a malformed path is drawn as far as it is well formed
        }
    }
    fp_end(f, start, 0);
#undef TX
#undef TY
}

static void paint_mask(struct ugfx_surface *s, const struct upix_mask *m, uint32_t colour) {
    for (int y = 0; y < m->h; y++)
        ugfx_blend_hspan(s, m->x, m->y + y, m->w, colour, m->a + (size_t)y * m->w, 255);
}

// SVG's path, anti-aliased: the fill NONZERO (SVG's default -- a
// letter's counter is a contour wound the other way), the stroke round
// at joins and caps, each laid on by its coverage. libsvgtiny has made
// arcs into curves and gradients into flat triangles already.
static nserror p_path(const struct redraw_context *ctx, const plot_style_t *st,
                      const float *p, unsigned int n, const float tr[6]) {
    struct ugfx_surface *s = T(ctx)->s;
    struct flat_path f = { 0 };
    flatten(&f, p, n, tr);
    if (st->fill_type != PLOT_OP_TYPE_NONE && f.nc) {
        struct upix_mask m;
        if (upix_mask_fill_rule(&m, f.p, f.ends, f.nc, UPIX_NONZERO, s->w, s->h, 1) == 0) {
            paint_mask(s, &m, rgb(st->fill_colour));
            upix_mask_free(&m);
        }
    }
    if (st->stroke_type != PLOT_OP_TYPE_NONE) {
        // A stroke scales with the drawing: by the transform's area scale.
        float w = plot_style_fixed_to_float(st->stroke_width) * sqrtf(fabsf(tr[0] * tr[3] - tr[1] * tr[2]));
        if (w < 1) w = 1;
        for (int c = 0, start = 0; c < f.nc; start = f.ends[c], c++) {
            struct upix_mask m;
            if (upix_mask_stroke(&m, f.p + start, f.ends[c] - start, f.closed[c], w, s->w, s->h, 1) == 0) {
                paint_mask(s, &m, rgb(st->stroke_colour));
                upix_mask_free(&m);
            }
        }
    }
    fp_free(&f);
    return NSERROR_OK;
}

static void blit_one(struct ugfx_surface *s, struct bitmap *b, const uint32_t *px, int x, int y, int w, int h) {
    if (b->opaque) ugfx_blit(s, x, y, w, h, px, w);
    else ugfx_blit_alpha(s, x, y, w, h, px, w);
}

static nserror p_bitmap(const struct redraw_context *ctx, struct bitmap *b, int x, int y,
                        int w, int h, colour bg, bitmap_flags_t flags) {
    (void)bg;
    struct ns_target *t = T(ctx);
    if (!b || w <= 0 || h <= 0) return NSERROR_OK;
    const uint32_t *px = ns_bitmap_at_size(b, w, h);
    if (!px) return NSERROR_OK;
    int rx = (flags & BITMAPF_REPEAT_X) != 0, ry = (flags & BITMAPF_REPEAT_Y) != 0;
    if (!rx && !ry) {
        blit_one(t->s, b, px, x, y, w, h);
        return NSERROR_OK;
    }
    // Tiled: from the tile at or before the clip's edge to past its far
    // edge, so the pattern stays anchored to (x, y) wherever it is cut.
    struct ugfx_surface *s = t->s;
    int cx0 = t->cx0, cy0 = t->cy0, cx1 = t->cx1, cy1 = t->cy1;
    if (s->clip_active) {
        if (s->clip_x0 > cx0) cx0 = s->clip_x0;
        if (s->clip_y0 > cy0) cy0 = s->clip_y0;
        if (s->clip_x1 < cx1) cx1 = s->clip_x1;
        if (s->clip_y1 < cy1) cy1 = s->clip_y1;
    }
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (rx) {
        x0 = cx0 - ((cx0 - x) % w + w) % w;
        x1 = cx1;
    }
    if (ry) {
        y0 = cy0 - ((cy0 - y) % h + h) % h;
        y1 = cy1;
    }
    for (int ty = y0; ty < y1; ty += h)
        for (int tx = x0; tx < x1; tx += w) blit_one(s, b, px, tx, ty, w, h);
    return NSERROR_OK;
}

static nserror p_text(const struct redraw_context *ctx, const plot_font_style_t *fs,
                      int x, int y, const char *text, size_t len) {
    ns_font_draw(T(ctx)->s, fs, x, y, text, len, rgb(fs->foreground));
    return NSERROR_OK;
}

static const struct plotter_table g_plotters = {
    .clip = p_clip,
    .arc = p_arc,
    .disc = p_disc,
    .line = p_line,
    .rectangle = p_rectangle,
    .polygon = p_polygon,
    .path = p_path,
    .bitmap = p_bitmap,
    .text = p_text,
    .option_knockout = true,
};

void ns_plot_context(struct redraw_context *ctx, struct ns_target *t) {
    ctx->interactive = true;
    ctx->background_images = true;
    ctx->plot = &g_plotters;
    ctx->priv = t;
}
