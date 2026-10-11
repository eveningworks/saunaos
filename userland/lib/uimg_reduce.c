// A box reducer fed by rows -- see uimg_reduce.h.
#include "lib/uimg_reduce.h"

#include <kerrno.h>
#include <stdlib.h>
#include <string.h>

#define ACC_N 5   // r, g, b, a, count
// A block's alpha-weighted sum is up to 255 * 255 per pixel, so 64 x 64
// blocks keep it inside 32 bits; a bigger shrink leaves the rest to the
// precise resampler.
#define K_MAX 64

void uimg_reduce_factors(int sw, int sh, int dw, int dh, int *kx, int *ky) {
    *kx = dw > 0 && sw >= 4 * dw ? sw / (2 * dw) : 1;
    *ky = dh > 0 && sh >= 4 * dh ? sh / (2 * dh) : 1;
    if (*kx > K_MAX) *kx = K_MAX;
    if (*ky > K_MAX) *ky = K_MAX;
}

int uimg_reduce_init(struct uimg_reduce *r, int sw, int sh, int kx, int ky, int alpha) {
    memset(r, 0, sizeof *r);
    if (sw <= 0 || sh <= 0 || kx <= 0 || ky <= 0 || kx > K_MAX || ky > K_MAX) return -EINVAL;
    r->sw = sw; r->sh = sh; r->kx = kx; r->ky = ky;
    r->alpha = alpha;
    r->out.w = (sw + kx - 1) / kx;
    r->out.h = (sh + ky - 1) / ky;
    r->out.has_alpha = alpha;
    r->out.px = malloc((size_t)r->out.w * r->out.h * sizeof *r->out.px);
    r->acc = calloc((size_t)r->out.w * ACC_N, sizeof *r->acc);
    if (!r->out.px || !r->acc) {
        uimg_reduce_abort(r);
        return -ENOMEM;
    }
    return 0;
}

// The band of rows in `acc` becomes output row `oy`, and `acc` is cleared.
static void emit(struct uimg_reduce *r, int oy) {
    uint32_t *dst = r->out.px + (size_t)oy * r->out.w;
    for (int ox = 0; ox < r->out.w; ox++) {
        uint32_t *a = r->acc + (size_t)ox * ACC_N;
        uint32_t n = a[4];
        uint32_t px = 0;
        if (n) {
            uint32_t al = r->alpha ? a[3] / n : 255;
            uint32_t div = r->alpha ? a[3] : n;   // colour is weighted by alpha
            if (div) {
                // A full block divides by the same count every time:
                // one reciprocal, in 16.16, rounds to the same answer.
                uint32_t rr, gg, bb;
                if (!r->alpha && n == (uint32_t)(r->kx * r->ky) && n < 65536) {
                    uint32_t inv = (65536u + n / 2) / n;
                    // ROUNDED, not floored: a reciprocal that rounded
                    // down turned a flat 255 into 254 (uimg_test).
                    rr = (uint32_t)(((uint64_t)a[0] * inv + 32768) >> 16);
                    gg = (uint32_t)(((uint64_t)a[1] * inv + 32768) >> 16);
                    bb = (uint32_t)(((uint64_t)a[2] * inv + 32768) >> 16);
                } else {
                    rr = a[0] / div; gg = a[1] / div; bb = a[2] / div;
                }
                px = (al << 24) | ((rr > 255 ? 255 : rr) << 16) | ((gg > 255 ? 255 : gg) << 8) |
                     (bb > 255 ? 255 : bb);
            } else {
                px = 0;   // wholly transparent
            }
        }
        dst[ox] = px;
        a[0] = a[1] = a[2] = a[3] = a[4] = 0;
    }
}

void uimg_reduce_row(struct uimg_reduce *r, const uint32_t *row) {
    if (!r->acc || r->y >= r->sh) return;
    for (int ox = 0, x = 0; ox < r->out.w; ox++) {
        uint32_t *a = r->acc + (size_t)ox * ACC_N;
        int x0 = x, end = x + r->kx < r->sw ? x + r->kx : r->sw;
        uint32_t sr = 0, sg = 0, sb = 0, sa = 0;
        if (r->alpha) {
            for (; x < end; x++) {
                uint32_t p = row[x], al = p >> 24;
                sr += ((p >> 16) & 0xFF) * al;
                sg += ((p >> 8) & 0xFF) * al;
                sb += (p & 0xFF) * al;
                sa += al;
            }
        } else {
            for (; x < end; x++) {
                uint32_t p = row[x];
                sr += (p >> 16) & 0xFF;
                sg += (p >> 8) & 0xFF;
                sb += p & 0xFF;
            }
        }
        a[0] += sr; a[1] += sg; a[2] += sb; a[3] += sa;
        a[4] += (uint32_t)(end - x0);
    }
    r->y++;
    if (r->y % r->ky == 0 || r->y == r->sh) emit(r, (r->y - 1) / r->ky);
}

int uimg_reduce_finish(struct uimg_reduce *r, struct uimg *out) {
    int complete = r->y == r->sh;
    free(r->acc);
    r->acc = 0;
    if (!complete) {
        uimg_free(&r->out);
        memset(out, 0, sizeof *out);
        return -EINVAL;
    }
    *out = r->out;
    memset(&r->out, 0, sizeof r->out);
    return 0;
}

void uimg_reduce_abort(struct uimg_reduce *r) {
    free(r->acc);
    r->acc = 0;
    uimg_free(&r->out);
}
