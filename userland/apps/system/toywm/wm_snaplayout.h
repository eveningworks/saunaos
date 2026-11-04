#ifndef WM_SNAPLAYOUT_H
#define WM_SNAPLAYOUT_H

// SNAP LAYOUTS: resting the pointer on a window's Maximize button opens
// a card of six screen layouts under it -- halves, a half and two
// quarters either way, quarters, thirds, two thirds and a third --
// Windows 11's Snap layouts. Each layout's zones are targets: the
// hovered one lights and its place on the screen shows as the snap
// preview; a click lays the window there, and Snap Assist then offers
// the rest of that layout's zones one after another (wm_switcher.h).
// Super+Z opens it for the focused window; the arrows walk the zones,
// Enter takes one, Esc closes it.
//
// An OVERLAY in wm_overlay.c's table. The window is held by open_seq.

#include <stdint.h>

// Per frame, after the title bar's hover is updated: opens the card once
// the pointer has rested on a Maximize button, and closes it once the
// pointer has left both the button and the card for a moment.
void wm_snaplayout_poll(int mx, int my, uint8_t buttons);
int  wm_snaplayout_wait_ms(void);

// Super+Z on windows[idx]; 0 when that window cannot snap.
int  wm_snaplayout_open_for(int idx);

int  wm_snaplayout_is_open(void);
void wm_snaplayout_draw(int mx, int my);
int  wm_snaplayout_click(int mx, int my);
int  wm_snaplayout_hover_at(int mx, int my);
void wm_snaplayout_damage(void);
int  wm_snaplayout_rect(int *x, int *y, int *w, int *h);
int  wm_snaplayout_key(int key, uint8_t mods);
void wm_snaplayout_close(void);

// For `gui snaplayout --json`: the card, and each zone's rect in it with
// the zone it stands for. Returns the zone count; 0 when closed.
struct wm_snaplayout_zone_info {
    int layout;          // 0..5, in the card's reading order
    uint16_t zone;       // wm_snap.h's packing
    int x, y, w, h;      // its rect on the card -- what a click hits
};
int wm_snaplayout_state(int *x, int *y, int *w, int *h, int *hot,
                        struct wm_snaplayout_zone_info *out, int max);

#endif // WM_SNAPLAYOUT_H
