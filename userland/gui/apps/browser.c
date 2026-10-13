// Web Browser -- NetSurf's engine in a toy-os window (docs/browser-design.md,
// stages 3 and 4).
//
// W1 from the design canvas: tabs in the title bar (the compositor's,
// as Files has them), a toolbar row -- back, forward, reload, home, the
// address field, then the right-hand buttons -- and the page.
//
// THE ENGINE IS NetSurf, LINKED INTO THIS ONE BINARY (GPL-2.0, as DOOM
// is; LICENSE says why that is an aggregation, and why /lib/libssl.so is
// taken here under mbedTLS's GPL option). Everything between it and the
// toolkit is in userland/browser/: fonts, plotters, bitmaps, the
// scheduler, and the page widget, which this file only places. http: and
// https: are userland/backends/netsurf/fetch_http.c, over libhttp.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "utils/errors.h"
#include "browser/ns_addr.h"
#include "browser/ns_bitmap.h"
#include "browser/ns_card.h"
#include "browser/ns_dlpanel.h"
#include "browser/ns_download.h"
#include "browser/ns_find.h"
#include "browser/ns_font.h"
#include "browser/ns_marks.h"
#include "browser/ns_sched.h"
#include "browser/ns_site.h"
#include "browser/ns_view.h"

#include "netsurf/browser.h"
#include "netsurf/browser_window.h"
#include "netsurf/netsurf.h"
#include "netsurf/url_db.h"
#include "netsurf/cookie_db.h"
#include "content/fetch.h"
#include "desktop/browser_history.h"
#include "desktop/version.h"
#include "backends/netsurf/fetch_http.h"
#include "backends/netsurf/image_uimg.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"

#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_entrylist.h"
#include "ui/uui_findbar.h"
#include "ui/uui_focus.h"
#include "ui/uui_menubar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_textbox.h"
#include "ui/uui_toolbar.h"
#include "ui/ulog.h"
#include "lib/usetting.h"
#include "win_proto.h"   // WIN_TAB_*, WIN_TABS_MAX, WIN_TITLE_LEN

#define HOME_URL "file:///usr/share/netsurf/start.html"
#define MESSAGES "/usr/share/netsurf/Messages"
// What the browser KEEPS: the cookie jar and the global history, in
// /var/lib like every app's kept state (docs/filesystem-layout.md). The
// cache stays in memory -- NetSurf's disk cache is not built here.
#define STATE_DIR    "/var/lib/browser"
#define COOKIES_FILE STATE_DIR "/cookies"
#define HISTORY_FILE STATE_DIR "/history"
#define BOOKMARKS_FILE STATE_DIR "/bookmarks"

enum { ID_TOOLS = 1, ID_ADDRESS, ID_TOOLS_R, ID_PAGE, ID_CTX, ID_FIND, ID_ADDRBAR, ID_CARD,
       ID_DLPANEL, ID_DLLIST, ID_SIDE_TABS, ID_SIDE_SEARCH, ID_SIDE_LIST };
enum { CMD_BACK = 1, CMD_FORWARD, CMD_RELOAD, CMD_HOME, CMD_DOWNLOADS, CMD_MENU,
       CMD_NEWTAB, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET, CMD_FIND,
       CMD_BOOKMARK, CMD_SIDE_BOOKMARKS, CMD_SIDE_HISTORY };

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
    { "tb-down", "Downloads (Ctrl+J)",  CMD_DOWNLOADS, 0, 0, 0, 0 },
    { "tb-menu", "Menu",                CMD_MENU, 0, 0, 0, 0 },
};
static struct uui_toolbar g_tools, g_tools_r;
static struct uui_textbox g_address;
static int g_shown_loading = -1;

// The address field's frame (S1): the security chip, the field, the zoom
// badge -- and the lock's card (P1), drawn over the page.
static struct ns_addr g_addr;
static struct ns_card g_card;
// The Downloads flyout (D1), under the down arrow.
static struct ns_dlpanel g_dlp;

// Bookmarks and history (H1): a sidebar left of the page -- which list,
// a search field, and the entries.
static const char *const SIDE_TABS[] = { "Bookmarks", "History" };
static struct uui_segmented g_side_tabs;
static struct uui_textbox g_side_search;
static struct uui_entrylist g_side_list;
static struct uui_entry g_side_rows[NS_MARKS_MAX];
static int g_side_open;

// The ☰ menu: a menu with no bar strip, opened under its button (KDE's
// ☰, Terminal's). Built as it opens, so Reset zoom can name the level.
static struct uui_menubar g_ctx;
static struct uui_menu_item g_menu_items[16];
static char g_reset_label[32];

// Find in page floats at the page's top right, as Notepad's and
// Terminal's do. While it has the keyboard the focus ring holds nothing,
// so every key reaches on_key and goes to the bar.
static struct uui_findbar g_find;
static int g_find_open;
static struct uui_focus g_focus;

static unsigned tool_flags(int code) {
    struct gui_window *gw = nsapp_active();
    if (!gw) return UUI_MI_DISABLED;
    switch (code) {
    case CMD_BACK:      return browser_window_back_available(gw->bw) ? 0 : UUI_MI_DISABLED;
    case CMD_FORWARD:   return browser_window_forward_available(gw->bw) ? 0 : UUI_MI_DISABLED;
    case CMD_DOWNLOADS: return g_dlp.open ? UUI_MI_CHECKED : 0;
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
    int state = ns_site_state(gw), zoom = ns_view_zoom_pct(gw);
    int starred = gw && ns_bookmarked(gw->url);
    if (state != g_addr.state || zoom != g_addr.zoom || starred != g_addr.starred) {
        g_addr.state = state;
        g_addr.zoom = zoom;
        g_addr.starred = starred;
        ns_addr_sync(&g_addr);
        ulogf("browser: site %d zoom %d\n", state, zoom);
    }
}

static void side_refresh(void);

void nsapp_window_changed(struct gui_window *gw) {
    static int was_loading;
    if (gw == nsapp_active()) {
        sync_chrome();
        // A page that finished loading is the newest in History.
        if (was_loading && !gw->loading && g_side_open && g_side_tabs.selected == NS_MARKS_HISTORY)
            side_refresh();
        was_loading = gw->loading;
    }
    sync_tabs();
    nsapp_redraw();
}

// --- navigation ---------------------------------------------------------------

// The User-Agent, from browser.user_agent -- read at each navigation, so
// a change in System Settings applies to the next page loaded.
static void apply_user_agent(void) {
    char which[16], ua[256];
    if (usetting_get("browser.user_agent", which, sizeof which) < 0) strcpy(which, "toyos");
    if (!strcmp(which, "custom") &&
        usetting_get("browser.user_agent_custom", ua, sizeof ua) >= 0 && ua[0]) {
        ns_fetch_http_set_user_agent(ua);
        return;
    }
    snprintf(ua, sizeof ua, "Mozilla/5.0 (%s) NetSurf/%d.%d",
             strcmp(which, "linux") ? "toy-os; x86_64" : "X11; Linux x86_64",
             netsurf_version_major, netsurf_version_minor);
    ns_fetch_http_set_user_agent(ua);
}

// Does typed text look like an address rather than words to search for?
// Chrome's omnibox test, cut down: no spaces, and a dot, a colon (a
// port) or "localhost" in it.
static int looks_like_host(const char *in) {
    if (strchr(in, ' ') || !in[0]) return 0;
    return strchr(in, '.') || strchr(in, ':') || !strncmp(in, "localhost", 9);
}

// Is the host part a dotted quad, or localhost? Those are reached over
// http, as Chrome leaves them out of its https upgrade: a machine on the
// LAN almost never has a certificate.
static int local_host(const char *in) {
    if (!strncmp(in, "localhost", 9)) return 1;
    int dots = 0;
    for (; *in && *in != '/' && *in != ':'; in++) {
        if (*in == '.') dots++;
        else if (*in < '0' || *in > '9') return 0;
    }
    return dots == 3;
}

// Where words typed in the address field are searched for: DuckDuckGo's
// page that needs no JavaScript, NetSurf's own default engine. `%s` is
// the words. `--search-url` replaces it, so a test can search a server
// on this machine instead of the internet.
static const char *g_search = "https://html.duckduckgo.com/html/?q=%s";

// The search URL for `words`: the template with them form-encoded
// (letters, digits and -._~ kept, a space as '+', the rest %XX).
static void search_url(const char *words, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const char *t = g_search; *t && n + 4 < cap; t++) {
        if (t[0] != '%' || t[1] != 's') { out[n++] = *t; continue; }
        for (const unsigned char *w = (const unsigned char *)words; *w && n + 4 < cap; w++) {
            if ((*w >= 'a' && *w <= 'z') || (*w >= 'A' && *w <= 'Z') || (*w >= '0' && *w <= '9') ||
                strchr("-._~", *w)) {
                out[n++] = (char)*w;
            } else if (*w == ' ') {
                out[n++] = '+';
            } else {
                out[n++] = '%';
                out[n++] = hex[*w >> 4];
                out[n++] = hex[*w & 15];
            }
        }
        t++;
    }
    out[n] = 0;
}

// What was typed, as a URL: a path is a file, anything with a scheme is
// taken as it is, a host is https (http for a local one or an explicit
// port other than 443 -- libhttp's own guess), and anything else is a
// search. 0, or -1 if it is nothing a URL can be made of.
static int typed_url(const char *in, struct nsurl **u) {
    static char url[NS_URL_MAX];   // static: past the ring-3 frame budget
    while (*in == ' ') in++;
    if (strstr(in, "://") || !strncmp(in, "about:", 6) || !strncmp(in, "data:", 5) ||
        !strncmp(in, "resource:", 9)) {
        strlcpy(url, in, sizeof url);
    } else if (in[0] == '/') {
        snprintf(url, sizeof url, "file://%s", in);
    } else if (looks_like_host(in)) {
        const char *colon = strchr(in, ':');
        int odd_port = colon && strncmp(colon, ":443", 4) != 0;
        snprintf(url, sizeof url, "%s://%s", local_host(in) || odd_port ? "http" : "https", in);
    } else {
        search_url(in, url, sizeof url);
    }
    return nsurl_create(url, u) == NSERROR_OK ? 0 : -1;
}

static void go(const char *text) {
    struct gui_window *gw = nsapp_active();
    struct nsurl *u;
    if (typed_url(text, &u) != 0) return;
    apply_user_agent();
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

static void find_close(void);

static void select_tab(int i) {
    if (i < 0 || i >= g_ntabs) return;
    if (g_find_open && i != g_cur) find_close();
    ns_card_close(&g_card);
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

// --- find in page and zoom ----------------------------------------------------

static void find_report(void) {
    g_find.matches = ns_find_matches();
    g_find.current = ns_find_current();
    ulogf("browser: find %d of %d\n", g_find.current, g_find.matches);
}

static void find_run(int dir) {
    ns_find(nsapp_active(), uui_findbar_query(&g_find), dir);
    find_report();
}

static void place_find(void) {
    struct ns_view *v = nsapp_view();
    int fw = 0, fh = 0;
    uui_findbar_natural_size(&g_find, &fw, &fh);
    int cap = 34 * ugfx_char_advance('n');   // a field, not a banner
    int room = ns_view_page_w(v) - 16;
    if (fw > cap) fw = cap;
    if (fw > room) fw = room;
    if (fw < 1) fw = 1;
    uui_findbar_set_geometry(&g_find, v->x + ns_view_page_w(v) - 8 - fw, v->y + 6, fw, fh);
}

static struct uui_item g_widgets[5];
enum { W_CTX, W_ROOT, W_FIND, W_CARD, W_DLP };

static void downloads_toggle(void) {
    if (g_dlp.open) {
        ns_dlpanel_close(&g_dlp);
        return;
    }
    int x, y, w, h;
    uui_toolbar_item_rect(&g_tools_r, 0, &x, &y, &w, &h);
    ns_dlpanel_open(&g_dlp, x, y + h + 2, w, uapp_width(g_app), uapp_height(g_app));
}

static struct uui_item ROW[3], ITEMS[2], SIDE[3], PAGE_ROW[2];
static struct uui_layout ROW_L, ROOT_L, SIDE_L, PAGE_L;

static void side_refresh(void) {
    if (!g_side_open) return;
    int which = g_side_tabs.selected;
    g_side_list.rows = g_side_rows;
    g_side_list.count = ns_marks_rows(which, uui_textbox_text(&g_side_search), g_side_rows, NS_MARKS_MAX);
    g_side_list.removable = 1;
    g_side_list.empty = which == NS_MARKS_BOOKMARKS ? "No bookmarks yet: Ctrl+D keeps the page you are on"
                                                    : "Nothing browsed to yet";
    uui_entrylist_reset(&g_side_list);
    ulogf("browser: sidebar %d rows %d\n", which, g_side_list.count);
}

// Ctrl+B and Ctrl+H: open the sidebar on that list, switch to it, or
// close it when it is the one shown -- Firefox's toggle.
static void side_show(int which) {
    if (g_side_open && g_side_tabs.selected == which) {
        g_side_open = 0;
    } else {
        g_side_open = 1;
        g_side_tabs.selected = which;
    }
    PAGE_ROW[0].hidden = !g_side_open;
    if (g_app) uui_layout_run(&ROOT_L, 0, 0, uapp_width(g_app), uapp_height(g_app));
    side_refresh();
}

static void bookmark_toggle(void) {
    struct gui_window *gw = nsapp_active();
    if (!gw || !gw->url[0]) return;
    int on = ns_bookmark_toggle(gw->url, gw->title);
    ulogf("browser: bookmark %d\n", on);
    sync_chrome();
    if (g_side_tabs.selected == NS_MARKS_BOOKMARKS) side_refresh();
}

// A download started or moved on. One that STARTS opens the flyout, as
// Chrome's bubble and Firefox's panel do, so a click that downloads is
// seen to have done something.
void nsapp_download_changed(int started) {
    if (!g_app) return;
    if (started && !g_dlp.open) downloads_toggle();
    else ns_dlpanel_sync(&g_dlp, uapp_width(g_app), uapp_height(g_app));
    uapp_redraw(g_app);
}

static void find_open(void) {
    g_find_open = 1;
    g_widgets[W_FIND].hidden = 0;
    place_find();
    uui_findbar_activate(&g_find);
    g_address.active = 0;
    uui_focus_set(&g_focus, -1);
    if (uui_findbar_query(&g_find)[0]) find_run(0);
}

static void find_close(void) {
    g_find_open = 0;
    g_widgets[W_FIND].hidden = 1;
    uui_textbox_set_active(&g_find.field, 0);
    ns_find_clear(nsapp_active());
    uui_focus_set(&g_focus, 1);
}

static void zoom(int dir) {
    int pct = ns_view_zoom(nsapp_active(), dir);
    ulogf("browser: zoom %d\n", pct);
    sync_chrome();
}

// The lock's card, under the chip; a second click on the chip closes it.
static void card_toggle(void) {
    int x, y, w, h;
    if (g_card.open) {
        ns_card_close(&g_card);
        return;
    }
    if (!ns_addr_part_rect(&g_addr, NS_ADDR_CHIP, &x, &y, &w, &h)) return;
    struct ns_site site;
    ns_site_get(nsapp_active(), &site);
    ns_card_open(&g_card, &site, x, y + h + 4, uapp_width(g_app), uapp_height(g_app));
    ulogf("browser: card %d cookies %d chain %d tls %s\n", site.state, site.cookies,
          site.chain_depth, site.version[0] ? site.version : "-");
}

static void card_clear(void) {
    int n = ns_site_clear_cookies(nsapp_active());
    ulogf("browser: cookies cleared %d\n", n);
    struct ns_site site;
    ns_site_get(nsapp_active(), &site);
    ns_card_open(&g_card, &site, g_card.x, g_card.y, uapp_width(g_app), uapp_height(g_app));
    ulogf("browser: card %d cookies %d chain %d tls %s\n", site.state, site.cookies,
          site.chain_depth, site.version[0] ? site.version : "-");
}

static void open_menu(void) {
    int n = 0;
    snprintf(g_reset_label, sizeof g_reset_label, "Reset zoom (%d%%)", ns_view_zoom_pct(nsapp_active()));
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("New tab", CMD_NEWTAB, "Ctrl+T", "tb-new", UTHEME_ACT_CREATE);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_SEP;
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("Zoom in", CMD_ZOOM_IN, "Ctrl+=", "tb-zoom-in", UTHEME_ACT_VIEW);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("Zoom out", CMD_ZOOM_OUT, "Ctrl+-", "tb-zoom-out", UTHEME_ACT_VIEW);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON(g_reset_label, CMD_ZOOM_RESET, "Ctrl+0", "tb-actual", UTHEME_ACT_VIEW);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_SEP;
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("Find in page...", CMD_FIND, "Ctrl+F", "tb-find", UTHEME_ACT_NAV);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_SEP;
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON(ns_bookmarked(nsapp_active() ? nsapp_active()->url : "")
                                                            ? "Remove bookmark" : "Bookmark this page",
                                                            CMD_BOOKMARK, "Ctrl+D", "tb-star", UTHEME_ACT_CREATE);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("Bookmarks", CMD_SIDE_BOOKMARKS, "Ctrl+B", "tb-star", UTHEME_ACT_NAV);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("History", CMD_SIDE_HISTORY, "Ctrl+H", "tb-history", UTHEME_ACT_NAV);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("Downloads", CMD_DOWNLOADS, "Ctrl+J", "tb-down", UTHEME_ACT_NAV);
    int x, y, w, h;
    uui_toolbar_item_rect(&g_tools_r, 1, &x, &y, &w, &h);
    uui_menubar_open_below(&g_ctx, g_menu_items, n, x, y, w, h);
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
    case CMD_MENU:    open_menu(); break;
    case CMD_DOWNLOADS: downloads_toggle(); break;
    case CMD_BOOKMARK:  bookmark_toggle(); break;
    case CMD_SIDE_BOOKMARKS: side_show(NS_MARKS_BOOKMARKS); break;
    case CMD_SIDE_HISTORY:   side_show(NS_MARKS_HISTORY); break;
    case CMD_NEWTAB:  open_tab(HOME_URL); break;
    case CMD_ZOOM_IN:    zoom(+1); break;
    case CMD_ZOOM_OUT:   zoom(-1); break;
    case CMD_ZOOM_RESET: zoom(0); break;
    case CMD_FIND:
        if (g_find_open) {
            uui_findbar_activate(&g_find);
            uui_focus_set(&g_focus, -1);
        } else {
            find_open();
        }
        break;
    }
}

// --- layout -----------------------------------------------------------------


// The address first in Tab order; the page has the keyboard on open.
static struct uui_focusable g_focusables[] = {
    { .widget = &g_address, .ops = &uui_textbox_ops },
    { .widget = &g_view,    .ops = &ns_view_ops },
    { .widget = &g_side_search, .ops = &uui_textbox_ops },
};

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    int code = 0;
    if (id == ID_TOOLS) code = uui_toolbar_take_code(&g_tools);
    else if (id == ID_TOOLS_R) code = uui_toolbar_take_code(&g_tools_r);
    else if (id == ID_CTX) code = uui_menubar_take_code(&g_ctx);
    else if (id == ID_FIND) {
        uui_focus_set(&g_focus, -1);   // a click on the bar gives it the keyboard
        switch (uui_findbar_take(&g_find)) {
        case UUI_FIND_NEXT:  find_run(+1); break;
        case UUI_FIND_PREV:  find_run(-1); break;
        case UUI_FIND_CLOSE: find_close(); break;
        default: break;
        }
    } else if (id == ID_ADDRBAR) {
        switch (ns_addr_take(&g_addr)) {
        case NS_ADDR_CHIP: card_toggle(); break;
        case NS_ADDR_ZOOM: zoom(0); break;
        case NS_ADDR_STAR: bookmark_toggle(); break;
        }
    } else if (id == ID_SIDE_TABS || id == ID_SIDE_SEARCH) {
        side_refresh();
    } else if (id == ID_SIDE_LIST) {
        int row;
        switch (uui_entrylist_take(&g_side_list, &row)) {
        case UUI_ENTRY_EV_OPEN:   go(ns_marks_url(row)); break;
        case UUI_ENTRY_EV_REMOVE:
            ns_marks_remove(g_side_tabs.selected, row);
            sync_chrome();
            side_refresh();
            break;
        }
    } else if (id == ID_DLPANEL) {
        switch (ns_dlpanel_take(&g_dlp)) {
        case NS_DLP_OPEN_FOLDER: uapp_spawn(a, "/bin/wm/apps/files", NS_DOWNLOAD_DIR); break;
        case NS_DLP_CLEAR:       ns_downloads_clear_finished(); break;
        }
    } else if (id == ID_DLLIST) {
        int row;
        switch (uui_transfers_take(&g_dlp.list, &row)) {
        case UUI_XFER_EV_CANCEL: ns_download_cancel(row); break;
        case UUI_XFER_EV_SHOW:   uapp_spawn(a, "/bin/wm/apps/files", ns_download_path(row)); break;
        case UUI_XFER_EV_RETRY:  go(ns_download_url(row)); break;
        }
    } else if (id == ID_CARD) {
        if (ns_card_take(&g_card) == NS_CARD_CLEAR) card_clear();
    } else if (id == ID_PAGE && g_find_open) {
        uui_textbox_set_active(&g_find.field, 0);   // the page took the keyboard back
    }
    if (code > 0) command(code);
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    struct gui_window *gw = nsapp_active();
    int ctrl = (mods & KEY_MOD_CTRL) != 0;
    // An open find bar with the keyboard: the query, Enter, Esc -- and
    // the field's own Ctrl+A/C/V. Not the browser's shortcuts.
    int ours = uui_key_is_shortcut(key, mods) || (ctrl && strchr("\x02\x04\x06\x08\x0a\x0c\x12\x14\x17", key));
    if (g_find_open && g_focus.current < 0 && !ours && uui_findbar_key(&g_find, key, mods)) {
        switch (uui_findbar_take(&g_find)) {
        case UUI_FIND_CHANGED: find_run(0); break;
        case UUI_FIND_NEXT:    find_run(+1); break;
        case UUI_FIND_PREV:    find_run(-1); break;
        case UUI_FIND_CLOSE:   find_close(); break;
        default: break;
        }
        uapp_redraw(a);
        return;
    }
    if (ctrl && (key == '=' || key == '+')) {
        command(CMD_ZOOM_IN);
    } else if (ctrl && key == '-') {
        command(CMD_ZOOM_OUT);
    } else if (ctrl && key == '0') {
        command(CMD_ZOOM_RESET);
    } else if (ctrl && key == 0x06) {                             // Ctrl+F
        command(CMD_FIND);
    } else if (ctrl && key == 0x0A) {                             // Ctrl+J, not Enter
        command(CMD_DOWNLOADS);
    } else if (ctrl && key == 0x04) {                             // Ctrl+D
        command(CMD_BOOKMARK);
    } else if (ctrl && key == 0x02) {                             // Ctrl+B
        command(CMD_SIDE_BOOKMARKS);
    } else if (ctrl && key == 0x08) {                             // Ctrl+H, not Backspace
        command(CMD_SIDE_HISTORY);
    } else if (mods & KEY_MOD_ALT) {
        if (key == KEY_ARROW_LEFT) command(CMD_BACK);
        else if (key == KEY_ARROW_RIGHT) command(CMD_FORWARD);
        else if (key == KEY_HOME) command(CMD_HOME);
        else return;
    } else if (key == KEY_F5 || (ctrl && key == 0x12)) {          // F5, Ctrl+R
        if (gw) browser_window_reload(gw->bw, true);
    } else if (ctrl && key == 0x0C) {                             // Ctrl+L
        if (g_find_open) uui_textbox_set_active(&g_find.field, 0);
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
    } else if (g_find_open && key == 0x1B) {
        find_close();
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
        // The LENGTH, and at most 72 bytes of the address: a layout line
        // is 160 bytes, and one cut by vsnprintf() loses its newline and
        // runs into the next. A test reads the real length from `len`.
        uapp_logf_layout("browser: tab %d/%d loading %d extent %d %d scroll %d %d len %d url %.72s\n",
                         g_cur, g_ntabs, gw->loading, gw->ext_w, gw->ext_h, gw->sx, gw->sy,
                         (int)strlen(gw->url), gw->url);
}

static void on_size(int *w, int *h) {
    *w = ugfx_char_advance('n') * 110;
    *h = ugfx_char_h() * 40;
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    uui_menubar_set_bounds(&g_ctx, 0, 0, w, h);
    if (g_find_open) place_find();
}

static void on_open(struct uapp *a) {
    g_app = a;
    uui_menubar_set_bounds(&g_ctx, 0, 0, uapp_width(a), uapp_height(a));
    struct nsurl *u;
    if (nsurl_create(HOME_URL, &u) == NSERROR_OK) {
        browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND, u, NULL, NULL, NULL);
        nsurl_unref(u);
    }
    uui_focus_set(&g_focus, 1);
}

static int on_close(struct uapp *a) {
    (void)a;
    urldb_save_cookies(COOKIES_FILE);
    urldb_save(HISTORY_FILE);
    ns_marks_fini();   // saves the bookmarks
    netsurf_exit();
    return 1;
}

// A fetch thread queued something (fetch_http.c's wake). NetSurf's own
// poll runs every 10 ms while a fetch is active; this drains NOW, through
// fetch_fdset(), which polls every fetcher -- how upstream's front ends
// drive their fetchers when select() wakes them.
static int on_user(struct uapp *a, int a0, int a1) {
    (void)a; (void)a0; (void)a1;
    fd_set r, w, e;
    int maxfd;
    fetch_fdset(&r, &w, &e, &maxfd);
    return 0;
}

static void wake(void) {
    if (g_app) uapp_post(g_app, 0, 0);
}

// --- NetSurf ----------------------------------------------------------------

static nserror set_defaults(struct nsoption_s *defaults) {
    (void)defaults;
    nsoption_setnull_charp(cookie_file, strdup(COOKIES_FILE));
    nsoption_setnull_charp(cookie_jar, strdup(COOKIES_FILE));
    nsoption_setnull_charp(url_file, strdup(HISTORY_FILE));
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
    table.download = ns_download_table;
    if (netsurf_register(&table) != NSERROR_OK) return 0;
    ns_bitmap_set_format();
    if (nsoption_init(set_defaults, &nsoptions, &nsoptions_default) != NSERROR_OK) return 0;
    // Ours before NetSurf's own parsing, which consumes "--" words:
    // `--weak-entropy` is https on a test guest with no virtio-rng, as
    // wget's flag; `--search-url` is above.
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--weak-entropy")) ns_fetch_http_allow_weak_entropy(1);
        else if (!strcmp(argv[i], "--search-url") && i + 1 < argc && strstr(argv[i + 1], "%s"))
            g_search = argv[++i];
    }
    nsoption_commandline(&argc, argv, nsoptions);
    // Six fetches at once and six to one host -- Chrome's and Firefox's
    // per-host figure, and each is a thread here (NetSurf's default 24).
    nsoption_set_int(max_fetchers, 6);
    nsoption_set_int(max_fetchers_per_host, 6);
    if (messages_add_from_file(MESSAGES) != NSERROR_OK) ulogf("browser: no %s\n", MESSAGES);
    if (netsurf_init(NULL) != NSERROR_OK) return 0;
    if (ns_fetch_http_register() != 0) ulogf("browser: no http fetcher -- local pages only\n");
    if (ns_image_uimg_register() != NSERROR_OK) ulogf("browser: no PNG/JPEG handler\n");
    ns_fetch_http_set_wake(wake);
    apply_user_agent();
    mkdir("/var/lib", 0755);
    mkdir(STATE_DIR, 0755);
    urldb_load(HISTORY_FILE);
    urldb_load_cookies(COOKIES_FILE);
    if (!ns_marks_init(BOOKMARKS_FILE)) ulogf("browser: bookmarks could not be loaded\n");
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
    uui_textbox_set_max(&g_address, NS_URL_MAX - 1);   // a URL, not a 127-byte value
    g_address.placeholder = "Type a web address or a path";
    ns_view_init(&g_view);
    uui_focus_init(&g_focus, g_focusables, (int)(sizeof g_focusables / sizeof g_focusables[0]));

    ROW[0] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &g_tools, .id = ID_TOOLS,
                                .name = "tools" };
    ns_addr_init(&g_addr, &g_address, ID_ADDRESS, "address");
    g_addr.show_star = 1;
    ROW[1] = (struct uui_item){ .ops = &ns_addr_ops, .widget = &g_addr, .id = ID_ADDRBAR,
                                .name = "addressbar", .flags = UUI_FILL_W };
    ROW[2] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &g_tools_r, .id = ID_TOOLS_R,
                                .name = "tools-right" };
    ROW_L = (struct uui_layout){ .dir = UUI_ROW, .items = ROW, .count = 3, .gap = 4, .margin = 3 };
    ITEMS[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &ROW_L, .flags = UUI_FILL_W };
    uui_segmented_init(&g_side_tabs, SIDE_TABS, 2, NS_MARKS_BOOKMARKS);
    uui_textbox_init(&g_side_search, "");
    g_side_search.placeholder = "Search";
    uui_entrylist_init(&g_side_list, NULL);
    SIDE[0] = (struct uui_item){ .ops = &uui_segmented_ops, .widget = &g_side_tabs, .id = ID_SIDE_TABS,
                                 .name = "side-tabs" };
    SIDE[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_side_search, .id = ID_SIDE_SEARCH,
                                 .name = "side-search", .flags = UUI_FILL_W };
    SIDE[2] = (struct uui_item){ .ops = &uui_entrylist_ops, .widget = &g_side_list, .id = ID_SIDE_LIST,
                                 .name = "side-list", .flags = UUI_FILL_W | UUI_FILL_H };
    SIDE_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = SIDE, .count = 3, .gap = 6, .margin = 6 };
    PAGE_ROW[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &SIDE_L, .flags = UUI_FILL_H,
                                     .hidden = 1 };
    PAGE_ROW[1] = (struct uui_item){ .ops = &ns_view_ops, .widget = &g_view, .id = ID_PAGE,
                                     .name = "page", .flags = UUI_FILL_W | UUI_FILL_H };
    PAGE_L = (struct uui_layout){ .dir = UUI_ROW, .items = PAGE_ROW, .count = 2, .margin = 1 };
    ITEMS[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &PAGE_L,
                                  .flags = UUI_FILL_W | UUI_FILL_H };
    // Edge to edge: a margin of 0 means "the default", a character cell,
    // which framed the page in panel grey -- 1 is the least there is.
    ROOT_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS, .count = 2, .margin = 1 };
    // Drawn in this order and hit-tested in reverse: the menu's popup is
    // offered every press first anyway (an overlay), and the find bar
    // draws over the page and takes a press on it before the page does.
    g_widgets[W_CTX] = (struct uui_item){ .ops = &uui_menubar_ops, .widget = &g_ctx, .id = ID_CTX,
                                          .name = "menu" };
    g_widgets[W_ROOT] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &ROOT_L };
    g_widgets[W_FIND] = (struct uui_item){ .ops = &uui_findbar_ops, .widget = &g_find, .id = ID_FIND,
                                           .name = "find", .hidden = 1 };
    g_widgets[W_CARD] = (struct uui_item){ .ops = &ns_card_ops, .widget = &g_card, .id = ID_CARD,
                                           .name = "card" };
    ns_dlpanel_init(&g_dlp, ID_DLLIST);
    g_widgets[W_DLP] = (struct uui_item){ .ops = &ns_dlpanel_ops, .widget = &g_dlp, .id = ID_DLPANEL,
                                          .name = "dlpanel" };
    uui_menubar_init(&g_ctx, 0, 0);   // no bar of its own: open_below() supplies the rows
    uui_findbar_init(&g_find);
    g_find.pill = 1;

    struct uapp_desc desc = {
        .title        = "Web Browser",
        .app_id       = "browser",
        .on_size      = on_size,
        .layout       = &ROOT_L,
        .widgets      = g_widgets,
        .widget_count = 5,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_tab       = on_tab,
        .on_draw_over = on_draw_over,
        .on_tick      = on_tick,
        .on_user      = on_user,
        .tick_ms      = 10,
        .on_open      = on_open,
        .on_resize    = on_resize,
        .on_close     = on_close,
        .focus        = &g_focus,
        .flags        = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
