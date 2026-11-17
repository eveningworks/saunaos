// Sharp bilinear picture scaling -- see upixscale.h.
#include "base/upixscale.h"
#include <stdlib.h>
#include <string.h>

// One axis: n source pixels shown as m. The picture is first enlarged by
// k (the whole factor at or above m/n) with nearest-neighbour, which
// changes nothing but where the blend boundaries fall; a bilinear sample
// of THAT at each destination pixel's centre falls inside one source
// pixel's run (weight 0) except within half an enlarged pixel of a
// boundary between two.
static void axis(int n, int m, int *a, int *b, uint16_t *w) {
    int64_t k = (m + n - 1) / n;
    if (k < 1) k = 1;
    for (int i = 0; i < m; i++) {
        // Centre of destination pixel i, in enlarged pixels, minus half a
        // pixel for the bilinear sample: ((2i+1) * n * k / (2m)) - 0.5,
        // in 16.16 fixed point.
        int64_t u = ((int64_t)(2 * i + 1) * n * k * 65536) / (2 * (int64_t)m) - 32768;
        if (u < 0) u = 0;
        int64_t e = u >> 16;                 // the enlarged pixel to the left
        int frac = (int)((u & 0xffff) >> 8); // 0..255 towards the next one
        int pa = (int)(e / k), pb = (int)((e + 1) / k);
        if (pa > n - 1) pa = n - 1;
        if (pb > n - 1) pb = n - 1;
        a[i] = pa;
        b[i] = pb;
        w[i] = pa == pb ? 0 : (uint16_t)(frac + (frac >> 7));   // 0..256
    }
}

void upixscale_free(struct upixscale *s) {
    free(s->xa);
    free(s->ya);
    memset(s, 0, sizeof *s);
}

int upixscale_prepare(struct upixscale *s, int sw, int sh, int dw, int dh) {
    if (s->xa && s->sw == sw && s->sh == sh && s->dw == dw && s->dh == dh) return 1;
    upixscale_free(s);
    if (sw < 1 || sh < 1 || dw < 1 || dh < 1) return 0;
    // One block per axis: two index arrays, then the weights.
    size_t xbytes = (size_t)dw * (2 * sizeof(int) + sizeof(uint16_t));
    size_t ybytes = (size_t)dh * (2 * sizeof(int) + sizeof(uint16_t));
    char *x = malloc(xbytes), *y = malloc(ybytes);
    if (!x || !y) {
        free(x);
        free(y);
        return 0;
    }
    s->xa = (int *)x;
    s->xb = s->xa + dw;
    s->xw = (uint16_t *)(s->xb + dw);
    s->ya = (int *)y;
    s->yb = s->ya + dh;
    s->yw = (uint16_t *)(s->yb + dh);
    s->sw = sw; s->sh = sh; s->dw = dw; s->dh = dh;
    axis(sw, dw, s->xa, s->xb, s->xw);
    axis(sh, dh, s->ya, s->yb, s->yw);
    return 1;
}

// c0 * (256 - w) + c1 * w, all four channels: two at a time in one word
// (0x00ff00ff lanes), so a lane's product (<= 0xff * 256) cannot carry
// into the next.
static inline uint32_t mix(uint32_t c0, uint32_t c1, unsigned w) {
    if (w == 0) return c0;
    unsigned v = 256 - w;
    uint32_t rb = (((c0 & 0x00ff00ffu) * v + (c1 & 0x00ff00ffu) * w) >> 8) & 0x00ff00ffu;
    uint32_t ag = (((c0 >> 8) & 0x00ff00ffu) * v + ((c1 >> 8) & 0x00ff00ffu) * w) & 0xff00ff00u;
    return rb | ag;
}

void upixscale_run(const struct upixscale *s, const uint32_t *src, int src_stride,
                   uint32_t *dst, int dst_stride) {
    if (!s->xa) return;
    for (int y = 0; y < s->dh; y++) {
        const uint32_t *ra = src + (size_t)s->ya[y] * src_stride;
        const uint32_t *rb = src + (size_t)s->yb[y] * src_stride;
        unsigned wy = s->yw[y];
        uint32_t *out = dst + (size_t)y * dst_stride;
        if (wy == 0) {
            for (int x = 0; x < s->dw; x++)
                out[x] = mix(ra[s->xa[x]], ra[s->xb[x]], s->xw[x]);
        } else {
            for (int x = 0; x < s->dw; x++) {
                uint32_t top = mix(ra[s->xa[x]], ra[s->xb[x]], s->xw[x]);
                uint32_t bot = mix(rb[s->xa[x]], rb[s->xb[x]], s->xw[x]);
                out[x] = mix(top, bot, wy);
            }
        }
    }
}
