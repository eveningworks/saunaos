// Web Browser -- NetSurf's engine in a toy-os window (docs/browser-design.md,
// stage 3).
//
// W1 from the design canvas: tabs in the title bar (the compositor's,
// as Files has them), a toolbar row -- back, forward, reload, home, the
// address field, then the right-hand buttons -- and the page.
//
// THE ENGINE IS NetSurf, LINKED INTO THIS ONE BINARY (GPL-2.0, as DOOM
// is; LICENSE says why that is an aggregation). Everything between it and
// the toolkit is in userland/browser/: fonts, plotters, bitmaps, the
// scheduler, and the page widget, which this file only places.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "browser/ns_bitmap.h"
#include "browser/ns_font.h"
#include "browser/ns_sched.h"
#include "browser/ns_view.h"

#include "netsurf/browser.h"
#include "netsurf/browser_window.h"
#include "netsurf/netsurf.h"
#include "desktop/browser_history.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"

#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_focus.h"
#include "ui/uui_textbox.h"
#include "ui/uui_toolbar.h"
#include "ui/ulog.h"
#include "win_proto.h"   // WIN_TAB_*, WIN_TABS_MAX, WIN_TITLE_LEN

#define HOME_URL "file:///usr/share/netsurf/start.html"
#define MESSAGES "/usr/share/netsurf/Messages"

enum { ID_TOOLS = 1, ID_ADDRESS, ID_TOOLS_R, ID_PAGE };
enum { CMD_BACK = 1, CMD_FORWARD, CMD_RELOAD, CMD_HOME, CMD_DOWNLOADS, CMD_MENU };

static struct uapp *g_app;

// --- tabs: one NetSurf window each ------------------------------------------

static struct gui_window *g_tabs[WIN_TABS_MAX];
static int g_ntabs, g_cur = -1;
static char g_label[WIN_TABS_MAX][WIN_TITLE_LEN];

struct gui_window *nsapp_active(void) { return g_cur >= 0 ? g_tabs[g_cur] : NULL; }

static struct ns_view g_view;
struct ns_view *nsapp_view(void) { return &g_view; }

void nsapp_redraw(void) {
    if (g_app) uapp_redraw(g_app);
}

static const char *tab_name(const struct gui_window *gw) {
    if (gw->title[0]) return gw->title;
    if (gw->url[0]) return gw->url;
    return "New tab";
}

static void sync_tabs(void) {
    if (!g_app || !g_ntabs) return;
    const char *labels[WIN_TABS_MAX];
    for (int i = 0; i < g_ntabs; i++) {
        strlcpy(g_label[i], tab_name(g_tabs[i]), sizeof g_label[i]);
        labels[i] = g_label[i];
    }
    uapp_set_tabs(g_app, labels, g_ntabs, g_cur);
    char title[WIN_TITLE_LEN + 16];
    snprintf(title, sizeof title, "%s - Web Browser", tab_name(g_tabs[g_cur]));
    uapp_set_title(g_app, title);
}

// --- the toolbar and the address field ---------------------------------------

static const struct uui_toolbar_item TOOLS[] = {
    { "tb-back",    "Back (Alt+Left)",     CMD_BACK, 0, 0, 0, 0 },
    { "tb-forward", "Forward (Alt+Right)", CMD_FORWARD, 0, 0, 0, 0 },
    { "tb-refresh", "Reload (F5)",         CMD_RELOAD, 0, 0, 0, 0 },
    { "tb-home",    "Home (Alt+Home)",     CMD_HOME, 0, 0, 0, 0 },
};
// The loading state's swap: Stop where Reload was, as every browser.
static const struct uui_toolbar_item TOOLS_LOADING[] = {
    { "tb-back",    "Back (Alt+Left)",     CMD_BACK, 0, 0, 0, 0 },
    { "tb-forward", "Forward (Alt+Right)", CMD_FORWARD, 0, 0, 0, 0 },
    { "tb-stop",    "Stop (Esc)",          CMD_RELOAD, 0, 0, 0, 0 },
    { "tb-home",    "Home (Alt+Home)",     CMD_HOME, 0, 0, 0, 0 },
};
static const struct uui_toolbar_item TOOLS_R[] = {
    { "tb-down", "Downloads (not yet)", CMD_DOWNLOADS, 0, 0, 0, 0 },
    { "tb-menu", "Menu (not yet)",      CMD_MENU, 0, 0, 0, 0 },
};
static struct uui_toolbar g_tools, g_tools_r;
static struct uui_textbox g_address;
static int g_shown_loading = -1;

static unsigned tool_flags(int code) {
    struct gui_window *gw = nsapp_active();
    if (!gw) return UUI_MI_DISABLED;
    switch (code) {
    case CMD_BACK:      return browser_window_back_available(gw->bw) ? 0 : UUI_MI_DISABLED;
    case CMD_FORWARD:   return browser_window_forward_available(gw->bw) ? 0 : UUI_MI_DISABLED;
    case CMD_DOWNLOADS:
    case CMD_MENU:      return UUI_MI_DISABLED;   // stage 4
    }
    return 0;
}

static void sync_chrome(void) {
    struct gui_window *gw = nsapp_active();
    int loading = gw && gw->loading;
    if (loading != g_shown_loading) {
        g_shown_loading = loading;
        const struct uui_toolbar_item *items = loading ? TOOLS_LOADING : TOOLS;
        g_tools.items = items;
    }
    // The address shows the page's -- unless the person is typing in it.
    if (gw && !g_address.active) {
        uui_textbox_set_text(&g_address, gw->url);
    }
}

void nsapp_window_changed(struct gui_window *gw) {
    if (gw == nsapp_active()) sync_chrome();
    sync_tabs();
    nsapp_redraw();
}

// --- navigation ---------------------------------------------------------------

// What was typed, as a URL: a path is a file, a word with a dot is a
// host, and anything with a scheme is taken as it is.
static void fix_url(const char *in, char *out, size_t cap) {
    while (*in == ' ') in++;
    if (strstr(in, "://") || !strncmp(in, "about:", 6) || !strncmp(in, "data:", 5) ||
        !strncmp(in, "resource:", 9))
        strlcpy(out, in, cap);
    else if (in[0] == '/')
        snprintf(out, cap, "file://%s", in);
    else
        snprintf(out, cap, "http://%s", in);
}

static void go(const char *text) {
    struct gui_window *gw = nsapp_active();
    static char url[NS_URL_MAX];   // static: past the ring-3 frame budget
    struct nsurl *u;
    fix_url(text, url, sizeof url);
    if (nsurl_create(url, &u) != NSERROR_OK) return;
    if (gw) browser_window_navigate(gw->bw, u, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
    nsurl_unref(u);
}

static void open_tab(const char *url) {
    struct nsurl *u;
    if (g_ntabs >= WIN_TABS_MAX) return;
    if (nsurl_create(url, &u) != NSERROR_OK) return;
    browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND | BW_CREATE_TAB, u, NULL,
                          NULL, NULL);
    nsurl_unref(u);
}

static void select_tab(int i) {
    if (i < 0 || i >= g_ntabs) return;
    g_cur = i;
    g_shown_loading = -1;
    g_address.active = 0;
    sync_chrome();
    sync_tabs();
    // A tab laid out at another window size reflows to this one.
    browser_window_schedule_reformat(g_tabs[i]->bw);
    nsapp_redraw();
}

void nsapp_window_added(struct gui_window *gw, struct gui_window *existing, int foreground) {
    (void)existing;
    if (g_ntabs >= WIN_TABS_MAX) return;
    g_tabs[g_ntabs++] = gw;
    if (foreground || g_cur < 0) select_tab(g_ntabs - 1);
    else sync_tabs();
}

void nsapp_window_removed(struct gui_window *gw) {
    int i = 0;
    while (i < g_ntabs && g_tabs[i] != gw) i++;
    if (i == g_ntabs) return;
    memmove(&g_tabs[i], &g_tabs[i + 1], (size_t)(g_ntabs - i - 1) * sizeof g_tabs[0]);
    g_ntabs--;
    if (g_cur >= g_ntabs) g_cur = g_ntabs - 1;
    if (g_ntabs) select_tab(g_cur);
}

static void close_tab(int i) {
    if (i < 0 || i >= g_ntabs) return;
    if (g_ntabs == 1) {               // the last tab: the window goes with it
        uapp_quit(g_app, 0);
        return;
    }
    browser_window_destroy(g_tabs[i]->bw);   // calls back into nsapp_window_removed()
}

static void command(int code) {
    struct gui_window *gw = nsapp_active();
    if (!gw) return;
    switch (code) {
    case CMD_BACK:    browser_window_history_back(gw->bw, false); break;
    case CMD_FORWARD: browser_window_history_forward(gw->bw, false); break;
    case CMD_RELOAD:
        if (gw->loading) browser_window_stop(gw->bw);
        else browser_window_reload(gw->bw, true);
        break;
    case CMD_HOME:    go(HOME_URL); break;
    }
}

// --- layout -----------------------------------------------------------------

static struct uui_item ROW[3], ITEMS[2];
static struct uui_layout ROW_L, ROOT_L;

// The address first in Tab order; the page has the keyboard on open.
static struct uui_focusable g_focusables[] = {
    { .widget = &g_address, .ops = &uui_textbox_ops },
    { .widget = &g_view,    .ops = &ns_view_ops },
};
static struct uui_focus g_focus;

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    int code = 0;
    if (id == ID_TOOLS) code = uui_toolbar_take_code(&g_tools);
    else if (id == ID_TOOLS_R) code = uui_toolbar_take_code(&g_tools_r);
    if (code > 0) command(code);
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    struct gui_window *gw = nsapp_active();
    int ctrl = (mods & KEY_MOD_CTRL) != 0;
    if (mods & KEY_MOD_ALT) {
        if (key == KEY_ARROW_LEFT) command(CMD_BACK);
        else if (key == KEY_ARROW_RIGHT) command(CMD_FORWARD);
        else if (key == KEY_HOME) command(CMD_HOME);
        else return;
    } else if (key == KEY_F5 || (ctrl && key == 0x12)) {          // F5, Ctrl+R
        if (gw) browser_window_reload(gw->bw, true);
    } else if (ctrl && key == 0x0C) {                             // Ctrl+L
        uui_focus_set(&g_focus, 0);
        uui_textbox_select(&g_address, 0, (int)strlen(uui_textbox_text(&g_address)));
    } else if (ctrl && key == 0x14) {                             // Ctrl+T
        open_tab(HOME_URL);
    } else if (ctrl && key == 0x17) {                             // Ctrl+W
        close_tab(g_cur);
    } else if (g_focus.current == 0 && (key == '\n' || key == '\r')) {
        go(uui_textbox_text(&g_address));
        g_address.active = 0;
        uui_focus_set(&g_focus, 1);
    } else if (g_focus.current == 0 && key == 0x1B) {             // Esc: undo the typing
        g_address.active = 0;
        sync_chrome();
        uui_focus_set(&g_focus, 1);
    } else if (gw && gw->loading && key == 0x1B) {
        browser_window_stop(gw->bw);
    } else {
        return;
    }
    uapp_redraw(a);
}

static void on_tab(struct uapp *a, int tab, int action) {
    (void)a;
    if (action == WIN_TAB_SELECT) select_tab(tab);
    else if (action == WIN_TAB_CLOSE) close_tab(tab);
    else if (action == WIN_TAB_NEW) open_tab(HOME_URL);
}

// NetSurf's timers run here, and the tick is re-armed for the next one:
// the app sleeps until NetSurf has something to do, not on a fixed beat.
static int on_tick(struct uapp *a) {
    int due = ns_sched_run();
    uapp_set_tick(a, due < 0 ? 500 : due < 5 ? 5 : (unsigned)due);
    int r = uui_toolbar_tick(&g_tools) | uui_toolbar_tick(&g_tools_r);
    return r;
}

static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    uapp_log_layout(a, "browser");
    struct gui_window *gw = nsapp_active();
    if (gw)
        uapp_logf_layout("browser: tab %d/%d loading %d extent %d %d scroll %d %d url %s\n",
                         g_cur, g_ntabs, gw->loading, gw->ext_w, gw->ext_h, gw->sx, gw->sy, gw->url);
}

static void on_size(int *w, int *h) {
    *w = ugfx_char_advance('n') * 110;
    *h = ugfx_char_h() * 40;
}

static void on_open(struct uapp *a) {
    g_app = a;
    struct nsurl *u;
    if (nsurl_create(HOME_URL, &u) == NSERROR_OK) {
        browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND, u, NULL, NULL, NULL);
        nsurl_unref(u);
    }
    uui_focus_set(&g_focus, 1);
}

static int on_close(struct uapp *a) {
    (void)a;
    netsurf_exit();
    return 1;
}

// --- NetSurf ----------------------------------------------------------------

static nserror set_defaults(struct nsoption_s *defaults) {
    (void)defaults;
    // No cookie or history file yet: where they live is an open question
    // of the plan (docs/browser-design.md).
    nsoption_setnull_charp(cookie_file, strdup(""));
    nsoption_setnull_charp(cookie_jar, strdup(""));
    nsoption_setnull_charp(url_file, strdup(""));
    return NSERROR_OK;
}

static int engine_init(int argc, char **argv) {
    // STATIC: netsurf_register() keeps the POINTER. A local here died
    // when this function returned, and NetSurf then called through the
    // stack that replaced it.
    static struct netsurf_table table = {
        .misc = NULL,   // filled below: the tables are pointers, not constants
    };
    table.misc = ns_misc_table;
    table.window = ns_window_table;
    table.fetch = ns_fetch_table;
    table.bitmap = ns_bitmap_table;
    table.layout = ns_layout_table;
    if (netsurf_register(&table) != NSERROR_OK) return 0;
    ns_bitmap_set_format();
    if (nsoption_init(set_defaults, &nsoptions, &nsoptions_default) != NSERROR_OK) return 0;
    nsoption_commandline(&argc, argv, nsoptions);
    if (messages_add_from_file(MESSAGES) != NSERROR_OK) ulogf("browser: no %s\n", MESSAGES);
    if (netsurf_init(NULL) != NSERROR_OK) return 0;
    browser_set_dpi(96);
    return 1;
}

int main(int argc, char **argv) {
    if (!ugfx_font_init()) return 2;
    if (!ns_font_init()) ulogf("browser: no page fonts -- text draws as boxes\n");
    if (!engine_init(argc, argv)) {
        ulogf("browser: NetSurf failed to start\n");
        return 1;
    }

    uui_toolbar_init(&g_tools, TOOLS, (int)(sizeof TOOLS / sizeof TOOLS[0]));
    g_tools.item_flags = tool_flags;
    uui_toolbar_init(&g_tools_r, TOOLS_R, (int)(sizeof TOOLS_R / sizeof TOOLS_R[0]));
    g_tools_r.item_flags = tool_flags;
    uui_textbox_init(&g_address, "");
    g_address.placeholder = "Type a web address or a path";
    ns_view_init(&g_view);
    uui_focus_init(&g_focus, g_focusables, (int)(sizeof g_focusables / sizeof g_focusables[0]));

    ROW[0] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &g_tools, .id = ID_TOOLS,
                                .name = "tools" };
    ROW[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_address, .id = ID_ADDRESS,
                                .name = "address", .flags = UUI_FILL_W };
    ROW[2] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &g_tools_r, .id = ID_TOOLS_R,
                                .name = "tools-right" };
    ROW_L = (struct uui_layout){ .dir = UUI_ROW, .items = ROW, .count = 3, .gap = 4, .margin = 3 };
    ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &ROW_L, .flags = UUI_FILL_W };
    ITEMS[1] = (struct uui_item){ .ops = &ns_view_ops, .widget = &g_view, .id = ID_PAGE,
                                  .name = "page", .flags = UUI_FILL_W | UUI_FILL_H };
    // Edge to edge: a margin of 0 means "the default", a character cell,
    // which framed the page in panel grey -- 1 is the least there is.
    ROOT_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS, .count = 2, .margin = 1 };
    static struct uui_item root = { .ops = &uui_layout_ops, .widget = &ROOT_L };

    struct uapp_desc desc = {
        .title        = "Web Browser",
        .app_id       = "browser",
        .on_size      = on_size,
        .layout       = &ROOT_L,
        .widgets      = &root,
        .widget_count = 1,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_tab       = on_tab,
        .on_draw_over = on_draw_over,
        .on_tick      = on_tick,
        .tick_ms      = 10,
        .on_open      = on_open,
        .on_close     = on_close,
        .focus        = &g_focus,
        .flags        = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
