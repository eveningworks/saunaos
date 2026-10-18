// ugfx_pix -- the pixel loops under ugfx's glass and blending, one table
// per CPU tier (C, SSE2), chosen ONCE (pixman's implementation chain).
//
// THE CONTRACT IS BYTE-IDENTICAL OUTPUT: every tier computes exactly what
// the C one does, rounding included, so switching tier changes the speed
// and nothing on screen -- tools/ugfx_pix_hostcheck.py holds each SIMD
// row against C on random inputs. A loop that cannot be exact for some
// input (a box sum past 16 bits) hands that call to the C table.
//
// Pixels are 0xXXRRGGBB; every op writes 0 into the top byte unless it
// says otherwise. Internal to userland/ui -- callers draw through ugfx.h.
#ifndef UGFX_PIX_H
#define UGFX_PIX_H

#include <stdint.h>

struct ugfx_pix_ops {
    const char *name;

    // --- the blur (ugfx_blur.c) -------------------------------------
    // One box pass along a line, window 2r+1, edges clamped, r >= 1.
    void (*box_h)(const uint32_t *src, uint32_t *dst, int n, int r);
    // One box pass DOWN every column of an n x m picture; `scratch`
    // holds 4 * n words.
    void (*box_v)(const uint32_t *src, uint32_t *dst, int n, int m, int r, uint32_t *scratch);
    // Rows r0 and r1, n pixels, averaged in 2x2 blocks into (n+1)/2
    // pixels (a lone last column pairs with itself).
    void (*down2)(const uint32_t *r0, const uint32_t *r1, uint32_t *dst, int n);
    // One full-size row of n pixels, bilinear between half-size rows a
    // and b (dn pixels each) at vertical weight fy/256, fy in {0, 64, 192}.
    void (*up2)(const uint32_t *a, const uint32_t *b, uint32_t *dst, int n, int dn, unsigned fy);

    // --- mixing: (u * (255 - a) + o * a + 127) / 255, per channel ----
    // dst[i] = mix(under[i], over, a), one colour over a row.
    void (*mix_const)(uint32_t *dst, const uint32_t *under, uint32_t over, unsigned a, int n);
    // dst[i] = mix(under[i], over[i], a).
    void (*mix_rows)(uint32_t *dst, const uint32_t *under, const uint32_t *over, unsigned a,
                     int n);
    // ugfx_blit_glass's row: src[i] copied whole where its top byte is 0,
    // else mix(under[i], src[i], a). `under` may be `dst`.
    void (*glass)(uint32_t *dst, const uint32_t *src, const uint32_t *under, unsigned a, int n);

    // --- blending: (c * a + d * (255 - a)) / 255, TRUNCATED ----------
    // dst[i] = blend(c, dst[i], a); a in 1..254.
    void (*blend_const)(uint32_t *dst, uint32_t c, unsigned a, int n);
    // The same with a = cov[i]: 0 leaves the pixel, 255 writes c.
    void (*blend_cov)(uint32_t *dst, uint32_t c, const uint8_t *cov, int n);
};

extern const struct ugfx_pix_ops ugfx_pix_c, ugfx_pix_sse2;

// The table in use: chosen on first call from UGFX_SIMD in the
// environment ("off" = C), then whatever ugfx_simd_set() last chose.
const struct ugfx_pix_ops *ugfx_pix(void);

#endif
