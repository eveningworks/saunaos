#ifndef ULIB_UPIXEL_H
#define ULIB_UPIXEL_H

#include <stdint.h>
#include "lib/uimg.h"

// upixel -- PAINTING ON A PICTURE: a brush stamp, a stroke of them, a
// flood fill, on a struct uimg's 0xAARRGGBB pixels. What a paint
// program's tools are made of, kept apart from any one program.
//
// PAINT REPLACES, IT DOES NOT BLEND: the colour written is the colour
// the pixel holds afterwards, alpha and all -- MS Paint's and
// KolourPaint's pencil, and what makes a fill's edge exact. Every call
// clips to the picture, so a stroke may run off its edge.
//
// Each call GROWS a dirty rect (when one is given) by what it touched,
// which is what a redraw and an undo step want to know.

struct upix_rect { int x, y, w, h; };   // w or h 0: empty

enum upix_shape { UPIX_ROUND, UPIX_SQUARE };

// `r` grown to cover (x, y, w, h), clipped to `im`.
void upix_rect_add(struct upix_rect *r, const struct uimg *im, int x, int y, int w, int h);

// The pixel at (x, y), or 0 outside the picture.
uint32_t upix_get(const struct uimg *im, int x, int y);

// One stamp of a `size`-pixel brush CENTRED on (cx, cy): a square, or the
// disc inside it. An even size has its centre between pixels, so it sits
// a half pixel down and right of (cx, cy), as every editor's does.
void upix_stamp(struct uimg *im, int cx, int cy, int size, enum upix_shape shape,
                uint32_t colour, struct upix_rect *dirty);

// Stamps along (x0, y0)-(x1, y1), one per pixel step, so a fast stroke
// has no gaps. `from_start` 0 leaves out the first point: a stroke's next
// segment begins where the last one ended, which is already painted.
void upix_line(struct uimg *im, int x0, int y0, int x1, int y1, int size,
               enum upix_shape shape, uint32_t colour, int from_start,
               struct upix_rect *dirty);

// Fills the 4-connected region around (x, y) whose pixels are within
// `tolerance` of its colour, channel by channel (0: that exact colour).
// Returns the pixels changed, or -ENOMEM. Its scratch is allocated per
// call: one bit per pixel, and a stack of row spans.
int upix_fill(struct uimg *im, int x, int y, uint32_t colour, int tolerance,
              struct upix_rect *dirty);

#endif
