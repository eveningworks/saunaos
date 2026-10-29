#ifndef ULIB_UCURSOR_H
#define ULIB_UCURSOR_H

#include <stdint.h>

// One cursor SHAPE, as /usr/share/cursors/<theme>/<shape> describes it --
// the file format, shared by the compositor (userland/wm/cursor_theme.c,
// which owns the live theme) and anything that only SHOWS a theme
// (System Settings' gallery). Two kinds, Xcursor's split:
//   - MASKS: an outline and a fill coverage grid, coloured at draw time
//     (a black rim under `fill_rgb`). `default` and `bold` are masks.
//   - an IMAGE: straight-alpha ARGB in a QOI file beside the descriptor,
//     whose colours are its own, pre-rendered at each UCURSOR_SET_PCT size
//     a theme ships, so a large pointer is redrawn art rather than
//     doubled pixels -- the in-between ones (125-175%) for display.scale.

#define UCURSOR_DIR "/usr/share/cursors"
// The authored size cap AT 1x, for both kinds, and the masks' storage.
#define CURSOR_SHAPE_MAX 32
// The largest size setting, and so the largest pre-rendered scale.
#define CURSOR_SCALE_MAX 3
// The renderings a descriptor can name, in percent: `image=` (100),
// `scale125=` .. `scale175=`, `image2=` (200), `image3=` (300). The
// in-between keys do not start with "image", so an older parser ignores
// them as unknown keys instead of refusing the shape.
#define UCURSOR_SETS 6
extern const int UCURSOR_SET_PCT[UCURSOR_SETS];
// An image file's name as the descriptor gives it: "arrow@2x.qoi".
#define CURSOR_IMAGE_NAME_MAX 32
// A whole descriptor; bigger is refused, not truncated.
#define CURSOR_FILE_MAX 4096

struct cursor_shape {
    int loaded;              // 0 means "use the built-in fallback"
    int w, h;                // AT 1x, whichever kind
    int hot_x, hot_y;        // the pixel that IS the pointer position, at 1x
    unsigned char outline[CURSOR_SHAPE_MAX][CURSOR_SHAPE_MAX];
    unsigned char fill[CURSOR_SHAPE_MAX][CURSOR_SHAPE_MAX];

    // An IMAGE shape: `image[i]` names the UCURSOR_SET_PCT[i] file ("" if
    // absent); ucursor_decode() fills `px` -- pw by ph straight-alpha
    // 0xAARRGGBB, malloc'd, freed by ucursor_release().
    int is_image;
    char image[UCURSOR_SETS][CURSOR_IMAGE_NAME_MAX];
    int baked;               // the size `px` was rendered at, in percent
    int pw, ph;              // `px`'s dimensions: ucursor_scaled() of w, h
    uint32_t *px;
};

// Parses one DESCRIPTOR; pure, no filesystem (an image shape only NAMES
// its files). 1 on success, 0 on any malformation -- it REJECTS rather
// than guesses. TRAP: it overwrites `out` wholesale, `px` included, so
// release an image shape before handing it back here, or it leaks.
int cursor_shape_parse(const char *text, uint32_t len, struct cursor_shape *out);

// A 1x length at `pct`, rounded half up -- the size gen_cursors.py
// renders an in-between set at, so a file is checked against it exactly.
int ucursor_scaled(int v, int pct);

// Which rendering `pct` takes: the exact set when the theme has one,
// else the whole-number set nearest it (the 1x when that is absent too).
// The percent of the set chosen.
int ucursor_pick(const struct cursor_shape *s, int pct);

// Decodes an image shape's rendering for `pct` (ucursor_pick()). A file
// whose size is not exactly ucursor_scaled() of the descriptor's is
// refused. 1 on success; on failure `why` (if any) holds a sentence for
// the caller's log and the shape is untouched.
int ucursor_decode(const char *theme, struct cursor_shape *s, int pct,
                   char *why, int why_len);

// Frees an image shape's pixels and marks it unloaded. Safe on any shape.
void ucursor_release(struct cursor_shape *s);

// Reads, parses and (for an image) decodes UCURSOR_DIR/<theme>/<name> at
// `pct`, for a caller with no file layer of its own. 1 on success; an
// absent shape is 0 and not an error -- a theme need not be complete.
// Not reentrant: one static read buffer.
int ucursor_load(const char *theme, const char *name, int pct, struct cursor_shape *out);

// The pixel at offset (dx, dy) from the top-left, where each stored pixel
// (of the decoded rendering, for an image) covers `step` drawn pixels: an image's own ARGB, or a mask pair folded
// into straight ARGB as `fill_rgb` over a black rim. 0 outside.
uint32_t ucursor_pixel(const struct cursor_shape *s, int dx, int dy, int step,
                       uint32_t fill_rgb);

#endif // ULIB_UCURSOR_H
