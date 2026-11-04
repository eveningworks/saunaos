#ifndef WM_SNAP_H
#define WM_SNAP_H

// WINDOW SNAPPING: a window laid into a share of the work area -- a half,
// a quarter, a third -- by dragging it to an edge or corner, by
// Super+arrows, or from the Snap layouts card on its Maximize button.
// Windows 11's Snap and KWin's quick tiling are the same gesture:
//
//   drag to the left/right edge     that half
//   drag into a corner              that quarter
//   drag to the top edge            maximize
//   Super+Left / Super+Right        a half; the other side's half, or a
//                                   maximized window, restores first
//   Super+Up                        a half becomes its top quarter, a
//                                   bottom quarter its half; else maximize
//   Super+Down                      maximized restores, a half becomes its
//                                   bottom quarter, a top quarter its
//                                   half; else minimize
//
// A ZONE is a rect in SIXTHS of the work area, packed four nibbles to a
// uint16_t -- halves, quarters, thirds and two-thirds are all whole
// sixths, and a zone survives a resolution or taskbar change because it
// is a fraction, not pixels (wm_snap_relayout()). `struct window.snap`
// holds the zone a window is in, 0 when it is in none; `saved_*` holds
// the rect it had before, which is where a drag away or Super+Down puts
// it back -- the same slot maximize uses, so a snapped window maximized
// and restored lands back on its own size, not on the half.
//
// AFTER A HALF-SCREEN SNAP the other half offers the remaining windows
// (Snap Assist, wm_switcher.h's assist mode); a click puts that one there.

#include <stdint.h>

#define WM_SNAP_ZONE(x0, x1, y0, y1) \
    ((uint16_t)((x0) | ((x1) << 4) | ((y0) << 8) | ((y1) << 12)))
#define WM_SNAP_LEFT   WM_SNAP_ZONE(0, 3, 0, 6)
#define WM_SNAP_RIGHT  WM_SNAP_ZONE(3, 6, 0, 6)
#define WM_SNAP_TL     WM_SNAP_ZONE(0, 3, 0, 3)
#define WM_SNAP_TR     WM_SNAP_ZONE(3, 6, 0, 3)
#define WM_SNAP_BL     WM_SNAP_ZONE(0, 3, 3, 6)
#define WM_SNAP_BR     WM_SNAP_ZONE(3, 6, 3, 6)
// Not a zone: the top edge's drop target, which maximizes.
#define WM_SNAP_MAXIMIZE 0xFFFFu

// The zone's rect in screen pixels, inside the work area.
void wm_snap_zone_rect(uint16_t zone, int *x, int *y, int *w, int *h);

// Lay windows[idx] into `zone` (0: back to its saved rect). A window
// that cannot be resized, a popup, or a fullscreen one is left alone;
// returns 1 when it moved.
int wm_snap_window(int idx, uint16_t zone);

// The same, then Snap Assist when the zone is a half and the other half
// is empty -- what a drag to an edge and Super+Left/Right do.
int wm_snap_window_assisted(int idx, uint16_t zone);

// Super+arrow on the focused window; 1 when the key was used.
int wm_snap_key(int idx, int key);

// The drag: what the pointer is over (0 none, a zone, or MAXIMIZE), the
// translucent preview of it, and the drop. wm_input.c calls these from
// the move path.
uint16_t wm_snap_zone_at(int mx, int my);
void wm_snap_preview(uint16_t zone);      // 0 hides it
uint16_t wm_snap_preview_zone(void);
void wm_snap_preview_draw(void);          // wm_render.c, over the windows

// A rounded sheet of `color` at `alpha` over what is drawn -- the
// preview's, and Snap Assist's ground.
void wm_snap_sheet(int x, int y, int w, int h, int r, uint32_t color, uint8_t alpha);

// The work area changed (taskbar, resolution): every snapped window is
// laid into its zone again.
void wm_snap_relayout(void);

// For `gui windows --json` and tests: "left", "tr", "6:0-2,0-6", or "".
void wm_snap_name(uint16_t zone, char *out, int cap);

#endif // WM_SNAP_H
