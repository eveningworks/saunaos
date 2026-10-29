#ifndef WM_SWITCHER_H
#define WM_SWITCHER_H

// ALT+TAB: a card of every window as a live thumbnail, most recently
// used first -- Windows 11's switcher and Plasma 6's Thumbnail Grid.
//
//   Alt+Tab        opens it with the PREVIOUS window picked; each further
//                  Tab moves on, Shift+Tab back, arrows too
//   Alt+`          the same over the focused app's windows only (KDE's
//                  Alt+`, GNOME's Alt+Above_Tab)
//   Alt released   switches to the pick -- a quick tap is "the last
//                  window", and swaps without the card ever drawing
//   Esc            cancels; a click on an entry switches to it
//
// MINIMIZED WINDOWS ARE LISTED, after the others (minimizing sends a
// window to the bottom of Windows' z-order, so its Alt+Tab puts it
// last). The list is what the taskbar lists (taskbar_lists_window()):
// no menus, no dialogs -- switching to an owner raises its dialog too.
//
// AN OVERLAY in wm_overlay.c's table. Entries are held by `open_seq`,
// never by windows[] index; thumbnails come from wm_thumb.h, the cache
// the taskbar's peek card uses.

#include <stdint.h>

// From wm.c's key loop. step() opens the switcher on its first call and
// moves the pick after that; it returns 0 when there is nothing to
// switch between, so the key goes on to its other uses.
int  wm_switcher_step(int dir, int same_app);
int  wm_switcher_active(void);     // Alt is held and a pick is being made
void wm_switcher_commit(void);     // Alt's release

// Per frame, after input: shows the card once the tap delay has passed,
// refreshes stale thumbnails and drops windows that closed.
void wm_switcher_tick(void);
// How long the loop may park before the card is due; -1 for no limit.
int  wm_switcher_wait_ms(void);
// A client presented: its thumbnail is stale (wm_thumb.c fans this out).
void wm_switcher_presented(uint32_t open_seq);

// The overlay's ops (wm_overlay.h).
int  wm_switcher_is_open(void);
void wm_switcher_draw(int mx, int my);
int  wm_switcher_click(int mx, int my);
int  wm_switcher_hover_at(int mx, int my);
void wm_switcher_damage(void);
int  wm_switcher_rect(int *x, int *y, int *w, int *h);
int  wm_switcher_key(int key, uint8_t mods);
void wm_switcher_close(void);

// For `gui switcher --json`: whether it is picking and drawn, the card,
// and each entry in order. Returns the entry count; 0 when closed.
struct wm_switcher_entry_info {
    const char *title;
    int minimized;
    int x, y, w, h;               // the entry's cell -- what a click hits
    int thumb_x, thumb_y, thumb_w, thumb_h;   // 0 x 0: drawn as the icon
};
// The last switch Alt's release made: how long Alt was held, in ms (-1:
// none yet), and whether the card had drawn by then.
void wm_switcher_last(int *ms, int *drawn);
int wm_switcher_state(int *shown, int *pick, int *x, int *y, int *w, int *h,
                      struct wm_switcher_entry_info *out, int max);

#endif // WM_SWITCHER_H
