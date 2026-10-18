// ugfx's glass: a box blur of the surface into a backdrop, the blit that
// shows a glass client's marked pixels over one, and a rect faded over
// one (ugfx.h).
//
// THE BLUR IS SEPARABLE AND RUNNING-SUM: three box passes along the rows,
// then three down the columns, each O(1) per pixel whatever the radius --
// the box approximation every software compositor uses rather than a true
// Gaussian kernel. The column passes walk ROWS, keeping a running sum per
// column, because a column-at-a-time walk misses the cache on every pixel.
// A wide blur runs at HALF RESOLUTION and is upsampled bilinearly (what
// KWin's dual-Kawase blur exploits): a blurred picture has no detail for
// the full-size pass to keep. Edges CLAMP to the rect, so nothing outside
// it is ever read. The per-row loops are ugfx_pix's, one table per CPU
// tier (ugfx_pix.h).
#include "ui/ugfx.h"
#include "ui/ugfx_pix.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 8192            // the longest row or column blurred
#define PASSES   3
#define HALF_AT  6               // radii from here blur at half resolution

static uint32_t g_a[MAX_LINE], g_b[MAX_LINE];   // main-thread only, as every ugfx call is
static uint32_t g_sum[4 * MAX_LINE];            // a box_v's scratch (ugfx_pix.h)
static uint32_t *g_w, *g_w2;                    // the working picture, two copies
static size_t g_cap;

static int grow(size_t n) {
    if (n <= g_cap) return 1;
    uint32_t *a = realloc(g_w, n * 4), *b = a ? realloc(g_w2, n * 4) : 0;
    if (a) g_w = a;
    if (!b) return 0;
    g_w2 = b;
    g_cap = n;
    return 1;
}

void ugfx_blur_rect(const struct ugfx_surface *s, int x, int y, int w, int h,
                    int radius, uint32_t *out) {
    if (!s || !s->pixels || !out || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (s->clip_active) {
        if (x0 < s->clip_x0) x0 = s->clip_x0;
        if (y0 < s->clip_y0) y0 = s->clip_y0;
        if (x1 > s->clip_x1) x1 = s->clip_x1;
        if (y1 > s->clip_y1) y1 = s->clip_y1;
    }
    int n = x1 - x0, m = y1 - y0;
    if (n <= 0 || m <= 0 || n > MAX_LINE || m > MAX_LINE) return;
    if (radius < 1) radius = 1;
    int f = radius >= HALF_AT && n > 1 && m > 1 ? 2 : 1;
    int r = radius / f;
    int dn = (n + f - 1) / f, dm = (m + f - 1) / f;
    if (!grow((size_t)dn * (size_t)dm)) return;
    size_t stride = (size_t)s->w;

    // In, a row at a time -- each f x f block averaged -- with the row
    // passes done while the row is hot.
    const struct ugfx_pix_ops *px = ugfx_pix();
    for (int j = 0; j < dm; j++) {
        const uint32_t *r0 = s->pixels + (size_t)(y0 + j * f) * stride + x0;
        const uint32_t *r1 = (f == 2 && y0 + j * f + 1 < y1) ? r0 + stride : r0;
        if (f == 2) px->down2(r0, r1, g_a, n);
        else memcpy(g_a, r0, (size_t)n * 4);
        uint32_t *src = g_a, *dst = g_b;
        for (int p = 0; p < PASSES; p++) {
            px->box_h(src, dst, dn, r);
            uint32_t *t = src; src = dst; dst = t;
        }
        memcpy(g_w + (size_t)j * dn, src, (size_t)dn * 4);
    }
    uint32_t *src = g_w, *dst = g_w2;
    for (int p = 0; p < PASSES; p++) {
        px->box_v(src, dst, dn, dm, r, g_sum);
        uint32_t *t = src; src = dst; dst = t;
    }

    // Out, at full size.
    for (int j = 0; j < m; j++) {
        uint32_t *orow = out + (size_t)(y0 + j) * stride + x0;
        if (f == 1) {
            memcpy(orow, src + (size_t)j * dn, (size_t)n * 4);
            continue;
        }
        // Sample centres: full-size row j sits at (j + 0.5) / 2 - 0.5.
        int vy = j * 128 - 64, ry = vy < 0 ? 0 : vy >> 8;
        unsigned fy = vy < 0 ? 0 : (unsigned)(vy & 255);
        int ry1 = ry + 1 < dm ? ry + 1 : ry;
        px->up2(src + (size_t)ry * dn, src + (size_t)ry1 * dn, orow, n, dn, fy);
    }
}

void ugfx_blit_glass(struct ugfx_surface *s, int x, int y, int w, int h,
                     const uint32_t *src, int src_pitch_px,
                     const uint32_t *backdrop, uint8_t alpha) {
    if (!s || !s->pixels || !src || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (s->clip_active) {
        if (x0 < s->clip_x0) x0 = s->clip_x0;
        if (y0 < s->clip_y0) y0 = s->clip_y0;
        if (x1 > s->clip_x1) x1 = s->clip_x1;
        if (y1 > s->clip_y1) y1 = s->clip_y1;
    }
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t stride = (uint32_t)s->w;
    const struct ugfx_pix_ops *px = ugfx_pix();
    for (int j = y0; j < y1; j++) {
        const uint32_t *srow = src + (uint32_t)(j - y) * (uint32_t)src_pitch_px + (uint32_t)(x0 - x);
        uint32_t *drow = s->pixels + (uint32_t)j * stride + (uint32_t)x0;
        const uint32_t *brow = backdrop ? backdrop + (uint32_t)j * stride + (uint32_t)x0 : drow;
        if (alpha >= 255)
            for (int i = 0; i < x1 - x0; i++) drow[i] = srow[i] & 0xFFFFFF;
        else
            px->glass(drow, srow, brow, alpha, x1 - x0);
    }
    ugfx_mark_dirty_rect(s, x0, y0, x1 - x0, y1 - y0);
}

void ugfx_fade_over(struct ugfx_surface *s, int x, int y, int w, int h,
                    const uint32_t *backdrop, uint8_t alpha) {
    if (!s || !s->pixels || !backdrop || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (s->clip_active) {
        if (x0 < s->clip_x0) x0 = s->clip_x0;
        if (y0 < s->clip_y0) y0 = s->clip_y0;
        if (x1 > s->clip_x1) x1 = s->clip_x1;
        if (y1 > s->clip_y1) y1 = s->clip_y1;
    }
    if (x1 <= x0 || y1 <= y0) return;
    size_t stride = (size_t)s->w;
    const struct ugfx_pix_ops *px = ugfx_pix();
    for (int j = y0; j < y1; j++) {
        uint32_t *row = s->pixels + (size_t)j * stride + x0;
        px->mix_rows(row, backdrop + (size_t)j * stride + x0, row, alpha, x1 - x0);
    }
    ugfx_mark_dirty_rect(s, x0, y0, x1 - x0, y1 - y0);
}
