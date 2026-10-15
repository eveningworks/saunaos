// Painting on a picture -- see lib/upixel.h.
#include "lib/upixel.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void upix_rect_add(struct upix_rect *r, const struct uimg *im, int x, int y, int w, int h) {
    if (!r) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > im->w) w = im->w - x;
    if (y + h > im->h) h = im->h - y;
    if (w <= 0 || h <= 0) return;
    if (r->w <= 0 || r->h <= 0) { *r = (struct upix_rect){ x, y, w, h }; return; }
    int x1 = r->x + r->w > x + w ? r->x + r->w : x + w;
    int y1 = r->y + r->h > y + h ? r->y + r->h : y + h;
    if (x < r->x) r->x = x;
    if (y < r->y) r->y = y;
    r->w = x1 - r->x;
    r->h = y1 - r->y;
}

uint32_t upix_get(const struct uimg *im, int x, int y) {
    if (!im->px || x < 0 || y < 0 || x >= im->w || y >= im->h) return 0;
    return im->px[(size_t)y * im->w + x];
}

void upix_stamp(struct uimg *im, int cx, int cy, int size, enum upix_shape shape,
                uint32_t colour, struct upix_rect *dirty) {
    if (!im->px || size < 1) return;
    int x0 = cx - (size - 1) / 2, y0 = cy - (size - 1) / 2;
    // IN DOUBLED COORDINATES, so an even brush's centre between two
    // pixels is a whole number: pixel i's centre is 2i - (size - 1) from
    // the brush's, and the disc is everything within `size` of it.
    int r2 = size * size;
    for (int j = 0; j < size; j++) {
        int y = y0 + j;
        if (y < 0 || y >= im->h) continue;
        int dy = 2 * j - (size - 1);
        uint32_t *row = im->px + (size_t)y * im->w;
        for (int i = 0; i < size; i++) {
            int x = x0 + i;
            if (x < 0 || x >= im->w) continue;
            int dx = 2 * i - (size - 1);
            if (shape == UPIX_ROUND && size > 2 && dx * dx + dy * dy > r2) continue;
            row[x] = colour;
        }
    }
    upix_rect_add(dirty, im, x0, y0, size, size);
}

void upix_line(struct uimg *im, int x0, int y0, int x1, int y1, int size,
               enum upix_shape shape, uint32_t colour, int from_start,
               struct upix_rect *dirty) {
    // Bresenham, all octants.
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, first = 1;
    for (;;) {
        if (!first || from_start) upix_stamp(im, x0, y0, size, shape, colour, dirty);
        first = 0;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int near(uint32_t a, uint32_t b, int tol) {
    for (int sh = 0; sh < 32; sh += 8) {
        int d = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

// A SCANLINE FLOOD over a stack of seeds: each pop takes the whole run of
// its row, then seeds the rows above and below once per run they touch.
// The bitmap is what ends it when the run's own action makes a pixel
// still match: with a tolerance, a pixel already filled can still look
// like the target. `run` is told each run, after it is marked.
typedef void (*flood_run)(void *ctx, int l, int r, int y);

static int flood(const struct uimg *im, int x, int y, int tolerance, flood_run run, void *ctx) {
    uint32_t target = im->px[(size_t)y * im->w + x];
    size_t n = (size_t)im->w * im->h;
    uint8_t *seen = calloc((n + 7) / 8, 1);
    if (!seen) return -ENOMEM;
    int cap = 1024, top = 0, count = 0;
    int *stack = malloc(sizeof(int) * 2 * (size_t)cap);
    if (!stack) { free(seen); return -ENOMEM; }
#define BIT(X, Y) ((size_t)(Y) * im->w + (size_t)(X))
#define SEEN(X, Y) (seen[BIT(X, Y) >> 3] & (1u << (BIT(X, Y) & 7)))
#define MARK(X, Y) (seen[BIT(X, Y) >> 3] |= (uint8_t)(1u << (BIT(X, Y) & 7)))
#define MATCH(X, Y) (!SEEN(X, Y) && near(im->px[BIT(X, Y)], target, tolerance))
    stack[top++] = x;
    stack[top++] = y;
    while (top) {
        int sy = stack[--top], sx = stack[--top];
        if (!MATCH(sx, sy)) continue;
        int l = sx, r = sx;
        while (l > 0 && MATCH(l - 1, sy)) l--;
        while (r + 1 < im->w && MATCH(r + 1, sy)) r++;
        for (int i = l; i <= r; i++) MARK(i, sy);
        run(ctx, l, r, sy);
        count += r - l + 1;
        for (int dyi = -1; dyi <= 1; dyi += 2) {
            int ny = sy + dyi;
            if (ny < 0 || ny >= im->h) continue;
            int in_run = 0;
            for (int i = l; i <= r; i++) {
                int m = MATCH(i, ny);
                if (m && !in_run) {
                    if (top + 2 > 2 * cap) {
                        int *grown = realloc(stack, sizeof(int) * 4 * (size_t)cap);
                        if (!grown) { free(stack); free(seen); return -ENOMEM; }
                        stack = grown;
                        cap *= 2;
                    }
                    stack[top++] = i;
                    stack[top++] = ny;
                }
                in_run = m;
            }
        }
    }
#undef BIT
#undef SEEN
#undef MARK
#undef MATCH
    free(stack);
    free(seen);
    return count;
}

struct fill_ctx { struct uimg *im; uint32_t colour; struct upix_rect *dirty; };

static void fill_run(void *p, int l, int r, int y) {
    struct fill_ctx *f = p;
    uint32_t *row = f->im->px + (size_t)y * f->im->w;
    for (int i = l; i <= r; i++) row[i] = f->colour;
    upix_rect_add(f->dirty, f->im, l, y, r - l + 1, 1);
}

int upix_fill(struct uimg *im, int x, int y, uint32_t colour, int tolerance,
              struct upix_rect *dirty) {
    if (!im->px || x < 0 || y < 0 || x >= im->w || y >= im->h) return 0;
    if (im->px[(size_t)y * im->w + x] == colour && tolerance == 0) return 0;
    struct fill_ctx f = { im, colour, dirty };
    return flood(im, x, y, tolerance, fill_run, &f);
}

// One pixel of `s` at `opacity` over `d`.
static uint32_t over_px(uint32_t dp, uint32_t sp, int opacity) {
    unsigned sa = ((sp >> 24) * (unsigned)opacity + 127) / 255;
    if (!sa) return dp;
    if (sa == 255) return sp | 0xFF000000u;
    // da' = da * (1 - sa), so out alpha = sa + da'; each channel the two
    // weighted by their alpha, over out alpha.
    unsigned da = ((dp >> 24) * (255 - sa) + 127) / 255, oa = sa + da;
    uint32_t out = oa << 24;
    for (int sh = 0; sh <= 16; sh += 8) {
        unsigned cs = (sp >> sh) & 0xFF, cd = (dp >> sh) & 0xFF;
        out |= ((cs * sa + cd * da + oa / 2) / oa) << sh;
    }
    return out;
}

void upix_over(struct uimg *dst, const struct uimg *src, const struct upix_rect *r, int opacity) {
    if (!dst->px || !src->px || dst->w != src->w || dst->h != src->h || opacity <= 0) return;
    if (opacity > 255) opacity = 255;
    struct upix_rect c = { 0, 0, 0, 0 };
    upix_rect_add(&c, dst, r->x, r->y, r->w, r->h);
    for (int y = c.y; y < c.y + c.h; y++) {
        uint32_t *d = dst->px + (size_t)y * dst->w;
        const uint32_t *s = src->px + (size_t)y * src->w;
        for (int x = c.x; x < c.x + c.w; x++) d[x] = over_px(d[x], s[x], opacity);
    }
}

void upix_over_at(struct uimg *dst, int x0, int y0, const struct uimg *src) {
    if (!dst->px || !src->px) return;
    struct upix_rect c = { 0, 0, 0, 0 };
    upix_rect_add(&c, dst, x0, y0, src->w, src->h);
    for (int y = c.y; y < c.y + c.h; y++) {
        uint32_t *d = dst->px + (size_t)y * dst->w;
        const uint32_t *s = src->px + (size_t)(y - y0) * src->w - x0;
        for (int x = c.x; x < c.x + c.w; x++) d[x] = over_px(d[x], s[x], 255);
    }
}

// --- shapes ----------------------------------------------------------------

void upix_mask_free(struct upix_mask *m) {
    free(m->a);
    *m = (struct upix_mask){ 0 };
}

// The points' box grown by `pad`, clipped to the picture.
static int mask_alloc(struct upix_mask *m, const struct upix_pt *p, int n, float pad, int cw, int ch) {
    *m = (struct upix_mask){ 0 };
    if (n < 1) return 0;
    float x0 = p[0].x, y0 = p[0].y, x1 = x0, y1 = y0;
    for (int i = 1; i < n; i++) {
        if (p[i].x < x0) x0 = p[i].x;
        if (p[i].x > x1) x1 = p[i].x;
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
    }
    int ix0 = (int)floorf(x0 - pad), iy0 = (int)floorf(y0 - pad);
    int ix1 = (int)ceilf(x1 + pad), iy1 = (int)ceilf(y1 + pad);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > cw) ix1 = cw;
    if (iy1 > ch) iy1 = ch;
    if (ix1 <= ix0 || iy1 <= iy0) return 0;
    m->a = calloc((size_t)(ix1 - ix0) * (iy1 - iy0), 1);
    if (!m->a) return -ENOMEM;
    m->x = ix0; m->y = iy0; m->w = ix1 - ix0; m->h = iy1 - iy0;
    return 0;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

#define SUBROWS 4

int upix_mask_fill(struct upix_mask *m, const struct upix_pt *p, int n, int cw, int ch, int aa) {
    int rc = mask_alloc(m, p, n, 1, cw, ch);
    if (rc || !m->a || n < 3) return rc;
    float *xs = malloc(sizeof *xs * (size_t)n);
    int *acc = malloc(sizeof *acc * (size_t)(m->w + 1));
    if (!xs || !acc) { free(xs); free(acc); upix_mask_free(m); return -ENOMEM; }
    int subs = aa ? SUBROWS : 1;
    for (int row = 0; row < m->h; row++) {
        memset(acc, 0, sizeof *acc * (size_t)(m->w + 1));
        for (int s = 0; s < subs; s++) {
            // A sub-row's centre, and where every edge crosses it.
            float sy = (float)(m->y + row) + (aa ? (s + 0.5f) / SUBROWS : 0.5f);
            int k = 0;
            for (int i = 0; i < n; i++) {
                struct upix_pt a = p[i], b = p[(i + 1) % n];
                if ((a.y <= sy) == (b.y <= sy)) continue;   // half-open: a vertex counts once
                xs[k++] = a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y);
            }
            qsort(xs, (size_t)k, sizeof *xs, cmp_float);
            for (int i = 0; i + 1 < k; i += 2) {
                float l = xs[i] - (float)m->x, r = xs[i + 1] - (float)m->x;
                if (!aa) {   // a pixel is in when its centre is
                    int a = (int)ceilf(l - 0.5f), b = (int)ceilf(r - 0.5f);
                    if (a < 0) a = 0;
                    if (b > m->w) b = m->w;
                    for (int x = a; x < b; x++) acc[x] = 255;
                    continue;
                }
                if (l < 0) l = 0;
                if (r > (float)m->w) r = (float)m->w;
                if (r <= l) continue;
                // EXACT ENDS: a partly covered first and last pixel get
                // their share; the run between them all of a sub-row.
                int a = (int)l, b = (int)r;
                int unit = 255 * 64 / SUBROWS;   // one sub-row's worth, in 1/64ths
                if (a == b) { acc[a] += (int)((r - l) * (float)unit); continue; }
                acc[a] += (int)(((float)(a + 1) - l) * (float)unit);
                for (int x = a + 1; x < b; x++) acc[x] += unit;
                if (b < m->w) acc[b] += (int)((r - (float)b) * (float)unit);
            }
        }
        uint8_t *dst = m->a + (size_t)row * m->w;
        for (int x = 0; x < m->w; x++) {
            int v = aa ? (acc[x] + 32) / 64 : acc[x];
            dst[x] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    free(xs);
    free(acc);
    return 0;
}

// The distance from (px, py) to the segment a-b.
static float seg_dist(float px, float py, struct upix_pt a, struct upix_pt b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((px - a.x) * dx + (py - a.y) * dy) / l2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float ex = a.x + t * dx - px, ey = a.y + t * dy - py;
    return sqrtf(ex * ex + ey * ey);
}

int upix_mask_stroke(struct upix_mask *m, const struct upix_pt *p, int n, int closed, float width,
                     int cw, int ch, int aa) {
    float hw = width / 2;
    int rc = mask_alloc(m, p, n, hw + 1, cw, ch);
    if (rc || !m->a) return rc;
    int segs = n == 1 ? 1 : closed ? n : n - 1;
    // SEGMENT BY SEGMENT over each one's own box, keeping the most any
    // segment covers -- so a join is round and nothing is counted twice.
    for (int i = 0; i < segs; i++) {
        struct upix_pt a = p[i], b = p[n == 1 ? 0 : (i + 1) % n];
        int x0 = (int)floorf((a.x < b.x ? a.x : b.x) - hw - 1) - m->x;
        int x1 = (int)ceilf((a.x > b.x ? a.x : b.x) + hw + 1) - m->x;
        int y0 = (int)floorf((a.y < b.y ? a.y : b.y) - hw - 1) - m->y;
        int y1 = (int)ceilf((a.y > b.y ? a.y : b.y) + hw + 1) - m->y;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > m->w) x1 = m->w;
        if (y1 > m->h) y1 = m->h;
        for (int y = y0; y < y1; y++) {
            uint8_t *row = m->a + (size_t)y * m->w;
            for (int x = x0; x < x1; x++) {
                float d = seg_dist((float)(m->x + x) + 0.5f, (float)(m->y + y) + 0.5f, a, b);
                // A pixel's coverage, near enough: how much of a unit
                // square centred on it the edge at distance d leaves in.
                float cov = aa ? hw + 0.5f - d : (d <= hw ? 1.0f : 0.0f);
                if (cov <= 0) continue;
                if (aa && hw < 0.5f) cov *= 2 * hw;   // a hairline is fainter, not wider
                int v = cov >= 1 ? 255 : (int)(cov * 255 + 0.5f);
                if (v > row[x]) row[x] = (uint8_t)v;
            }
        }
    }
    return 0;
}

void upix_mask_paint(struct uimg *im, const struct upix_mask *m, uint32_t colour, int exact,
                     struct upix_rect *dirty) {
    if (!im->px || !m->a) return;
    for (int y = 0; y < m->h; y++) {
        int iy = m->y + y;
        if (iy < 0 || iy >= im->h) continue;
        const uint8_t *src = m->a + (size_t)y * m->w;
        uint32_t *row = im->px + (size_t)iy * im->w;
        for (int x = 0; x < m->w; x++) {
            int ix = m->x + x;
            if (!src[x] || ix < 0 || ix >= im->w) continue;
            if (exact) { row[ix] = colour; continue; }
            unsigned a = ((colour >> 24) * src[x] + 127) / 255;
            row[ix] = over_px(row[ix], (colour & 0xFFFFFF) | a << 24, 255);
        }
    }
    upix_rect_add(dirty, im, m->x, m->y, m->w, m->h);
}

int upix_bezier(struct upix_pt p0, struct upix_pt p1, struct upix_pt p2, struct upix_pt p3,
                struct upix_pt *out, int n) {
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n, u = 1 - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        out[i] = (struct upix_pt){ a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y };
    }
    return n + 1;
}

static int ellipse_pts(float cx, float cy, float rx, float ry, float a0, float a1, int steps,
                       struct upix_pt *out, int at, int max) {
    for (int i = 0; i <= steps && at < max; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)steps;
        out[at++] = (struct upix_pt){ cx + rx * (float)cos(a), cy + ry * (float)sin(a) };
    }
    return at;
}

int upix_figure(enum upix_figure f, float x0, float y0, float x1, float y1, struct upix_pt *out, int max) {
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
    float w = x1 - x0, h = y1 - y0, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const float PI = 3.14159265f;
    int n = 0;
#define PT(X, Y) do { if (n < max) out[n++] = (struct upix_pt){ (X), (Y) }; } while (0)
    switch (f) {
    case UPIX_FIG_RECT:
        PT(x0, y0); PT(x1, y0); PT(x1, y1); PT(x0, y1);
        break;
    case UPIX_FIG_ROUNDED: {
        // Windows 11 Paint's: a corner a fifth of the shorter side.
        float r = (w < h ? w : h) / 5;
        n = ellipse_pts(x1 - r, y0 + r, r, r, -PI / 2, 0, 8, out, n, max);
        n = ellipse_pts(x1 - r, y1 - r, r, r, 0, PI / 2, 8, out, n, max);
        n = ellipse_pts(x0 + r, y1 - r, r, r, PI / 2, PI, 8, out, n, max);
        n = ellipse_pts(x0 + r, y0 + r, r, r, PI, 3 * PI / 2, 8, out, n, max);
        break;
    }
    case UPIX_FIG_ELLIPSE: {
        // Enough sides that none is longer than a couple of pixels.
        int steps = (int)((w + h) * 0.8f) + 16;
        if (steps > max - 1) steps = max - 1;
        n = ellipse_pts(cx, cy, w / 2, h / 2, 0, 2 * PI, steps, out, n, max) - 1;   // the last = the first
        break;
    }
    case UPIX_FIG_TRIANGLE:
        PT(cx, y0); PT(x1, y1); PT(x0, y1);
        break;
    case UPIX_FIG_ARROW: {
        // Pointing right: a shaft half the height, a head a third of the length.
        float hx = x1 - w / 3, t = h / 4;
        PT(x0, cy - t); PT(hx, cy - t); PT(hx, y0); PT(x1, cy); PT(hx, y1); PT(hx, cy + t); PT(x0, cy + t);
        break;
    }
    case UPIX_FIG_STAR:
        // Five points, the inner radius 0.38 of the outer, the top point up.
        for (int i = 0; i < 10; i++) {
            float a = -PI / 2 + (float)i * PI / 5, k = i % 2 ? 0.38f : 1.0f;
            PT(cx + w / 2 * k * (float)cos(a), cy + h / 2 * k * (float)sin(a));
        }
        break;
    }
#undef PT
    return n;
}

// --- brushes ----------------------------------------------------------------

int upix_stroke_begin(struct upix_stroke *s, struct uimg *im, uint32_t colour, enum upix_blend blend) {
    memset(s, 0, sizeof *s);
    if (!im->px) return -EINVAL;
    size_t n = (size_t)im->w * im->h;
    s->cov = calloc(n, 1);
    s->before = malloc(n * 4);
    if (!s->cov || !s->before) { upix_stroke_end(s); return -ENOMEM; }
    s->im = im;
    s->colour = colour;
    s->blend = blend;
    return 0;
}

void upix_stroke_end(struct upix_stroke *s) {
    free(s->cov);
    free(s->before);
    memset(s, 0, sizeof *s);
}

// A pixel that held `d` before the stroke, at coverage `c`.
static uint32_t blend_px(const struct upix_stroke *s, uint32_t d, unsigned c) {
    uint32_t col = s->colour;
    switch (s->blend) {
    case UPIX_BLEND_ERASE: {
        unsigned a = ((d >> 24) * (255 - c) + 127) / 255;
        return a ? (d & 0xFFFFFF) | a << 24 : 0;
    }
    case UPIX_BLEND_MULTIPLY: {
        // What is there, darkened by the colour; WHERE NOTHING IS THERE
        // (an empty layer) there is nothing to darken, so the uncovered
        // part takes the colour at half strength beneath it.
        uint32_t m = d & 0xFF000000u;
        for (int sh = 0; sh <= 16; sh += 8) {
            unsigned cd = (d >> sh) & 0xFF, cm = (cd * ((col >> sh) & 0xFF) + 127) / 255;
            m |= (cd - ((cd - cm) * c + 127) / 255) << sh;
        }
        uint32_t under = (col & 0xFFFFFF) | ((c * 128 + 127) / 255) << 24;
        return over_px(under, m, 255);
    }
    default: {
        unsigned a = ((col >> 24) * c + 127) / 255;
        return over_px(d, (col & 0xFFFFFF) | a << 24, 255);
    }
    }
}

// Coverage at pixel `i` raised to `c`, and the pixel redone if it rose.
static void cover(struct upix_stroke *s, size_t i, unsigned c) {
    unsigned was = s->cov[i];
    if (c <= was) return;
    if (!was) s->before[i] = s->im->px[i];
    s->cov[i] = (uint8_t)c;
    s->im->px[i] = blend_px(s, s->before[i], c);
}

void upix_stroke_round(struct upix_stroke *s, float x0, float y0, float x1, float y1, float size,
                       int hardness, struct upix_rect *dirty) {
    if (!s->cov || size <= 0) return;
    struct uimg *im = s->im;
    if (hardness < 0) hardness = 0;
    if (hardness > 100) hardness = 100;
    float r = size / 2, soft = r * (float)(100 - hardness) / 100;
    if (soft < 1) soft = 1;
    float inner = r - soft;
    int bx0 = (int)floorf((x0 < x1 ? x0 : x1) - r), by0 = (int)floorf((y0 < y1 ? y0 : y1) - r);
    int bx1 = (int)ceilf((x0 > x1 ? x0 : x1) + r), by1 = (int)ceilf((y0 > y1 ? y0 : y1) + r);
    struct upix_rect box = { 0, 0, 0, 0 };
    upix_rect_add(&box, im, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
    struct upix_pt a = { x0, y0 }, b = { x1, y1 };
    for (int y = box.y; y < box.y + box.h; y++) {
        for (int x = box.x; x < box.x + box.w; x++) {
            float d = seg_dist((float)x + 0.5f, (float)y + 0.5f, a, b);
            if (d >= r) continue;
            unsigned c = 255;
            if (d > inner) {
                float t = (d - inner) / soft;   // 0 at the full part's edge, 1 at the rim
                c = (unsigned)((1 - t * t * (3 - 2 * t)) * 255 + 0.5f);
            }
            if (c) cover(s, (size_t)y * im->w + x, c);
        }
    }
    upix_rect_add(dirty, im, box.x, box.y, box.w, box.h);
}

void upix_stroke_chisel(struct upix_stroke *s, int x0, int y0, int x1, int y1, int w, int h,
                        struct upix_rect *dirty) {
    if (!s->cov || w < 1 || h < 1) return;
    struct uimg *im = s->im;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        struct upix_rect tip = { 0, 0, 0, 0 };
        upix_rect_add(&tip, im, x0 - (w - 1) / 2, y0 - (h - 1) / 2, w, h);
        for (int y = tip.y; y < tip.y + tip.h; y++)
            for (int x = tip.x; x < tip.x + tip.w; x++) cover(s, (size_t)y * im->w + x, 255);
        upix_rect_add(dirty, im, tip.x, tip.y, tip.w, tip.h);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static uint32_t xorshift(uint32_t *s) {
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

void upix_spray(struct uimg *im, int cx, int cy, int size, int n, uint32_t colour, uint32_t *seed,
                struct upix_rect *dirty) {
    if (!im->px || size < 1) return;
    // One of the pixels upix_stamp's disc would cover, each as likely:
    // a cell of the size x size box, in its doubled coordinates.
    int x0 = cx - (size - 1) / 2, y0 = cy - (size - 1) / 2, r2 = size * size;
    while (n-- > 0) {
        int i, j, dx, dy;
        do {
            i = (int)(xorshift(seed) % (unsigned)size);
            j = (int)(xorshift(seed) % (unsigned)size);
            dx = 2 * i - (size - 1);
            dy = 2 * j - (size - 1);
        } while (size > 2 && dx * dx + dy * dy > r2);
        int x = x0 + i, y = y0 + j;
        if (x < 0 || y < 0 || x >= im->w || y >= im->h) continue;
        im->px[(size_t)y * im->w + x] = colour;
        upix_rect_add(dirty, im, x, y, 1, 1);
    }
}

// --- gradients --------------------------------------------------------------

#define GRAD_STEPS 4096   // the table's positions: 16x finer than 8 bits, for the dither

float upix_gradient_at(const struct upix_gradient *g, float px, float py) {
    float dx = g->x1 - g->x0, dy = g->y1 - g->y0, l2 = dx * dx + dy * dy;
    if (l2 <= 0) return 1;
    float vx = px - g->x0, vy = py - g->y0, t;
    switch (g->shape) {
    case UPIX_GRAD_REFLECTED: t = fabsf((vx * dx + vy * dy) / l2); break;
    case UPIX_GRAD_RADIAL:    t = sqrtf((vx * vx + vy * vy) / l2); break;
    case UPIX_GRAD_DIAMOND:   t = (fabsf(vx * dx + vy * dy) + fabsf(vx * -dy + vy * dx)) / l2; break;
    case UPIX_GRAD_CONICAL: {
        // The angle between (vx, vy) and the line, over pi.
        float a = (float)atan2(vx * -dy + vy * dx, vx * dx + vy * dy);
        t = fabsf(a) / 3.14159265f;
        break;
    }
    default: t = (vx * dx + vy * dy) / l2; break;
    }
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

void upix_gradient_fill(struct uimg *im, const struct upix_gradient *g, const uint8_t *mask,
                        const struct upix_rect *r, struct upix_rect *dirty) {
    if (!im->px || !r) return;
    struct upix_rect c = { 0, 0, 0, 0 };
    upix_rect_add(&c, im, r->x, r->y, r->w, r->h);
    if (c.w <= 0 || c.h <= 0) return;
    // A TABLE of the colour at each of GRAD_STEPS positions, each channel
    // in 8.8 fixed point, so a pixel costs its position and a lookup.
    static uint16_t lut[GRAD_STEPS][4];
    for (int i = 0; i < GRAD_STEPS; i++) {
        float t = (float)i / (GRAD_STEPS - 1);
        for (int ch = 0; ch < 4; ch++) {
            float a = (float)((g->c0 >> (8 * ch)) & 0xFF), b = (float)((g->c1 >> (8 * ch)) & 0xFF);
            lut[i][ch] = (uint16_t)((a + (b - a) * t) * 256.0f + 0.5f);
        }
    }
    int linear = g->shape == UPIX_GRAD_LINEAR || g->shape == UPIX_GRAD_REFLECTED;
    float dx = g->x1 - g->x0, dy = g->y1 - g->y0, l2 = dx * dx + dy * dy;
    for (int y = c.y; y < c.y + c.h; y++) {
        uint32_t *row = im->px + (size_t)y * im->w;
        const uint8_t *m = mask ? mask + (size_t)y * im->w : 0;
        // A LINE'S POSITION IS LINEAR ALONG A ROW: one add a pixel.
        float t = 0, step = 0;
        if (linear && l2 > 0) {
            t = (((float)c.x + 0.5f - g->x0) * dx + ((float)y + 0.5f - g->y0) * dy) / l2;
            step = dx / l2;
        }
        for (int x = c.x; x < c.x + c.w; x++, t += step) {
            if (m && !m[x]) continue;
            float u;
            if (linear && l2 > 0) {
                u = g->shape == UPIX_GRAD_REFLECTED ? fabsf(t) : t;
                u = u < 0 ? 0 : u > 1 ? 1 : u;
            } else {
                u = upix_gradient_at(g, (float)x + 0.5f, (float)y + 0.5f);
            }
            const uint16_t *e = lut[(int)(u * (GRAD_STEPS - 1) + 0.5f)];
            // The dither: a threshold under one 8-bit step, a 4x4 Bayer.
            unsigned d = g->dither ? (unsigned)BAYER[y & 3][x & 3] * 16 + 8 : 128;
            uint32_t sp = 0;
            for (int ch = 0; ch < 4; ch++) {
                unsigned v = ((unsigned)e[ch] + d) >> 8;
                sp |= (v > 255 ? 255u : v) << (8 * ch);
            }
            row[x] = over_px(row[x], sp, 255);
        }
    }
    upix_rect_add(dirty, im, c.x, c.y, c.w, c.h);
}

// --- selections -------------------------------------------------------------

int upix_sel_init(struct upix_sel *s, int w, int h) {
    memset(s, 0, sizeof *s);
    s->a = calloc((size_t)w * h, 1);
    if (!s->a) return -ENOMEM;
    s->w = w;
    s->h = h;
    return 0;
}

void upix_sel_free(struct upix_sel *s) {
    free(s->a);
    memset(s, 0, sizeof *s);
}

// The selected pixels' bounds, found again after a change.
static void sel_bounds(struct upix_sel *s) {
    int x0 = s->w, y0 = s->h, x1 = -1, y1 = -1;
    for (int y = 0; y < s->h; y++) {
        const uint8_t *row = s->a + (size_t)y * s->w;
        int l = 0, r = s->w - 1;
        while (l < s->w && !row[l]) l++;
        if (l == s->w) continue;
        while (!row[r]) r--;
        if (l < x0) x0 = l;
        if (r > x1) x1 = r;
        if (y < y0) y0 = y;
        y1 = y;
    }
    s->box = x1 < 0 ? (struct upix_rect){ 0, 0, 0, 0 } : (struct upix_rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
}

int upix_sel_any(const struct upix_sel *s) { return s->a && s->box.w > 0; }

int upix_sel_at(const struct upix_sel *s, int x, int y) {
    return s->a && x >= 0 && y >= 0 && x < s->w && y < s->h && s->a[(size_t)y * s->w + x];
}

void upix_sel_clear(struct upix_sel *s) {
    if (!s->a) return;
    memset(s->a, 0, (size_t)s->w * s->h);
    s->box = (struct upix_rect){ 0, 0, 0, 0 };
}

void upix_sel_all(struct upix_sel *s) {
    if (!s->a) return;
    memset(s->a, 255, (size_t)s->w * s->h);
    s->box = (struct upix_rect){ 0, 0, s->w, s->h };
}

void upix_sel_invert(struct upix_sel *s) {
    if (!s->a) return;
    for (size_t i = 0; i < (size_t)s->w * s->h; i++) s->a[i] = (uint8_t)(255 - s->a[i]);
    sel_bounds(s);
}

// NEW starts from nothing; ADD sets what the shape covers, SUB clears it.
static uint8_t sel_value(enum upix_sel_op op) { return op == UPIX_SEL_SUB ? 0 : 255; }

void upix_sel_rect(struct upix_sel *s, int x0, int y0, int x1, int y1, enum upix_sel_op op) {
    if (!s->a) return;
    if (op == UPIX_SEL_NEW) memset(s->a, 0, (size_t)s->w * s->h);
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    struct upix_rect r = { 0, 0, 0, 0 };
    struct uimg dims = { .w = s->w, .h = s->h };
    upix_rect_add(&r, &dims, x0, y0, x1 - x0, y1 - y0);
    for (int y = r.y; y < r.y + r.h; y++) memset(s->a + (size_t)y * s->w + r.x, sel_value(op), (size_t)r.w);
    sel_bounds(s);
}

int upix_sel_poly(struct upix_sel *s, const struct upix_pt *p, int n, enum upix_sel_op op) {
    if (!s->a) return 0;
    struct upix_mask m;
    if (upix_mask_fill(&m, p, n, s->w, s->h, 0) != 0) return -ENOMEM;
    if (op == UPIX_SEL_NEW) memset(s->a, 0, (size_t)s->w * s->h);
    uint8_t v = sel_value(op);
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++)
            if (m.a[(size_t)y * m.w + x]) s->a[(size_t)(m.y + y) * s->w + m.x + x] = v;
    upix_mask_free(&m);
    sel_bounds(s);
    return 0;
}

struct wand_ctx { struct upix_sel *s; uint8_t v; };

static void wand_run(void *p, int l, int r, int y) {
    struct wand_ctx *w = p;
    memset(w->s->a + (size_t)y * w->s->w + l, w->v, (size_t)(r - l + 1));
}

int upix_sel_wand(struct upix_sel *s, const struct uimg *im, int x, int y, int tolerance, enum upix_sel_op op) {
    if (!s->a || !im->px || im->w != s->w || im->h != s->h) return 0;
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return 0;
    if (op == UPIX_SEL_NEW) memset(s->a, 0, (size_t)s->w * s->h);
    struct wand_ctx w = { s, sel_value(op) };
    int n = flood(im, x, y, tolerance, wand_run, &w);
    sel_bounds(s);
    return n;
}

int upix_sel_copy(const struct upix_sel *s, const struct uimg *im, struct uimg *out) {
    memset(out, 0, sizeof *out);
    if (!upix_sel_any(s) || !im->px || im->w != s->w || im->h != s->h) return -EINVAL;
    const struct upix_rect *b = &s->box;
    out->px = malloc((size_t)b->w * b->h * 4);
    if (!out->px) return -ENOMEM;
    out->w = b->w;
    out->h = b->h;
    out->has_alpha = 1;
    for (int y = 0; y < b->h; y++) {
        const uint32_t *src = im->px + (size_t)(b->y + y) * im->w + b->x;
        const uint8_t *m = s->a + (size_t)(b->y + y) * s->w + b->x;
        uint32_t *dst = out->px + (size_t)y * b->w;
        for (int x = 0; x < b->w; x++) dst[x] = m[x] ? src[x] : 0;
    }
    return 0;
}

void upix_sel_paint(const struct upix_sel *s, struct uimg *im, uint32_t colour, struct upix_rect *dirty) {
    if (!upix_sel_any(s) || !im->px || im->w != s->w || im->h != s->h) return;
    const struct upix_rect *b = &s->box;
    for (int y = b->y; y < b->y + b->h; y++) {
        uint32_t *row = im->px + (size_t)y * im->w;
        const uint8_t *m = s->a + (size_t)y * s->w;
        for (int x = b->x; x < b->x + b->w; x++) if (m[x]) row[x] = colour;
    }
    upix_rect_add(dirty, im, b->x, b->y, b->w, b->h);
}

void upix_sel_keep_outside(const struct upix_sel *s, struct uimg *im, const struct uimg *before,
                           const struct upix_rect *r) {
    if (!upix_sel_any(s) || !im->px || !before->px || im->w != s->w || im->h != s->h ||
        before->w != im->w || before->h != im->h || !r || r->w <= 0 || r->h <= 0)
        return;
    struct upix_rect c = { 0, 0, 0, 0 };
    upix_rect_add(&c, im, r->x, r->y, r->w, r->h);
    for (int y = c.y; y < c.y + c.h; y++) {
        uint32_t *row = im->px + (size_t)y * im->w;
        const uint32_t *was = before->px + (size_t)y * im->w;
        const uint8_t *m = s->a + (size_t)y * s->w;
        for (int x = c.x; x < c.x + c.w; x++) if (!m[x]) row[x] = was[x];
    }
}
