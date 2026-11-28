#ifndef WM_BATTERY_POPUP_H
#define WM_BATTERY_POPUP_H

// THE TRAY'S BATTERY ITEM: the charge as the icon's fill (a bolt while it
// charges, amber when low, red when critical), a member of Quick Settings'
// group as on Windows 11, and the Battery card -- Quick Settings' page,
// opened from its footer -- with the charge and the battery's details.
// The "Battery low" notice comes from here too, once per discharge.
//
// The facts are the kernel's (QUERY_POWER, base/upower.h); `auto` shows
// the item only where a battery is in and says its charge, as the brightness sun shows only
// where there is a backlight. Read-only: nothing here changes the battery.

struct battery_geom {
    int x, y, w, h;                    // the card
    int tray_x, tray_y, tray_w, tray_h;
    int pad, hero_h, row_h;
    int bar_x, bar_y, bar_w, bar_h;    // the charge bar under the header
    int rule_y;
    int cap_y, kv_y, kv_rows, kv_key_w;
    int foot_y, foot_h, btn_y, btn_h;
    int set_x, set_w;                  // "Power settings..."
};

extern int battery_open;

void battery_tray_init(void);
void battery_poll(void);              // once a second
int  battery_tray_id(void);
int  battery_tray_hidden(void);
int  battery_present(void);           // a battery is in: Quick Settings' footer shows it
// "78% · 4 h 53 min left" -- Quick Settings' footer and the tooltip.
void battery_summary(char *out, unsigned cap);
const char *battery_icon(void);       // the icon now, for the footer

void battery_geometry(struct battery_geom *g);
void battery_draw(int mx, int my);
int  battery_handle_click(int mx, int my);
int  battery_hover_at(int mx, int my);
void battery_damage(void);
int  battery_rect(int *x, int *y, int *w, int *h);
void battery_close(void);
void battery_open_page(void);         // Quick Settings' Battery page

// For `gui battery --json` (wm_debug.c).
struct battery_debug {
    int batteries, percent, charging, discharging, plugged;
    long long seconds;
    int warned;                       // the low notice went up this discharge
    unsigned ink;                     // the icon's own ink; 0 the panel's
};
void battery_debug(struct battery_debug *d);

#endif // WM_BATTERY_POPUP_H
