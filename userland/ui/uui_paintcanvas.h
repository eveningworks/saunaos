#ifndef UUI_PAINTCANVAS_H
#define UUI_PAINTCANVAS_H

#include <stdint.h>
#include "lib/uimg.h"
#include "ui/uui_sbar.h"
#include "ui/uui_widget.h"

// uui_paintcanvas -- A PICTURE TO PAINT ON, ZOOMED AND SCROLLED: the view
// half of a paint program. It shows the caller's struct uimg at a zoom,
// centres it when it is smaller than the view, scrolls it with two bars
// and the wheel, and turns the pointer into PICTURE coordinates.
//
// **WHAT THE POINTER DOES IS THE APP'S.** A press, each motion with it
// held, the release and every hover arrive through `on_pointer` in
// picture pixels -- which is where the tools live, as a GtkDrawingArea
// leaves its events to its owner. The widget never changes a pixel.
//
// NEAREST NEIGHBOUR AT EVERY ZOOM, on purpose: at 800% each pixel is a
// square to aim a pencil at, and a smoothed picture would hide the very
// pixel being edited. Transparency shows through a checkerboard.
//
// The wheel scrolls (Shift: sideways); Ctrl+wheel zooms about the
// pointer, as in every editor -- through uapp_keyboard_mods().

enum uui_paint_phase { UUI_PAINT_DOWN, UUI_PAINT_MOVE, UUI_PAINT_UP, UUI_PAINT_HOVER };

#define UUI_PAINT_ZOOM_MIN 10     // percent
#define UUI_PAINT_ZOOM_MAX 3200

struct uui_paintcanvas {
    int x, y, w, h;
    struct uimg *img;            // the caller's; NULL shows the ground alone
    int zoom;                    // percent; 100 is one picture pixel per screen pixel

    // `x`/`y` in picture pixels (outside the picture too, so a stroke
    // can run off its edge); `mods` the KEY_MOD_* held.
    void (*on_pointer)(void *ctx, enum uui_paint_phase phase, int x, int y, unsigned mods);
    // After the picture, in screen coordinates: a selection, a shape
    // being drawn. Use uui_paintcanvas_to_screen() to place it.
    void (*draw_over)(void *ctx, struct ugfx_surface *s, const struct uui_paintcanvas *c);
    void *ctx;
    int cursor;                  // WIN_CURSOR_* over the picture; the app's per tool

    // --- OWNED --------------------------------------------------------
    int sx, sy;                  // the view's offset into the zoomed picture
    struct uui_sbar vbar, hbar;
    int painting;                // a press of ours is held
    int bar_held;                // ...or a bar has it
    int hover_x, hover_y, hovering;   // the pointer's last picture position
};

void uui_paintcanvas_init(struct uui_paintcanvas *c, struct uimg *img);
// The picture's size changed (a new one, a rotate): back in range.
void uui_paintcanvas_picture_changed(struct uui_paintcanvas *c);
// To `zoom` percent, keeping the picture point under (ax, ay) where it
// is -- or the view's centre, for ax < 0.
void uui_paintcanvas_set_zoom(struct uui_paintcanvas *c, int zoom, int ax, int ay);
// The next step in or out of 10..3200%: 100, 200, 400... and back.
int  uui_paintcanvas_zoom_step(int zoom, int dir);
// The largest zoom, at most 100%, that shows the whole picture.
int  uui_paintcanvas_fit_zoom(const struct uui_paintcanvas *c);

// Screen to picture (rounding down, so a pixel is the whole square that
// shows it): 1 when inside the picture. And back: a picture pixel's
// top-left on screen, and the picture's whole rect there.
int  uui_paintcanvas_to_picture(const struct uui_paintcanvas *c, int cx, int cy, int *x, int *y);
void uui_paintcanvas_to_screen(const struct uui_paintcanvas *c, int x, int y, int *cx, int *cy);
void uui_paintcanvas_picture_rect(const struct uui_paintcanvas *c, int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops uui_paintcanvas_ops;

#endif
