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
#include <string.h>
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

// --- a window kept scaled between frames ------------------------------------------

// Screen pixels [X0, X1) x [Y0, Y1) of `sc->out` from `cur`, by the taps
// already made for this size.
static void scale_part(struct w16_scaler *sc, const uint32_t *cur, int X0, int Y0, int X1, int Y1) {
    int w = sc->w, W = sc->W;
    if (g_w16_q % 4 == 0) {                      // a whole step: every pixel a square, exactly
        int k = g_w16_q / 4;
        for (int Y = Y0; Y < Y1; Y++) {
            const uint32_t *in = cur + (size_t)(Y / k) * w;
            uint32_t *o = sc->out + (size_t)Y * W;
            for (int X = X0; X < X1; X++) o[X] = in[X / k];
        }
        return;
    }
    for (int Y = Y0; Y < Y1; Y++) {
        const struct tap *ty = &sc->ty[Y];
        const uint32_t *ra = cur + (size_t)ty->a * w, *rb = cur + (size_t)ty->b * w;
        uint32_t *o = sc->out + (size_t)Y * W;
        for (int X = X0; X < X1; X++) {
            const struct tap *tx = &sc->tx[X];
            uint32_t top = mix(ra[tx->a], ra[tx->b], tx->w);
            uint32_t bot = ty->w ? mix(rb[tx->a], rb[tx->b], tx->w) : top;
            o[X] = mix(top, bot, ty->w);
        }
    }
}

// The rectangle of `cur` that differs from `prev`, both w x h; 0 when none.
static int changed(const uint32_t *cur, const uint32_t *prev, int w, int h, int *x0, int *y0, int *x1, int *y1) {
    *x0 = w; *y0 = h; *x1 = *y1 = 0;
    for (int y = 0; y < h; y++) {
        const uint32_t *a = cur + (size_t)y * w, *b = prev + (size_t)y * w;
        if (!memcmp(a, b, (size_t)w * 4)) continue;
        int l = 0, r = w;
        while (a[l] == b[l]) l++;
        while (a[r - 1] == b[r - 1]) r--;
        if (l < *x0) *x0 = l;
        if (r > *x1) *x1 = r;
        if (y < *y0) *y0 = y;
        *y1 = y + 1;
    }
    return *x1 > *x0;
}

void w16_scaler_show(struct w16_scaler *sc, struct ugfx_surface *dst, int dx, int dy, const uint32_t *cur,
                     int w, int h) {
    int W = w16_up(w), H = w16_up(h);
    if (W <= 0 || H <= 0) return;
    int fresh = sc->w != w || sc->h != h || sc->q != g_w16_q || sc->method != g_w16_method || !sc->out;
    if (fresh) {
        w16_scaler_free(sc);
        sc->prev = malloc((size_t)w * h * 4);
        sc->out = malloc((size_t)W * H * 4);
        sc->tx = malloc(sizeof *sc->tx * (size_t)W);
        sc->ty = malloc(sizeof *sc->ty * (size_t)H);
        if (!sc->prev || !sc->out || !sc->tx || !sc->ty) { w16_scaler_free(sc); return; }
        sc->w = w; sc->h = h; sc->W = W; sc->H = H; sc->q = g_w16_q; sc->method = g_w16_method;
        taps(sc->tx, w, W);
        taps(sc->ty, h, H);
    }
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (fresh || changed(cur, sc->prev, w, h, &x0, &y0, &x1, &y1)) {
        // Every screen pixel whose taps can reach the change: a blend reads
        // its neighbour, so a pixel past each side, scaled, and one more.
        int X0 = (x0 - 1) * g_w16_q / 4 - 1, X1 = (x1 + 1) * g_w16_q / 4 + 2;
        int Y0 = (y0 - 1) * g_w16_q / 4 - 1, Y1 = (y1 + 1) * g_w16_q / 4 + 2;
        scale_part(sc, cur, X0 < 0 ? 0 : X0, Y0 < 0 ? 0 : Y0, X1 > W ? W : X1, Y1 > H ? H : Y1);
        for (int y = y0; y < y1; y++)
            memcpy(sc->prev + (size_t)y * w + x0, cur + (size_t)y * w + x0, (size_t)(x1 - x0) * 4);
    }
    ugfx_blit(dst, dx, dy, W, H, sc->out, W);
}

void w16_scaler_free(struct w16_scaler *sc) {
    free(sc->prev);
    free(sc->out);
    free(sc->tx);
    free(sc->ty);
    memset(sc, 0, sizeof *sc);
}
