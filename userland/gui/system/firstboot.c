// First-boot setup: a full-screen wizard of the few things a new machine
// should be told before anything else -- region, keyboard, time zone,
// looks, its name, the network -- shown once, before the desktop is used.
// Never called "OOBE" here (the maintainer's word, 2026-10-11).
//
// STARTED BY INIT, NOT BY THE DESKTOP: /etc/services.d/firstboot runs
// `firstboot --boot` once toywm is ready, and --boot exits at once unless
// lib/ufbs.h says this boot needs it -- systemd-firstboot's
// ConditionFirstBoot, and macOS's .AppleSetupDone. Run with no argument
// (`fbs --now`, System Settings) it shows regardless.
//
// THE LOCK IS FULL SCREEN PLUS THE SHORTCUT INHIBITOR: the taskbar and
// Start are not drawn under a full-screen window, and Super and the
// compositor's shortcuts reach this window instead. Nothing new in the
// compositor -- and a crash leaves an ordinary desktop, not a locked one.
//
// EACH CHOICE IS SAVED AS IT IS MADE, as GNOME Initial Setup does: the
// pointer, the layout and the clock's spelling change under the person
// choosing them. The two text fields save on Next, once valid. So Skip
// keeps what was already chosen.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <locale.h>

#include "rt/sys.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uui_wizard.h"
#include "ui/uui_layout.h"
#include "ui/uui_label.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_listbox.h"
#include "ui/uui_textbox.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_segmented.h"
#include "ui/uui_switch.h"
#include "ui/uui_gallery.h"
#include "ui/uui_keymap.h"
#include "ui/uui_dialog.h"
#include "ui/uui_focus.h"
#include "ui/uui_route.h"
#include "ui/uui_tiles.h"
#include "lib/usetting.h"
#include "lib/ukeymap.h"
#include "lib/udate.h"
#include "lib/ufbs.h"
#include "lib/uhostname.h"
#include "query_abi.h"

#define SET_REGION   "locale.region"
#define SET_TIMEFMT  "locale.time_format"
#define SET_LAYOUT   "system.keyboard_layout"
#define SET_LAYOUTS  "system.keyboard_layouts"
#define SET_DEAD     "system.keyboard_dead_keys"
#define SET_TZ       "system.timezone"
#define SET_NTP      "system.ntp"
#define SET_WALL     "desktop.wallpaper"
#define SET_WALLTYPE "desktop.wallpaper_type"
#define SET_CURSOR   "system.cursor_theme"
#define SET_TASKBAR  "desktop.taskbar_theme"
#define SET_GLASS    "desktop.transparency"
#define SET_SERVER   "update.server"

enum { S_REGION, S_KEYBOARD, S_TIME, S_LOOK, S_NAME, S_NET, S_READY, S_COUNT };

enum {
    ID_REGION = 1, ID_TIMEFMT, ID_LAYOUTS, ID_TRY, ID_DEAD, ID_TZ, ID_NTP,
    ID_WALL, ID_CURSOR, ID_TASKBAR, ID_GLASS, ID_NAME, ID_SERVER, ID_DIALOG,
};

enum { ASK_BACK = 1, ASK_SKIP };

// --- one enum setting's choices -------------------------------------------

#define CHOICES_MAX 96
struct choices {
    const char *setting;
    char value[CHOICES_MAX][SETTING_ABI_VALUE_MAX];
    char label[CHOICES_MAX][SETTING_ABI_LABEL_MAX];
    const char *ptr[CHOICES_MAX];
    int count, current;
};

static void choices_load(struct choices *c, const char *setting) {
    c->setting = setting;
    c->count = usetting_choices(setting, c->value, c->label, CHOICES_MAX, &c->current);
    if (c->count < 0) c->count = 0;
    for (int i = 0; i < c->count; i++) c->ptr[i] = c->label[i];
}

static void choices_apply(const struct choices *c, int i) {
    if (i < 0 || i >= c->count) return;
    int r = usetting_set(c->setting, c->value[i]);
    ulogf("firstboot: %s=%s%s\n", c->setting, c->value[i], r > 0 ? "" : " (refused)");
}

static const char *choices_label(const struct choices *c, int i) {
    return i >= 0 && i < c->count ? c->label[i] : "unchanged";
}

static struct choices g_region, g_timefmt, g_layout, g_tz, g_ntp, g_wall, g_cursor, g_taskbar,
                      g_glass;

static struct uapp *g_app;

// --- the widgets, page by page ----------------------------------------------

static struct uui_label c_region, c_timefmt, g_sample, c_tz, g_tz_sample, c_wall, c_cursor,
                        c_taskbar, c_glass, c_name, g_name_hint, g_name_err, c_net, g_net1,
                        g_net2, c_server, g_server_hint, g_server_err, c_try;
static char g_sample_text[96], g_tz_text[96], g_net1_text[96], g_net2_text[96];

static struct uui_dropdown g_dd_region, g_dd_timefmt, g_dd_tz;
static struct uui_listbox g_lb_layout;
static struct ukeymap g_map;
static struct uui_keymap g_keymap;
static struct uui_textbox g_try, g_name, g_server;
static struct uui_checkbox g_dead, g_ntp_cb;
static struct uui_gallery g_gal_wall, g_gal_cursor;
static struct uui_segmented g_seg_taskbar;
static struct uui_switch g_sw_glass;
static struct uui_dialog g_ask;

static struct uui_cursor_tiles g_cursor_tiles;
static struct uui_picture_tiles g_wall_tiles;

#define READY_ROWS 7
static const char *const READY_LABEL[READY_ROWS] = {
    "Region & formats", "Keyboard", "Time zone", "Appearance", "Computer name", "Network",
    "System Update",
};
static char g_ready_text[READY_ROWS][160];
static struct uui_custom g_ready;

static struct uui_item P_REGION[5], P_KB_RIGHT[4], P_KB[2], P_TIME[4], P_LOOK_ROW[2],
                       P_LOOK_TB[2], P_LOOK_GL[2], P_LOOK[5], P_NAME[4], P_NET[7],
                       P_READY[1];
static struct uui_layout L_REGION, L_KB_RIGHT, L_KB, L_TIME, L_LOOK_ROW, L_LOOK_TB, L_LOOK_GL,
                         L_LOOK, L_NAME, L_NET, L_READY;

static struct uui_wizard g_wz;
static struct uui_wizard_step g_steps[S_COUNT];
static struct uui_item g_widgets[2];
static struct uui_layout g_root;
static struct uui_focusable g_focusables[12];
static struct uui_focus g_focus;

static void caption(struct uui_label *l, const char *text) {
    uui_label_init(l, text);
    l->font = ugfx_font_session(UGFX_FONT_BOLD);
}

static void hint(struct uui_label *l, const char *text) {
    uui_label_init(l, text);
    l->fg = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    uui_label_set_wrap(l, 2);
}

static struct uui_item item(const struct uui_widget_ops *ops, void *w, int id, const char *name,
                            unsigned flags) {
    return (struct uui_item){ .ops = ops, .widget = w, .id = id, .name = name, .flags = flags };
}

static struct uui_layout column(struct uui_item *items, int count) {
    return (struct uui_layout){ .dir = UUI_COLUMN, .items = items, .count = count,
                                .gap = ugfx_char_h() / 2 };
}

// --- live readouts ------------------------------------------------------------

// The clock in the locale just chosen. libc caches the zone and the
// region, so both are re-read first -- what the taskbar clock does too.
static void refresh_samples(void) {
    tzset();
    setlocale(LC_ALL, "");
    struct rtc_time t;
    sys_gettime(&t);
    char d[64];
    udate_format(d, sizeof d, &t, UDATE_DATE | UDATE_TIME | UDATE_LONG);
    snprintf(g_sample_text, sizeof g_sample_text, "Now: %s", d);
    udate_format(d, sizeof d, &t, UDATE_TIME);
    snprintf(g_tz_text, sizeof g_tz_text, "The time here: %s", d);
}

static void refresh_network(void) {
    struct query_netdev nd;
    int any = 0;
    g_net1_text[0] = g_net2_text[0] = 0;
    QUERY_FOREACH(QUERY_NETDEV, nd, i) {
        if (any++) continue;   // the first adapter speaks for the machine
        snprintf(g_net1_text, sizeof g_net1_text, "%s (%s): %s", nd.name, nd.driver,
                 nd.link_known && !nd.link_up ? "no cable" : "connected");
        if (nd.ip)
            snprintf(g_net2_text, sizeof g_net2_text, "Address %u.%u.%u.%u",
                     (unsigned)(nd.ip >> 24) & 255, (unsigned)(nd.ip >> 16) & 255,
                     (unsigned)(nd.ip >> 8) & 255, (unsigned)nd.ip & 255);
        else
            snprintf(g_net2_text, sizeof g_net2_text, "No address yet -- it is still asking");
    }
    if (!any) snprintf(g_net1_text, sizeof g_net1_text, "No network adapter was found.");
}

static void load_keymap(void) {
    int i = g_lb_layout.selected;
    ukeymap_load(&g_map, i >= 0 && i < g_layout.count ? g_layout.value[i] : "");
}

static int name_valid(void) { return uhostname_valid(uui_textbox_text(&g_name)); }

static int server_valid(void) {
    const char *s = uui_textbox_text(&g_server);
    return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8);
}

static void refresh_ready(void) {
    char tf[SETTING_ABI_LABEL_MAX];
    strlcpy(tf, choices_label(&g_timefmt, uui_dropdown_selected(&g_dd_timefmt)), sizeof tf);
    snprintf(g_ready_text[0], sizeof g_ready_text[0], "%s; times: %s",
             choices_label(&g_region, uui_dropdown_selected(&g_dd_region)), tf);
    snprintf(g_ready_text[1], sizeof g_ready_text[1], "%s; dead keys %s",
             choices_label(&g_layout, g_lb_layout.selected), g_dead.checked ? "on" : "off");
    snprintf(g_ready_text[2], sizeof g_ready_text[2], "%s; network time %s",
             choices_label(&g_tz, uui_dropdown_selected(&g_dd_tz)), g_ntp_cb.checked ? "on" : "off");
    snprintf(g_ready_text[3], sizeof g_ready_text[3], "%s wallpaper, %s pointer, %s taskbar, transparency %s",
             choices_label(&g_wall, g_gal_wall.selected), choices_label(&g_cursor, g_gal_cursor.selected),
             choices_label(&g_taskbar, g_seg_taskbar.selected), g_sw_glass.on ? "on" : "off");
    snprintf(g_ready_text[4], sizeof g_ready_text[4], "%s", uui_textbox_text(&g_name));
    snprintf(g_ready_text[5], sizeof g_ready_text[5], "%s", g_net1_text);
    snprintf(g_ready_text[6], sizeof g_ready_text[6], "%s", uui_textbox_text(&g_server));
}

// The summary: one card, a label column and a value column -- what the
// mockup's Ready step drew.
static int ready_row_h(void) { return ugfx_char_h() * 2; }

static void draw_ready(struct ugfx_surface *s, const struct uui_custom *c) {
    int r = ugfx_char_h() / 2, pad = ugfx_char_w() * 2;
    uui_fill_round_rect(s, c->x, c->y, c->w, c->h, r, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, c->x + 1, c->y + 1, c->w - 2, c->h - 2, r - 1, UTHEME_WHITE);
    int lw = ugfx_char_advance('n') * 20, rh = ready_row_h();
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    for (int i = 0; i < READY_ROWS; i++) {
        int y = c->y + i * rh;
        if (i) ugfx_fill_rect(s, c->x + pad, y, c->w - 2 * pad, 1, UTHEME_WINDOW_BG);
        int ty = y + (rh - ugfx_char_h()) / 2;
        ugfx_draw_string_clipped(s, c->x + pad, ty, lw, READY_LABEL[i], dim, UTHEME_WHITE);
        ugfx_draw_string_clipped(s, c->x + pad + lw, ty, c->w - 2 * pad - lw, g_ready_text[i],
                                 UTHEME_TEXT, UTHEME_WHITE);
    }
}

// --- the focus ring follows the page --------------------------------------------

static void focus_for(int step) {
    int n = 0;
    struct uui_focusable *f = g_focusables;
    switch (step) {
    case S_REGION:
        f[n++] = (struct uui_focusable){ &g_dd_region, &uui_dropdown_focus_ops };
        f[n++] = (struct uui_focusable){ &g_dd_timefmt, &uui_dropdown_focus_ops };
        break;
    case S_KEYBOARD:
        f[n++] = (struct uui_focusable){ &g_lb_layout, &uui_listbox_focus_ops };
        f[n++] = (struct uui_focusable){ &g_try, &uui_textbox_focus_ops };
        f[n++] = (struct uui_focusable){ &g_dead, &uui_checkbox_ops };
        break;
    case S_TIME:
        f[n++] = (struct uui_focusable){ &g_dd_tz, &uui_dropdown_focus_ops };
        f[n++] = (struct uui_focusable){ &g_ntp_cb, &uui_checkbox_ops };
        break;
    case S_LOOK:
        f[n++] = (struct uui_focusable){ &g_gal_wall, &uui_gallery_ops };
        f[n++] = (struct uui_focusable){ &g_gal_cursor, &uui_gallery_ops };
        f[n++] = (struct uui_focusable){ &g_seg_taskbar, &uui_segmented_ops };
        f[n++] = (struct uui_focusable){ &g_sw_glass, &uui_switch_ops };
        break;
    case S_NAME:
        f[n++] = (struct uui_focusable){ &g_name, &uui_textbox_focus_ops };
        break;
    case S_NET:
        f[n++] = (struct uui_focusable){ &g_server, &uui_textbox_focus_ops };
        break;
    }
    // Next first among the buttons, so Tab from the page lands on it.
    const struct uui_widget_ops *ops[3];
    void *w[3];
    int k = uui_wizard_focusables(&g_wz, ops, w, 3);
    int order[3] = { 1, 0, 2 };
    for (int j = 0; j < k; j++) {
        int o = order[j] < k ? order[j] : j;
        f[n++] = (struct uui_focusable){ w[o], ops[o] };
    }
    uui_focus_init(&g_focus, g_focusables, n);
    uui_focus_set(&g_focus, 0);
}

// --- the wizard's callbacks -------------------------------------------------------

static void on_enter(int step) {
    ulogf("firstboot: step %d\n", step);
    if (step == S_REGION || step == S_TIME) refresh_samples();
    if (step == S_NET || step == S_READY) refresh_network();
    if (step == S_READY) refresh_ready();
    focus_for(step);
    if (g_app) uapp_redraw(g_app);
}

static int can_leave(int step) {
    if (step == S_NAME) {
        g_name_err.text = name_valid() ? "" : "Use 1 to 63 letters, digits and hyphens, not starting or ending with a hyphen.";
        // Refused: the keys go back to the field that was refused.
        if (!name_valid()) { uui_focus_set(&g_focus, 0); return 0; }
        char was[UHOSTNAME_MAX + 2];
        uhostname_get(was, sizeof was);
        if (strcmp(was, uui_textbox_text(&g_name)) != 0) {
            usetting_set(UHOSTNAME_SETTING, uui_textbox_text(&g_name));
            ulogf("firstboot: %s=%s\n", UHOSTNAME_SETTING, uui_textbox_text(&g_name));
        }
    }
    if (step == S_NET) {
        g_server_err.text = server_valid() ? "" : "An address starting with http:// or https://.";
        if (!server_valid()) { uui_focus_set(&g_focus, 0); return 0; }
        char was[SETTING_ABI_VALUE_MAX] = "";
        usetting_get(SET_SERVER, was, sizeof was);
        if (strcmp(was, uui_textbox_text(&g_server)) != 0) {
            usetting_set(SET_SERVER, uui_textbox_text(&g_server));
            ulogf("firstboot: %s=%s\n", SET_SERVER, uui_textbox_text(&g_server));
        }
    }
    return 1;
}

// The startup layout list starts with the layout chosen, keeping any
// others after it -- what Super+Space walks.
static void save_layout_list(void) {
    int i = g_lb_layout.selected;
    if (i < 0 || i >= g_layout.count) return;
    char now[SETTING_ABI_VALUE_MAX] = "", list[SETTING_ABI_VALUE_MAX];
    usetting_get(SET_LAYOUTS, now, sizeof now);
    strlcpy(list, g_layout.value[i], sizeof list);
    for (char *p = now, *e; *p; p = e ? e + 1 : p + strlen(p)) {
        e = strchr(p, ',');
        if (e) *e = 0;
        if (*p && strcmp(p, g_layout.value[i]) != 0) {
            strlcat(list, ",", sizeof list);
            strlcat(list, p, sizeof list);
        }
        if (!e) break;
    }
    if (strcmp(list, now) != 0) {
        usetting_set(SET_LAYOUTS, list);
        ulogf("firstboot: %s=%s\n", SET_LAYOUTS, list);
    }
}

static void finish(const char *how) {
    save_layout_list();
    if (ufbs_mark(how) != 0) ulogf("firstboot: could not write %s\n", UFBS_MARKER);
    ulogf("firstboot: %s\n", how);
    uui_cursor_tiles_reset(&g_cursor_tiles);
    uui_picture_tiles_reset(&g_wall_tiles);
    uapp_quit(g_app, 0);
}

static void on_finish(void) { finish("finished"); }

static void ask_skip(void) {
    static const char *rows[] = {
        "What you have chosen so far is kept; everything else keeps its default.",
        "Each setting is in System Settings, and setup can be run again from System > Boot menu.",
    };
    static const struct uui_dialog_button btns[] = {
        { "Back to setup", ASK_BACK, UUI_DLG_PLAIN },
        { "Skip setup", ASK_SKIP, UUI_DLG_PRIMARY },
    };
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(g_app), uapp_height(g_app));
    uui_dialog_open(&g_ask, "Skip first-boot setup?", rows, 2, btns, 2, 1, ASK_BACK);
    uapp_redraw(g_app);
}

// --- saving what changed ------------------------------------------------------------
//
// ONE PASS OVER EVERY CONTROL, from on_widget and from on_key: comparing
// each control with what was last saved means no control's change is
// missed whichever way it arrives, and an unchanged one writes nothing.

enum { K_REGION, K_TIMEFMT, K_LAYOUT, K_DEAD, K_TZ, K_NTP, K_WALL, K_CURSOR, K_TASKBAR, K_GLASS, K_COUNT };
static int g_saved[K_COUNT];

static int control_value(int k) {
    switch (k) {
    case K_REGION:  return uui_dropdown_selected(&g_dd_region);
    case K_TIMEFMT: return uui_dropdown_selected(&g_dd_timefmt);
    case K_LAYOUT:  return g_lb_layout.selected;
    case K_DEAD:    return g_dead.checked;
    case K_TZ:      return uui_dropdown_selected(&g_dd_tz);
    case K_NTP:     return g_ntp_cb.checked;
    case K_WALL:    return g_gal_wall.selected;
    case K_CURSOR:  return g_gal_cursor.selected;
    case K_TASKBAR: return g_seg_taskbar.selected;
    case K_GLASS:   return g_sw_glass.on;
    }
    return -1;
}

static void save_control(int k, int v) {
    switch (k) {
    case K_REGION:  choices_apply(&g_region, v); refresh_samples(); break;
    case K_TIMEFMT: choices_apply(&g_timefmt, v); refresh_samples(); break;
    case K_LAYOUT:  choices_apply(&g_layout, v); load_keymap(); break;
    case K_DEAD:    usetting_set(SET_DEAD, v ? "on" : "off"); break;
    case K_TZ:      choices_apply(&g_tz, v); refresh_samples(); break;
    case K_NTP:     usetting_set(SET_NTP, v ? "on" : "off"); break;
    case K_WALL:    choices_apply(&g_wall, v); usetting_set(SET_WALLTYPE, "picture"); break;
    case K_CURSOR:  choices_apply(&g_cursor, v); break;
    case K_TASKBAR: choices_apply(&g_taskbar, v); break;
    case K_GLASS:   usetting_set(SET_GLASS, v ? "on" : "off"); break;
    }
}

static void sync_controls(void) {
    for (int k = 0; k < K_COUNT; k++) {
        int v = control_value(k);
        if (v == g_saved[k]) continue;
        g_saved[k] = v;
        save_control(k, v);
    }
}

// --- uapp -------------------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id == ID_DIALOG) {
        if (uui_dialog_take_code(&g_ask) == ASK_SKIP) finish("skipped");
    } else {
        sync_controls();
    }
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    uui_wizard_action(&g_wz, code);
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    sync_controls();
    // Re-read the fields after the ring delivered a key to one.
    if (uui_wizard_step(&g_wz) == S_NAME && name_valid()) g_name_err.text = "";
    if (uui_wizard_step(&g_wz) == S_NET && server_valid()) g_server_err.text = "";
    (void)key;
    uapp_redraw(a);
}

// A close (Alt+F4) asks, as Skip does: the machine is not set up yet.
static int on_close(struct uapp *a) {
    if (uapp_question_open(a)) return 0;
    ask_skip();
    return 0;
}

static int on_tick(struct uapp *a) {
    (void)a;
    int s = uui_wizard_step(&g_wz);
    if (s != S_REGION && s != S_TIME) return 0;
    refresh_samples();
    return 1;
}

static void build(void);

// BUILT HERE, NOT IN main(): the captions' bold face and every width taken
// from the font exist only once the session font has arrived.
static void on_open(struct uapp *a) {
    g_app = a;
    build();
    g_root.count = 1;
    uapp_set_widgets(a, g_widgets, 2);
    uapp_set_fullscreen(a, 1);
    uapp_inhibit_shortcuts(a, 1);
    on_enter(uui_wizard_step(&g_wz));
}

static void wall_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    (void)ctx;
    uui_picture_tile(s, &g_wall_tiles, i, i >= 0 && i < g_wall.count ? g_wall.value[i] : 0,
                     x, y, w, h, uui_tile_load_dir, (void *)"/usr/share/wallpapers");
}

static void cursor_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    (void)ctx;
    uui_cursor_tile(s, &g_cursor_tiles, i, i >= 0 && i < g_cursor.count ? g_cursor.value[i] : 0,
                    x, y, w, h);
}

static int on_off(const char *setting) {
    char v[SETTING_ABI_VALUE_MAX] = "";
    usetting_get(setting, v, sizeof v);
    return !strcmp(v, "on");
}

static void build(void) {
    choices_load(&g_region, SET_REGION);
    choices_load(&g_timefmt, SET_TIMEFMT);
    choices_load(&g_layout, SET_LAYOUT);
    choices_load(&g_tz, SET_TZ);
    choices_load(&g_ntp, SET_NTP);
    choices_load(&g_wall, SET_WALL);
    choices_load(&g_cursor, SET_CURSOR);
    choices_load(&g_taskbar, SET_TASKBAR);
    choices_load(&g_glass, SET_GLASS);

    int cw = ugfx_char_advance('n');

    // Region & formats
    caption(&c_region, "Region");
    uui_dropdown_init(&g_dd_region, 0, 0, cw * 36, 0, g_region.ptr, g_region.count);
    uui_dropdown_set_selected(&g_dd_region, g_region.current);
    caption(&c_timefmt, "Time of day");
    uui_dropdown_init(&g_dd_timefmt, 0, 0, cw * 36, 0, g_timefmt.ptr, g_timefmt.count);
    uui_dropdown_set_selected(&g_dd_timefmt, g_timefmt.current);
    uui_label_init(&g_sample, g_sample_text);
    P_REGION[0] = item(&uui_label_ops, &c_region, 0, "region_caption", 0);
    P_REGION[1] = item(&uui_dropdown_ops, &g_dd_region, ID_REGION, "region", 0);
    P_REGION[2] = item(&uui_label_ops, &c_timefmt, 0, "timefmt_caption", 0);
    P_REGION[3] = item(&uui_dropdown_ops, &g_dd_timefmt, ID_TIMEFMT, "timefmt", 0);
    P_REGION[4] = item(&uui_label_ops, &g_sample, 0, "sample", UUI_FILL_W);
    L_REGION = column(P_REGION, 5);

    // Keyboard
    uui_listbox_init(&g_lb_layout, 0, 0, cw * 28, ugfx_char_h() * 18, g_layout.ptr, g_layout.count);
    g_lb_layout.selected = g_layout.current;
    load_keymap();
    uui_keymap_init(&g_keymap, &g_map);
    caption(&c_try, "Try it");
    uui_textbox_init(&g_try, "");
    uui_checkbox_init(&g_dead, 0, 0, 0, "Dead keys (an accent, then a letter)", UTHEME_WINDOW_BG, UTHEME_TEXT);
    g_dead.checked = on_off(SET_DEAD);
    g_dead.accent = 1;
    P_KB_RIGHT[0] = item(&uui_keymap_ops, &g_keymap, 0, "keymap", UUI_FILL_W);
    P_KB_RIGHT[1] = item(&uui_label_ops, &c_try, 0, "try_caption", 0);
    P_KB_RIGHT[2] = item(&uui_textbox_ops, &g_try, ID_TRY, "try", UUI_FILL_W);
    P_KB_RIGHT[3] = item(&uui_checkbox_ops, &g_dead, ID_DEAD, "dead", 0);
    L_KB_RIGHT = column(P_KB_RIGHT, 4);
    P_KB[0] = item(&uui_listbox_ops, &g_lb_layout, ID_LAYOUTS, "layouts", UUI_FILL_H);
    P_KB[1] = item(&uui_layout_ops, &L_KB_RIGHT, 0, 0, UUI_FILL_W);
    L_KB = (struct uui_layout){ .dir = UUI_ROW, .items = P_KB, .count = 2, .gap = cw * 3 };

    // Time zone
    caption(&c_tz, "Time zone");
    uui_dropdown_init(&g_dd_tz, 0, 0, cw * 36, 0, g_tz.ptr, g_tz.count);
    uui_dropdown_set_selected(&g_dd_tz, g_tz.current);
    uui_label_init(&g_tz_sample, g_tz_text);
    uui_checkbox_init(&g_ntp_cb, 0, 0, 0, "Set the clock from the internet (the time server in System Settings)",
                      UTHEME_WINDOW_BG, UTHEME_TEXT);
    g_ntp_cb.checked = on_off(SET_NTP);
    g_ntp_cb.accent = 1;
    P_TIME[0] = item(&uui_label_ops, &c_tz, 0, "tz_caption", 0);
    P_TIME[1] = item(&uui_dropdown_ops, &g_dd_tz, ID_TZ, "tz", 0);
    P_TIME[2] = item(&uui_label_ops, &g_tz_sample, 0, "tz_sample", UUI_FILL_W);
    P_TIME[3] = item(&uui_checkbox_ops, &g_ntp_cb, ID_NTP, "ntp", 0);
    L_TIME = column(P_TIME, 4);

    // Appearance
    caption(&c_wall, "Wallpaper");
    uui_gallery_init(&g_gal_wall, g_wall.ptr, g_wall.count, g_wall.current);
    g_gal_wall.draw_tile = wall_tile;
    g_gal_wall.min_w = cw * 16;
    caption(&c_cursor, "Pointer");
    uui_gallery_init(&g_gal_cursor, g_cursor.ptr, g_cursor.count, g_cursor.current);
    g_gal_cursor.draw_tile = cursor_tile;
    g_gal_cursor.min_w = cw * 16;
    caption(&c_taskbar, "Taskbar");
    uui_segmented_init(&g_seg_taskbar, g_taskbar.ptr, g_taskbar.count, g_taskbar.current);
    caption(&c_glass, "Transparency");
    uui_switch_init(&g_sw_glass, on_off(SET_GLASS));
    P_LOOK_TB[0] = item(&uui_label_ops, &c_taskbar, 0, "taskbar_caption", 0);
    P_LOOK_TB[1] = item(&uui_segmented_ops, &g_seg_taskbar, ID_TASKBAR, "taskbar", 0);
    L_LOOK_TB = column(P_LOOK_TB, 2);
    P_LOOK_GL[0] = item(&uui_label_ops, &c_glass, 0, "glass_caption", 0);
    P_LOOK_GL[1] = item(&uui_switch_ops, &g_sw_glass, ID_GLASS, "glass", 0);
    L_LOOK_GL = column(P_LOOK_GL, 2);
    P_LOOK_ROW[0] = item(&uui_layout_ops, &L_LOOK_TB, 0, 0, 0);
    P_LOOK_ROW[1] = item(&uui_layout_ops, &L_LOOK_GL, 0, 0, 0);
    L_LOOK_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = P_LOOK_ROW, .count = 2, .gap = cw * 6 };
    P_LOOK[0] = item(&uui_label_ops, &c_wall, 0, "wall_caption", 0);
    P_LOOK[1] = item(&uui_gallery_ops, &g_gal_wall, ID_WALL, "wallpaper", UUI_FILL_W);
    P_LOOK[2] = item(&uui_label_ops, &c_cursor, 0, "cursor_caption", 0);
    P_LOOK[3] = item(&uui_gallery_ops, &g_gal_cursor, ID_CURSOR, "cursor", UUI_FILL_W);
    P_LOOK[4] = item(&uui_layout_ops, &L_LOOK_ROW, 0, 0, 0);
    L_LOOK = column(P_LOOK, 5);

    // Computer name
    char host[UHOSTNAME_MAX + 2];
    uhostname_get(host, sizeof host);
    caption(&c_name, "Computer name");
    uui_textbox_init(&g_name, host);
    hint(&g_name_hint, "Shown in About, and to a remote-desktop viewer. Letters, digits and hyphens, up to 63.");
    uui_label_init(&g_name_err, "");
    g_name_err.fg = utheme_severity(UTHEME_SEV_ERROR);
    P_NAME[0] = item(&uui_label_ops, &c_name, 0, "name_caption", 0);
    P_NAME[1] = item(&uui_textbox_ops, &g_name, ID_NAME, "name", 0);
    P_NAME[2] = item(&uui_label_ops, &g_name_hint, 0, "name_hint", UUI_FILL_W);
    P_NAME[3] = item(&uui_label_ops, &g_name_err, 0, "name_error", UUI_FILL_W);
    L_NAME = column(P_NAME, 4);
    P_NAME[1].main_size = 0;
    g_name.w = cw * 36;

    // Network
    char server[SETTING_ABI_VALUE_MAX] = "";
    usetting_get(SET_SERVER, server, sizeof server);
    caption(&c_net, "Wired network");
    uui_label_init(&g_net1, g_net1_text);
    uui_label_init(&g_net2, g_net2_text);
    caption(&c_server, "System Update server");
    uui_textbox_init(&g_server, server);
    hint(&g_server_hint, "Where System Update looks for new versions of toy-os.");
    uui_label_init(&g_server_err, "");
    g_server_err.fg = utheme_severity(UTHEME_SEV_ERROR);
    P_NET[0] = item(&uui_label_ops, &c_net, 0, "net_caption", 0);
    P_NET[1] = item(&uui_label_ops, &g_net1, 0, "net_status", UUI_FILL_W);
    P_NET[2] = item(&uui_label_ops, &g_net2, 0, "net_address", UUI_FILL_W);
    P_NET[3] = item(&uui_label_ops, &c_server, 0, "server_caption", 0);
    P_NET[4] = item(&uui_textbox_ops, &g_server, ID_SERVER, "server", UUI_FILL_W);
    P_NET[5] = item(&uui_label_ops, &g_server_hint, 0, "server_hint", UUI_FILL_W);
    P_NET[6] = item(&uui_label_ops, &g_server_err, 0, "server_error", UUI_FILL_W);
    L_NET = column(P_NET, 7);

    // Ready
    g_ready = (struct uui_custom){ .w = cw * 70, .h = ready_row_h() * READY_ROWS, .draw = draw_ready };
    P_READY[0] = item(&uui_custom_ops, &g_ready, 0, "summary", UUI_FILL_W);
    L_READY = column(P_READY, 1);

    static const struct { const char *title, *note; struct uui_layout *page; } STEPS[S_COUNT] = {
        { "Region & formats", "How dates, times and numbers are written.", &L_REGION },
        { "Keyboard", "The layout printed on your keyboard. More can be added later, and Super+Space switches between them.", &L_KB },
        { "Time zone", "Where this machine is, for the clock and the calendar.", &L_TIME },
        { "Appearance", "Each of these is in System Settings > Desktop as well.", &L_LOOK },
        { "Computer name", "What this computer is called.", &L_NAME },
        { "Network", "How this machine reaches the network, and where its updates come from.", &L_NET },
        { "Ready", "These are saved. Each one is also in System Settings.", &L_READY },
    };
    for (int i = 0; i < S_COUNT; i++)
        g_steps[i] = (struct uui_wizard_step){ STEPS[i].title, STEPS[i].note,
                                               item(&uui_layout_ops, STEPS[i].page, 0, 0, 0) };

    uui_dialog_init(&g_ask);
    struct uui_wizard_desc d = {
        .heading = "Set up toy-os",
        .steps = g_steps,
        .count = S_COUNT,
        .skip = "Skip setup -- keep the defaults",
        .finish = "Start using toy-os",
        .can_leave = can_leave,
        .on_enter = on_enter,
        .on_finish = on_finish,
        .on_skip = ask_skip,
    };
    uui_wizard_init(&g_wz, &d);
    for (int k = 0; k < K_COUNT; k++) g_saved[k] = control_value(k);
    g_widgets[0] = *uui_wizard_item(&g_wz);
    g_widgets[1] = item(&uui_dialog_ops, &g_ask, ID_DIALOG, "dialog", 0);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--boot") && !ufbs_needed()) return 0;
    // Empty until on_open fills it.
    g_root = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_widgets, .count = 0, .margin = 1 };
    struct uapp_desc desc = {
        .title        = "First-boot setup",
        .app_id       = "firstboot",
        .layout       = &g_root,
        .w            = 640,
        .h            = 480,
        .focus        = &g_focus,
        .flags        = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .tick_ms      = 1000,
        .on_open      = on_open,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
        .on_close     = on_close,
        .on_tick      = on_tick,
    };
    return uapp_run(&desc);
}
