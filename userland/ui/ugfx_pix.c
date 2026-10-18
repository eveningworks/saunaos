// ugfx_pix's C table -- the REFERENCE every SIMD tier must match byte for
// byte -- and the choice of tier (ugfx_pix.h).
//
// SSE2 is architectural on x86-64, so it needs no CPUID test; a tier
// that does (AVX2, needing the kernel's XSAVE as well) adds one here.
#include "ui/ugfx_pix.h"
#include "ui/ugfx.h"
#include <stdlib.h>
#include <string.h>

// --- the blur ----------------------------------------------------------

static inline uint32_t avg(uint32_t sr, uint32_t sg, uint32_t sb, uint32_t inv) {
    uint32_t r = (sr * inv + 32768) >> 16, g = (sg * inv + 32768) >> 16, b = (sb * inv + 32768) >> 16;
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static void box_h(const uint32_t *src, uint32_t *dst, int n, int r) {
    uint32_t d = (uint32_t)(2 * r + 1), inv = (65536u + d / 2) / d;
    uint32_t sr = 0, sg = 0, sb = 0;
    for (int k = -r; k <= r; k++) {
        uint32_t p = src[k < 0 ? 0 : (k >= n ? n - 1 : k)];
        sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
    }
    for (int k = 0; k < n; k++) {
        dst[k] = avg(sr, sg, sb, inv);
        int in = k + r + 1, out = k - r;
        uint32_t pi = src[in >= n ? n - 1 : in], po = src[out < 0 ? 0 : out];
        sr += ((pi >> 16) & 0xFF) - ((po >> 16) & 0xFF);
        sg += ((pi >> 8) & 0xFF) - ((po >> 8) & 0xFF);
        sb += (pi & 0xFF) - (po & 0xFF);
    }
}

// A row at a time, a running sum per column: a column-at-a-time walk
// misses the cache on every pixel.
static void box_v(const uint32_t *src, uint32_t *dst, int n, int m, int r, uint32_t *scratch) {
    uint32_t d = (uint32_t)(2 * r + 1), inv = (65536u + d / 2) / d;
    uint32_t *sr = scratch, *sg = scratch + n, *sb = scratch + 2 * n;
    for (int c = 0; c < n; c++) sr[c] = sg[c] = sb[c] = 0;
    for (int k = -r; k <= r; k++) {
        const uint32_t *row = src + (size_t)(k < 0 ? 0 : (k >= m ? m - 1 : k)) * n;
        for (int c = 0; c < n; c++) {
            uint32_t p = row[c];
            sr[c] += (p >> 16) & 0xFF; sg[c] += (p >> 8) & 0xFF; sb[c] += p & 0xFF;
        }
    }
    for (int j = 0; j < m; j++) {
        uint32_t *drow = dst + (size_t)j * n;
        int in = j + r + 1, out = j - r;
        const uint32_t *ri = src + (size_t)(in >= m ? m - 1 : in) * n;
        const uint32_t *ro = src + (size_t)(out < 0 ? 0 : out) * n;
        for (int c = 0; c < n; c++) {
            drow[c] = avg(sr[c], sg[c], sb[c], inv);
            uint32_t pi = ri[c], po = ro[c];
            sr[c] += ((pi >> 16) & 0xFF) - ((po >> 16) & 0xFF);
            sg[c] += ((pi >> 8) & 0xFF) - ((po >> 8) & 0xFF);
            sb[c] += (pi & 0xFF) - (po & 0xFF);
        }
    }
}

static void down2(const uint32_t *r0, const uint32_t *r1, uint32_t *dst, int n) {
    for (int k = 0; k < (n + 1) / 2; k++) {
        int i0 = 2 * k, i1 = i0 + 1 < n ? i0 + 1 : i0;
        uint32_t q[4] = { r0[i0], r0[i1], r1[i0], r1[i1] }, o = 0;
        for (int sh = 0; sh <= 16; sh += 8)
            o |= ((((q[0] >> sh) & 0xFF) + ((q[1] >> sh) & 0xFF) +
                   ((q[2] >> sh) & 0xFF) + ((q[3] >> sh) & 0xFF) + 2) >> 2) << sh;
        dst[k] = o;
    }
}

// Bilinear, between four 0xRRGGBB pixels, weights in 1/256.
static inline uint32_t lerp4(uint32_t p00, uint32_t p10, uint32_t p01, uint32_t p11,
                             unsigned fx, unsigned fy) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        unsigned a = (p00 >> sh) & 0xFF, b = (p10 >> sh) & 0xFF;
        unsigned c = (p01 >> sh) & 0xFF, e = (p11 >> sh) & 0xFF;
        unsigned top = a * (256 - fx) + b * fx, bot = c * (256 - fx) + e * fx;
        out |= (((top * (256 - fy) + bot * fy) >> 16) & 0xFF) << sh;
    }
    return out;
}

// Sample centres: full-size pixel i sits at (i + 0.5) / 2 - 0.5.
static void up2(const uint32_t *a, const uint32_t *b, uint32_t *dst, int n, int dn, unsigned fy) {
    for (int i = 0; i < n; i++) {
        int vx = i * 128 - 64, rx = vx < 0 ? 0 : vx >> 8;
        unsigned fx = vx < 0 ? 0 : (unsigned)(vx & 255);
        int rx1 = rx + 1 < dn ? rx + 1 : rx;
        dst[i] = lerp4(a[rx], a[rx1], b[rx], b[rx1], fx, fy);
    }
}

// --- mixing and blending ------------------------------------------------

static inline uint32_t mix(uint32_t under, uint32_t over, unsigned a) {
    unsigned r = (((under >> 16) & 0xFF) * (255 - a) + ((over >> 16) & 0xFF) * a + 127) / 255;
    unsigned g = (((under >> 8) & 0xFF) * (255 - a) + ((over >> 8) & 0xFF) * a + 127) / 255;
    unsigned b = ((under & 0xFF) * (255 - a) + (over & 0xFF) * a + 127) / 255;
    return r << 16 | g << 8 | b;
}

static void mix_const(uint32_t *dst, const uint32_t *under, uint32_t over, unsigned a, int n) {
    for (int i = 0; i < n; i++) dst[i] = mix(under[i], over, a);
}

static void mix_rows(uint32_t *dst, const uint32_t *under, const uint32_t *over, unsigned a,
                     int n) {
    for (int i = 0; i < n; i++) dst[i] = mix(under[i], over[i], a);
}

static void glass(uint32_t *dst, const uint32_t *src, const uint32_t *under, unsigned a, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t p = src[i];
        dst[i] = p >> 24 ? mix(under[i], p, a) : p;
    }
}

static inline uint32_t blend(uint32_t fg, uint32_t bg, unsigned alpha) {
    unsigned fr = (fg >> 16) & 0xFF, fg_ = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    unsigned br = (bg >> 16) & 0xFF, bg_ = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    unsigned r = (fr * alpha + br * (255 - alpha)) / 255;
    unsigned g = (fg_ * alpha + bg_ * (255 - alpha)) / 255;
    unsigned b = (fb * alpha + bb * (255 - alpha)) / 255;
    return (r << 16) | (g << 8) | b;
}

static void blend_const(uint32_t *dst, uint32_t c, unsigned a, int n) {
    for (int i = 0; i < n; i++) dst[i] = blend(c, dst[i], a);
}

static void blend_cov(uint32_t *dst, uint32_t c, const uint8_t *cov, int n) {
    for (int i = 0; i < n; i++) {
        unsigned al = cov[i];
        if (al) dst[i] = al >= 255 ? c : blend(c, dst[i], al);
    }
}

const struct ugfx_pix_ops ugfx_pix_c = {
    .name = "c",
    .box_h = box_h, .box_v = box_v, .down2 = down2, .up2 = up2,
    .mix_const = mix_const, .mix_rows = mix_rows, .glass = glass,
    .blend_const = blend_const, .blend_cov = blend_cov,
};

// --- the choice ----------------------------------------------------------

static const struct ugfx_pix_ops *g_pix;

static const struct ugfx_pix_ops *tier(const char *name) {
    if (!name || !strcmp(name, "auto") || !strcmp(name, "on")) return &ugfx_pix_sse2;
    if (!strcmp(name, "off") || !strcmp(name, "c")) return &ugfx_pix_c;
    if (!strcmp(name, "sse2")) return &ugfx_pix_sse2;
    return 0;
}

const struct ugfx_pix_ops *ugfx_pix(void) {
    if (!g_pix) {
        g_pix = tier(getenv("UGFX_SIMD"));
        if (!g_pix) g_pix = tier(0);   // a word we do not know: the default
    }
    return g_pix;
}

int ugfx_simd_set(const char *name) {
    const struct ugfx_pix_ops *t = tier(name);
    if (!t) return -1;
    g_pix = t;
    return 0;
}

const char *ugfx_simd(void) { return ugfx_pix()->name; }
