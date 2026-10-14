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

// ONE PICTURE OVER ANOTHER, the exception to "paint replaces": `src`
// laid on `dst` over `r` at `opacity` 0..255, Porter-Duff source-over on
// STRAIGHT (unpremultiplied) alpha -- how a layer shows over the ones
// below it. The two must be the same size; `r` is clipped to them.
void upix_over(struct uimg *dst, const struct uimg *src, const struct upix_rect *r, int opacity);
// The whole of `src` laid on `dst` with its top-left at (x, y), clipped
// -- a stamp of anti-aliased letters, their coverage in the alpha.
void upix_over_at(struct uimg *dst, int x, int y, const struct uimg *src);

// --- shapes: coverage masks -------------------------------------------
//
// A SHAPE IS PAINTED IN TWO STEPS: its coverage -- 0..255 per pixel, how
// much of the pixel it covers -- into a mask, then the mask laid on the
// picture in a colour. Anti-aliased, a fill takes four sub-rows per pixel
// with exact span ends and a stroke the distance to its path; without,
// a pixel is in or out at its centre and painted exactly, as the pencil
// is. The fill rule is even-odd.

struct upix_pt { float x, y; };

struct upix_mask {
    int x, y, w, h;      // where it sits on the picture
    uint8_t *a;          // w * h coverage, malloc'd
};

// The interior of the closed polygon `p`, clipped to a cw x ch picture.
// 0 or -ENOMEM; an empty mask (w 0) is not an error.
int  upix_mask_fill(struct upix_mask *m, const struct upix_pt *p, int n, int cw, int ch, int aa);
// The path `p` stroked `width` wide, with round joins and caps; `closed`
// joins the last point back to the first.
int  upix_mask_stroke(struct upix_mask *m, const struct upix_pt *p, int n, int closed, float width,
                      int cw, int ch, int aa);
// `colour` laid on `im` at the mask's coverage, source-over. With `exact`,
// a covered pixel takes the colour as it is (alpha too), as paint does.
void upix_mask_paint(struct uimg *im, const struct upix_mask *m, uint32_t colour, int exact,
                     struct upix_rect *dirty);
void upix_mask_free(struct upix_mask *m);

// THE FIGURES, as a path inside the box (x0, y0)-(x1, y1): what a paint
// program's shape gallery draws. Writes at most `max` points and returns
// how many; every figure is closed.
enum upix_figure {
    UPIX_FIG_RECT, UPIX_FIG_ROUNDED, UPIX_FIG_ELLIPSE, UPIX_FIG_TRIANGLE, UPIX_FIG_ARROW, UPIX_FIG_STAR,
};
int  upix_figure(enum upix_figure f, float x0, float y0, float x1, float y1, struct upix_pt *out, int max);
// A cubic Bezier from p0 to p3 flattened into `n` + 1 points.
int  upix_bezier(struct upix_pt p0, struct upix_pt p1, struct upix_pt p2, struct upix_pt p3,
                 struct upix_pt *out, int n);

#endif
