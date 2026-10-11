#ifndef ULIB_UIMG_REDUCE_H
#define ULIB_UIMG_REDUCE_H

// A picture shrunk by WHOLE factors as its rows arrive: every kx x ky
// block of source pixels becomes one pixel, its average -- alpha weighted
// when the picture has alpha, so a transparent pixel's colour does not
// bleed into its neighbours. The cheap first step of a big shrink
// (uimg_scale(), and a thumbnail decoded straight from a file): libjpeg's
// scale-on-decode and a mipmap level, in effect.
//
// ROWS GO IN ONE AT A TIME, so the full-size picture need never exist: a
// codec that decodes in row order (QOI) feeds this instead of a 33 MB
// buffer. The blocks at the right and bottom edges may be partial and are
// averaged over the pixels they have.
//
// All integer. A full block divides by a reciprocal; only a partial one,
// and the colour of a block with alpha, divide.

#include <stdint.h>
#include "lib/uimg.h"

struct uimg_reduce {
    int sw, sh, kx, ky;
    int alpha;              // weigh colour by alpha
    int y;                  // source rows taken so far
    struct uimg out;        // ceil(sw/kx) x ceil(sh/ky), filled as bands complete
    uint32_t *acc;          // 5 sums per output column: r, g, b (weighted when alpha), a, count
};

// 0, or -ENOMEM. `alpha`: the source may have transparent pixels.
int  uimg_reduce_init(struct uimg_reduce *r, int sw, int sh, int kx, int ky, int alpha);
// One source row, `sw` pixels, top to bottom.
void uimg_reduce_row(struct uimg_reduce *r, const uint32_t *row);
// The reduced picture, moved into `out` (the caller frees it); call once,
// after the last row. 0, or -EINVAL when rows were missing.
int  uimg_reduce_finish(struct uimg_reduce *r, struct uimg *out);
// Frees what init took, for a caller giving up part way.
void uimg_reduce_abort(struct uimg_reduce *r);

// The factors that shrink sw x sh to no less than twice dw x dh -- the
// headroom the precise resampler then needs for a clean last step. 1
// along an axis not shrunk by 4x or more.
void uimg_reduce_factors(int sw, int sh, int dw, int dh, int *kx, int *ky);

#endif
