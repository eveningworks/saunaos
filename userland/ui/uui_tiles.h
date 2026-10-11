#ifndef UUI_TILES_H
#define UUI_TILES_H

// The pictures a uui_gallery's cards show for a cursor set and for a
// wallpaper -- System Settings' and First-boot setup's, from one place.
// Each painter is a tile callback's body: card `slot`'s rect, the choice's
// name, and a cache the caller owns and resets when its page closes.
//
// CACHED PER SLOT, decoded once: a gallery repaints on every hover, and a
// tile drawn per paint was a decode per hover.

#include <stdint.h>
#include "ui/ugfx.h"
#include "lib/ucursor.h"
#include "lib/uimg.h"

#define UUI_TILES_MAX 16
// The desktop's own colour (desktop.c's DESKTOP_BG): behind a cursor
// set's lower half, and under a picture that has not loaded.
#define UUI_TILE_DARK ugfx_rgb(24, 60, 90)

#define UUI_TILE_CURSOR_SAMPLES 5
struct uui_cursor_tiles {
    struct cursor_shape shape[UUI_TILES_MAX][UUI_TILE_CURSOR_SAMPLES];
    char theme[UUI_TILES_MAX][64];
    int loaded[UUI_TILES_MAX];
};

// Five shapes of `theme` -- arrow, I-beam, hand, busy, a resize -- on a
// light half and a dark half: a set must read on a window AND on the
// desktop (KDE's Cursors page samples the same handful).
void uui_cursor_tile(struct ugfx_surface *s, struct uui_cursor_tiles *c, int slot,
                     const char *theme, int x, int y, int w, int h);
void uui_cursor_tiles_reset(struct uui_cursor_tiles *c);

struct uui_picture_tiles {
    struct uimg img[UUI_TILES_MAX];
    char name[UUI_TILES_MAX][64];
};

// Fills `out` with a picture COVERING w x h; 0 when it cannot.
typedef int (*uui_tile_loader)(const char *name, int w, int h, struct uimg *out, void *ctx);

// The picture for `name`, made by `load` the first time and blitted
// centred after -- re-made only when the name or a bigger size is asked.
void uui_picture_tile(struct ugfx_surface *s, struct uui_picture_tiles *c, int slot,
                      const char *name, int x, int y, int w, int h,
                      uui_tile_loader load, void *ctx);
void uui_picture_tiles_reset(struct uui_picture_tiles *c);

// `dir/stem.<ext>` for the first extension that decodes -- or a video's
// frame a second in -- scaled to COVER w x h, cropped the way "Fill the
// screen" crops. 1 when it loaded. A uui_tile_loader for a picture
// directory is this with the directory as `ctx`: uui_tile_load_dir.
int uui_tile_load_cover(const char *dir, const char *stem, int w, int h, struct uimg *out);
int uui_tile_load_dir(const char *name, int w, int h, struct uimg *out, void *dir);

#endif
