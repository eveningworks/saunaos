#ifndef WM_QUICK_H
#define WM_QUICK_H

// QUICK SETTINGS: network, volume and brightness are ONE tray button
// (tray_register_group()), and it opens a card of tiles -- network, Do Not
// Disturb, keyboard layout, remote desktop, the on-screen keyboard,
// transparency -- with the volume and brightness sliders and All
// settings. Windows 11's shape, picked from mockups 2026-10-29.
//
// A TILE'S CHEVRON OPENS TODAY'S FLYOUT AS A PAGE: the owner's own card,
// anchored over the group (its tray item is absorbed, so its rect is the
// group's), while this overlay stays up as its parent and draws Back
// over the page's badge. Nothing of those flyouts is drawn twice; a
// tile and a slider act through the owner's own entry points
// (network_toggle(), volume_slider(), ...), so a change made here is
// the one the flyout and the tray icon already show.
//
// Drives and the bell stay their own tray items; the remote-activity
// icon too, as a status that comes and goes. An OVERLAY, ahead of every
// member's row in wm_overlay.c's table, so the group's click is its own.

#include <stdint.h>

void wm_quick_init(void);   // after every member's tray item exists
void wm_quick_poll(void);   // per frame: `desktop.tray.quick` groups or ungroups

int  wm_quick_is_open(void);
void wm_quick_draw(int mx, int my);
int  wm_quick_click(int mx, int my);
int  wm_quick_hover_at(int mx, int my);
void wm_quick_damage(void);
int  wm_quick_rect(int *x, int *y, int *w, int *h);
void wm_quick_update_press(int mx, int my, uint8_t buttons);
int  wm_quick_key(int key, uint8_t mods);
void wm_quick_close(void);

// For `gui quick --json`: the card, the page shown (0 none), the group
// button, Back, and each tile and slider where it is drawn.
#define WM_QUICK_TILES 6
struct wm_quick_tile_info {
    const char *name;
    int on, disabled;
    int x, y, w, h;          // the tile's main part
    int cx, cw;              // its chevron, 0 wide for none
};
struct wm_quick_slider_info {
    const char *name;
    int level, min, max, shown;
    int x, y, w, h;          // the track
    int cx, cy, cs;          // its chevron
};
struct wm_quick_info {
    int open, page;
    const char *page_name;
    int x, y, w, h;
    int bx, by, bw, bh;      // the group button in the tray
    int back_x, back_y, back_s;
    struct wm_quick_tile_info tile[WM_QUICK_TILES];
    struct wm_quick_slider_info slider[2];
    int set_x, set_y, set_w, set_h;   // All settings
};
void wm_quick_state(struct wm_quick_info *out);

#endif // WM_QUICK_H
