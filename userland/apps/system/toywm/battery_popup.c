// See battery_popup.h. The tray's battery item and its card.
#include "wm_internal.h"
#include "battery_popup.h"
#include "crash_notice.h"
#include "wm_flyout.h"
#include "wm_tray.h"
#include "wm_overlay.h"
#include "wm_taskbar.h"     // taskbar_dark()
#include "base/upower.h"
#include "base/ulog.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "apps/system/toywm/wm_conf.h"   // wm_setting_generation()

int battery_open = 0;

static int g_tray_id = -1;
static int g_hover;                 // 1 the tray item, 2 Power settings
static struct upower g_p;
static uint64_t g_seen_power;       // QUERY_POWER's generation last read
static uint32_t g_seen_setting;
static int g_last_present = -1;
static int g_warned;                // the low notice went up this discharge
static char g_icon[32] = "tray-battery-100";
static uint32_t g_ink;              // the icon's own ink; 0 the panel's

// --- the facts ------------------------------------------------------------

static void choose_icon(void) {
    // An unknown charge keeps the glyph it had: an empty one reads as dead.
    if (g_p.percent < 0) { tray_set_ink(g_tray_id, g_ink = 0); return; }
    int pct = g_p.percent;
    // In tenths, but never an empty glyph while there is charge: 1-9% shows
    // a sliver, as Windows' does.
    int tenths = (pct + 5) / 10;
    if (pct > 0 && !tenths) tenths = 1;
    k_snprintf(g_icon, sizeof g_icon, g_p.charging ? "tray-battery-charging-%d" : "tray-battery-%d", tenths * 10);
    tray_set_icon(g_tray_id, g_icon);
    // Amber and red only when it is running down; a charging battery at 4%
    // is getting better, and says so with its bolt.
    uint32_t ink = 0;
    if (!g_p.charging && g_p.percent >= 0 && !g_p.plugged) {
        int dark = taskbar_dark();
        if (g_p.percent < UPOWER_CRITICAL_PCT) ink = dark ? ugfx_rgb(255, 107, 107) : ugfx_rgb(196, 43, 28);
        else if (g_p.percent < UPOWER_LOW_PCT) ink = dark ? ugfx_rgb(240, 160, 48) : ugfx_rgb(176, 88, 0);
    }
    g_ink = ink;
    tray_set_ink(g_tray_id, ink);
}

static void open_power_settings(void) {
    int pid = sys_spawn("/bin/wm/system/settings", "desktop.lid", -1);
    if (pid > 0) wm_track_launched(pid);
}

// THE LOW NOTICE: once each time the charge falls through UPOWER_WARN_PCT
// on battery; plugging in re-arms it.
static void low_notice(void) {
    if (g_p.plugged || g_p.charging || g_p.percent < 0) { g_warned = 0; return; }
    if (g_warned || g_p.percent > UPOWER_WARN_PCT) return;
    g_warned = 1;
    char sub[64];
    k_snprintf(sub, sizeof sub, "%d%% left. Plug in the charger.", g_p.percent);
    crash_notice_action("Battery low", sub, "cat-power", "Power settings", open_power_settings);
    ulogf("wm: battery low (%d%%) -- the notice is up\n", g_p.percent);
}

static void build_rows(void);

static void refresh(void) {
    if (upower_read(&g_p) != 0) {
        k_memset(&g_p, 0, sizeof g_p);
        g_p.percent = -1;
        g_p.seconds = -1;
    }
    g_seen_power = g_p.generation;
    build_rows();
    choose_icon();
    low_notice();
    if (battery_open) battery_damage();
}

void battery_poll(void) {
    // The time left moves with no generation change (nothing in the kernel
    // moved), so an open card re-reads every poll; a closed one on a change.
    if (upower_generation() != g_seen_power || battery_open) refresh();
    // A battery with no charge to show (the ASUS's, whose EC reads nothing)
    // is not shown: a full glyph and "?" would be a claim nobody made.
    int present = g_p.batteries > 0 && g_p.percent >= 0;
    uint32_t sg = wm_setting_generation();
    if (sg != g_seen_setting || present != g_last_present) {
        g_seen_setting = sg;
        g_last_present = present;
        tray_set_hidden(g_tray_id, !tray_want_shown("battery", present));
    }
    if (battery_open && tray_is_hidden(g_tray_id)) battery_close();
}

void battery_tray_init(void) {
    g_tray_id = tray_register_icon(g_icon);
    refresh();
    g_warned = g_p.percent >= 0 && g_p.percent <= UPOWER_WARN_PCT;   // not news at sign-in
    g_seen_setting = wm_setting_generation();
    g_last_present = g_p.batteries > 0 && g_p.percent >= 0;
    tray_set_hidden(g_tray_id, !tray_want_shown("battery", g_last_present));
}

int battery_tray_id(void) { return g_tray_id; }
int battery_tray_hidden(void) { return tray_is_hidden(g_tray_id); }
int battery_present(void) { return g_p.batteries > 0 && g_p.percent >= 0 && !tray_is_hidden(g_tray_id); }
void battery_summary(char *out, unsigned cap) { upower_summary(&g_p, out, cap); }
const char *battery_icon(void) { return g_icon; }

void battery_debug(struct battery_debug *d) {
    d->batteries = g_p.batteries;
    d->percent = g_p.percent;
    d->charging = g_p.charging;
    d->discharging = g_p.discharging;
    d->plugged = g_p.plugged;
    d->seconds = g_p.seconds;
    d->warned = g_warned;
    d->ink = g_ink;
}

// --- geometry ---------------------------------------------------------

#define BAT_ROWS 4

// The DETAILS rows, built here so drawing, the card's height and `gui
// battery --json` cannot disagree (the one-geometry rule).
static char g_keys[BAT_ROWS][16], g_vals[BAT_ROWS][48];
static int g_rows;

// Built at each reading, never on a hover: the pack's name is a query.
static void build_rows(void) {
    char (*keys)[16] = g_keys, (*vals)[48] = g_vals;
    int n = 0;
    if (g_p.rate_mw) {
        k_strlcpy(keys[n], g_p.charging ? "Charging at" : "Power draw", 16);
        k_snprintf(vals[n], 48, "%llu.%llu W", (unsigned long long)(g_p.rate_mw / 1000),
                   (unsigned long long)(g_p.rate_mw % 1000 / 100));
        n++;
    }
    if (g_p.full_mwh) {
        k_strlcpy(keys[n], "Energy", 16);
        k_snprintf(vals[n], 48, "%llu.%llu of %llu.%llu Wh", (unsigned long long)(g_p.now_mwh / 1000),
                   (unsigned long long)(g_p.now_mwh % 1000 / 100), (unsigned long long)(g_p.full_mwh / 1000),
                   (unsigned long long)(g_p.full_mwh % 1000 / 100));
        n++;
    }
    if (g_p.design_mwh && g_p.full_mwh) {
        // Linux's and Windows' "health": what it holds now of what it was made to.
        k_strlcpy(keys[n], "Health", 16);
        k_snprintf(vals[n], 48, "%llu%% of its %llu Wh design",
                   (unsigned long long)((g_p.full_mwh * 100 + g_p.design_mwh / 2) / g_p.design_mwh),
                   (unsigned long long)((g_p.design_mwh + 500) / 1000));
        n++;
    }
    struct query_power_supply s;
    QUERY_FOREACH(QUERY_POWER_SUPPLY, s, i) {
        if (s.type != QUERY_SUPPLY_BATTERY || !(s.flags & QUERY_SUPPLY_PRESENT)) continue;
        s.maker[sizeof s.maker - 1] = s.model[sizeof s.model - 1] = 0;
        k_strlcpy(keys[n], "Battery", 16);
        k_snprintf(vals[n], 48, "%s%s%s", s.maker, s.maker[0] && s.model[0] ? " " : "", s.model);
        if (vals[n][0]) n++;
        break;
    }
    g_rows = n;
}

static void headline(char *title, unsigned tcap, char *sub, unsigned scap) {
    if (g_p.percent >= 0) k_snprintf(title, tcap, "%d%%", g_p.percent);
    else k_strlcpy(title, "Battery", tcap);
    char t[32];
    upower_duration(g_p.seconds, t, sizeof t);
    const char *state = upower_state(&g_p);
    if (t[0] && g_p.charging) k_snprintf(sub, scap, "%s \xC2\xB7 %s to full", state, t);
    else if (t[0] && g_p.discharging) k_snprintf(sub, scap, "%s \xC2\xB7 %s left", state, t);
    else k_strlcpy(sub, state, scap);
}

void battery_geometry(struct battery_geom *g) {
    k_memset(g, 0, sizeof *g);
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    int ch = ugfx_char_h();
    int rows = g_rows;
    int key_w = ugfx_text_width("Charging at") + m.pad;
    int w = ch * 21;
    int val_w = 0;
    for (int i = 0; i < rows; i++) {
        int vw = ugfx_text_width(g_vals[i]);
        if (vw > val_w) val_w = vw;
    }
    if (w < 2 * m.pad + key_w + val_w) w = 2 * m.pad + key_w + val_w;
    int foot_w = 2 * m.pad + wm_flyout_button_w("Power settings...", 0);
    if (w < foot_w) w = foot_w;
    int bar_h = ugfx_px(8);
    int h = m.hero_h + bar_h + m.vpad + 1;
    if (rows) h += m.vpad + m.cap_h + rows * m.row_h + m.vpad;
    h += m.foot_h;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(g_tray_id, &tx, &ty, &tw, &th)) {
        tx = screen_w - ugfx_px(32); ty = screen_h - taskbar_h; tw = ugfx_px(16); th = taskbar_h;
    }
    int x, y;
    wm_popup_place(tx + tw - w, screen_h - taskbar_h - h, w, h, &x, &y);
    g->x = x; g->y = y; g->w = w; g->h = h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;
    g->pad = m.pad; g->hero_h = m.hero_h; g->row_h = m.row_h;
    g->bar_x = x + m.pad; g->bar_y = y + m.hero_h; g->bar_w = w - 2 * m.pad; g->bar_h = bar_h;
    g->rule_y = g->bar_y + bar_h + m.vpad;
    int cy = g->rule_y + 1;
    if (rows) {
        g->cap_y = cy + m.vpad;
        g->kv_y = g->cap_y + m.cap_h;
        g->kv_rows = rows;
        g->kv_key_w = key_w;
    }
    g->foot_h = m.foot_h;
    g->foot_y = y + h - m.foot_h;
    g->btn_h = m.btn_h;
    g->btn_y = g->foot_y + (m.foot_h - m.btn_h) / 2;
    g->set_w = wm_flyout_button_w("Power settings...", 0);
    g->set_x = x + w - (m.pad - 6) - g->set_w;
}

int battery_rect(int *x, int *y, int *w, int *h) {
    struct battery_geom g;
    battery_geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

void battery_damage(void) { wm_overlay_damage("battery"); }

static void battery_open_now(void) {
    wm_overlay_close_others("battery");
    refresh();
    battery_open = 1;
    g_hover = 0;
    battery_damage();
}

void battery_open_page(void) { battery_open_now(); }

void battery_close(void) {
    if (!battery_open) return;
    battery_open = 0;   // where it was drawn is damaged by the core (wm_overlay.h)
    g_hover = 0;
}

// --- input ------------------------------------------------------------

int battery_handle_click(int mx, int my) {
    struct battery_geom g;
    battery_geometry(&g);
    if (!battery_open) {
        if (tray_is_hidden(g_tray_id)) return 0;
        if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
            battery_open_now();
            return 1;
        }
        return 0;
    }
    if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
        battery_close();      // a second click on the icon closes it
        return 1;
    }
    if (uui_hit(g.x, g.y, g.w, g.h, mx, my)) {
        if (uui_hit(g.set_x, g.btn_y, g.set_w, g.btn_h, mx, my)) {
            battery_close();
            open_power_settings();
        }
        return 1;   // the rest is ours, and inert
    }
    battery_close();
    return my < screen_h - taskbar_h;   // a click on the taskbar is still the taskbar's
}

int battery_hover_at(int mx, int my) {
    if (tray_is_hidden(g_tray_id)) return 0;
    struct battery_geom g;
    battery_geometry(&g);
    int h = uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my) ? 1
          : battery_open && uui_hit(g.set_x, g.btn_y, g.set_w, g.btn_h, mx, my) ? 2 : 0;
    if (h != g_hover && battery_open) battery_damage();
    g_hover = h;
    return h;
}

// --- drawing ----------------------------------------------------------

void battery_draw(int mx, int my) {
    (void)mx; (void)my;
    struct battery_geom g;
    battery_geometry(&g);
    wm_flyout_card(g.x, g.y, g.w, g.h, g.foot_h);
    char title[16], sub[64];
    headline(title, sizeof title, sub, sizeof sub);
    wm_flyout_hero(g.x, g.y, g.w - g.pad, UTHEME_ACCENT, g_icon, title, sub);
    // THE CHARGE BAR: the accent, or the icon's own amber/red when low.
    struct ugfx_surface *s = wm_surface();
    uui_fill_round_rect(s, g.bar_x, g.bar_y, g.bar_w, g.bar_h, g.bar_h / 2, UTHEME_SEPARATOR);
    int fill = g_p.percent > 0 ? g.bar_w * g_p.percent / 100 : 0;
    if (fill < g.bar_h && fill > 0) fill = g.bar_h;
    if (fill > 0) uui_fill_round_rect(s, g.bar_x, g.bar_y, fill, g.bar_h, g.bar_h / 2, g_ink ? g_ink : UTHEME_ACCENT);
    wm_flyout_rule(g.x, g.rule_y, g.w);
    if (g.kv_rows) {
        int rows = g_rows;
        wm_flyout_caption(g.x + g.pad, g.cap_y, g.w - 2 * g.pad, "DETAILS");
        for (int i = 0; i < rows && i < g.kv_rows; i++)
            wm_flyout_kv(g.x + g.pad, g.kv_y + i * g.row_h + (g.row_h - ugfx_char_h()) / 2, g.w - 2 * g.pad,
                         g.kv_key_w, g_keys[i], g_vals[i]);
    }
    wm_flyout_button(g.set_x, g.btn_y, "Power settings...", 0, 1, g_hover == 2, 0);
}
