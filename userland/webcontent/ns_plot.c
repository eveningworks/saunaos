// NetSurf's draw calls, onto a ugfx surface (ns_plot.h).
//
// **EVERY CLIP NETSURF ASKS FOR IS INTERSECTED WITH THE VIEW'S.** A page
// clips to its own boxes, which may lie anywhere on the page; the view's
// rectangle is what keeps those boxes off the toolbar and the window's
// chrome, so it is the ceiling every clip call is cut to.
#include <math.h>
#include <stdlib.h>

#include "utils/errors.h"

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

// SVG's path. Curves are drawn as their chords: no SVG content is
// built yet (docs/browser-design.md), so nothing reaches this but the
// odd decoration.
static nserror p_path(const struct redraw_context *ctx, const plot_style_t *st,
                      const float *p, unsigned int n, const float tr[6]) {
    static int xs[POLY_MAX], ys[POLY_MAX];
    int count = 0;
    for (unsigned i = 0; i < n && count < POLY_MAX;) {
        int cmd = (int)p[i];
        if (cmd == PLOTTER_PATH_MOVE || cmd == PLOTTER_PATH_LINE) {
            float x = p[i + 1], y = p[i + 2];
            xs[count] = (int)(tr[0] * x + tr[2] * y + tr[4]);
            ys[count] = (int)(tr[1] * x + tr[3] * y + tr[5]);
            count++;
            i += 3;
        } else if (cmd == PLOTTER_PATH_BEZIER) {
            float x = p[i + 5], y = p[i + 6];
            xs[count] = (int)(tr[0] * x + tr[2] * y + tr[4]);
            ys[count] = (int)(tr[1] * x + tr[3] * y + tr[5]);
            count++;
            i += 7;
        } else {
            i += 1;
        }
    }
    if (count < 2) return NSERROR_OK;
    struct ugfx_surface *s = T(ctx)->s;
    if (st->fill_type != PLOT_OP_TYPE_NONE && count > 2)
        ugfx_fill_polygon(s, xs, ys, count, rgb(st->fill_colour));
    if (st->stroke_type != PLOT_OP_TYPE_NONE)
        ugfx_draw_polyline(s, xs, ys, count, 0, rgb(st->stroke_colour), GEOM_AA);
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
