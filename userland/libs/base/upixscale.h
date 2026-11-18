#ifndef ULIB_UPIXSCALE_H
#define ULIB_UPIXSCALE_H

// A 32-bit PICTURE SCALED SHARP: nearest-neighbour up to the next whole
// factor, then bilinear to the size asked -- the "sharp bilinear" retro
// emulators default to. Pixels stay crisp and only the edges between
// them blend, so a 640x400 game shown at 125% is neither soft (plain
// bilinear, what Windows does to a DPI-unaware window) nor uneven
// (nearest, which doubles every fourth pixel). A whole factor is pure
// pixel duplication. All four channels are blended, alpha included.
//
// The work per pixel is two table lookups and up to three blends; the
// tables depend only on the four sizes, so prepare once and run each
// frame.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct upixscale {
    int sw, sh, dw, dh;
    // Per destination column (x*) and row (y*): the two source indices
    // and the weight of the second, 0..256.
    int *xa, *xb, *ya, *yb;
    uint16_t *xw, *yw;
};

// Builds the tables for scaling sw x sh to dw x dh (each >= 1), reusing
// them when the sizes are unchanged. 1, or 0 when out of memory or a
// size is not positive -- `s` is then empty and upixscale_run() a no-op.
// Zero the struct before its first use.
int upixscale_prepare(struct upixscale *s, int sw, int sh, int dw, int dh);

// `src` is sw x sh with `src_stride` pixels per row; `dst` dw x dh with
// `dst_stride`. They must not overlap.
void upixscale_run(const struct upixscale *s, const uint32_t *src, int src_stride,
                   uint32_t *dst, int dst_stride);

void upixscale_free(struct upixscale *s);

#ifdef __cplusplus
}
#endif

#endif
