// scale16.c -- a Win16 program's pixels at the scale its settings ask for
// (w16prefs.h): its world is drawn at 1x and this puts it on the screen,
// at whole steps (2x, 3x) pixel for pixel and between them by the method
// chosen -- Pixels (nearest: hard, uneven), Smooth (bilinear: even, soft)
// or Sharp (sharp-bilinear: doubled pixel for pixel, then smoothed down,
// so only the seam between two source pixels is blended).
//
// THE SCALE IS IN QUARTERS (g_w16_q: 4 is 1x, 6 is 1.5x), so a size
// converts exactly both ways through w16_up() and w16_down() and nothing
// drifts by a rounding: input is mapped back by the same rule.
#include "w16_internal.h"

#include <stdlib.h>
#include "ui/ugfx.h"
#include "w16/w16prefs.h"

int g_w16_q = 4;
int g_w16_method = W16_METHOD_SHARP;

int w16_up(int v) { return (v * g_w16_q + 2) / 4; }
int w16_down(int v) { return v * 4 / g_w16_q; }

// For each of `dst` screen pixels along one axis: the two source pixels
// it blends and the weight of the second, 0..256.
struct tap { int a, b, w; };

static void taps(struct tap *t, int src, int dst) {
    int n = (g_w16_q + 3) / 4;                   // Sharp's whole pre-scale: 2 for 1.25-2x, 3 for 2.5-3x
    for (int i = 0; i < dst; i++) {
        double u = (i + 0.5) * src / dst;        // the pixel centre, in source pixels
        int a, b;
        double f = 0;
        if (g_w16_method == W16_METHOD_PIXELS) {
            a = b = (int)u;
        } else if (g_w16_method == W16_METHOD_SMOOTH) {
            double p = u - 0.5;
            a = p < 0 ? 0 : (int)p;
            f = p < 0 ? 0 : p - a;
            b = a + 1;
        } else {
            // Sharp: as if the source were first n times its size, pixel
            // for pixel, then smoothed down -- weight only where two
            // pre-scaled pixels from DIFFERENT source pixels meet.
            double p = u * n - 0.5;
            int k = p < 0 ? 0 : (int)p;
            f = p < 0 ? 0 : p - k;
            a = k / n;
            b = (k + 1) / n;
            if (a == b) f = 0;
        }
        if (a >= src) a = src - 1;
        if (b >= src) b = src - 1;
        t[i] = (struct tap){ a, b, (int)(f * 256 + 0.5) };
    }
}

static uint32_t mix(uint32_t p, uint32_t q, int w) {
    if (!w || p == q) return p;
    uint32_t rb = ((p & 0x00FF00FF) * (uint32_t)(256 - w) + (q & 0x00FF00FF) * (uint32_t)w) >> 8;
    uint32_t ag = (((p >> 8) & 0x00FF00FF) * (uint32_t)(256 - w) + ((q >> 8) & 0x00FF00FF) * (uint32_t)w) >> 8;
    return (rb & 0x00FF00FF) | ((ag & 0x00FF00FF) << 8);
}

// `src` (w x h, `stride` pixels a row) onto `dst` at (dx, dy), w16_up(w) x
// w16_up(h) of it.
void w16_scale_blit(struct ugfx_surface *dst, int dx, int dy, const uint32_t *src, int w, int h,
                    int stride) {
    int W = w16_up(w), H = w16_up(h);
    if (W <= 0 || H <= 0) return;
    if (g_w16_q % 4 == 0) {                      // a whole step: every pixel a square, exactly
        int s = g_w16_q / 4;
        uint32_t *row = malloc((size_t)W * 4);
        if (!row) return;
        for (int y = 0; y < h; y++) {
            const uint32_t *in = src + (size_t)y * stride;
            for (int x = 0; x < w; x++)
                for (int k = 0; k < s; k++) row[x * s + k] = in[x];
            for (int k = 0; k < s; k++) ugfx_blit(dst, dx, dy + y * s + k, W, 1, row, W);
        }
        free(row);
        return;
    }
    struct tap *tx = malloc(sizeof *tx * (size_t)W), *ty = malloc(sizeof *ty * (size_t)H);
    uint32_t *row = malloc((size_t)W * 4);
    if (tx && ty && row) {
        taps(tx, w, W);
        taps(ty, h, H);
        for (int y = 0; y < H; y++) {
            const uint32_t *ra = src + (size_t)ty[y].a * stride, *rb = src + (size_t)ty[y].b * stride;
            for (int x = 0; x < W; x++) {
                uint32_t top = mix(ra[tx[x].a], ra[tx[x].b], tx[x].w);
                uint32_t bot = ty[y].w ? mix(rb[tx[x].a], rb[tx[x].b], tx[x].w) : top;
                row[x] = mix(top, bot, ty[y].w);
            }
            ugfx_blit(dst, dx, dy + y, W, 1, row, W);
        }
    }
    free(tx);
    free(ty);
    free(row);
}
