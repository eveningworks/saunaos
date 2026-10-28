// Web Browser -- web pages in a SaunaOS window (docs/browser-design.md,
// stages 3 to 5).
//
// W1 from the design canvas: tabs in the title bar (the compositor's,
// as Files has them), a toolbar row -- back, forward, reload, home, the
// address field, then the right-hand buttons -- and the page.
//
// THE ENGINE IS NOT HERE. Each tab is a ui/uui_webview, whose page is
// laid out and painted by its own /bin/webcontent (NetSurf, GPL-2.0); the
// network is /bin/webnet, started below, one per browser. This program
// links neither and is MIT. What it keeps itself: the tabs and their
// chrome, bookmarks and history (browser/ns_marks.c), the downloads list
// webnet reports (browser/ns_net.c), and the open tabs across a restart
// (browser/ns_session.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "browser/ns_addr.h"
#include "browser/ns_card.h"
#include "browser/ns_dlpanel.h"
#include "browser/ns_download.h"
#include "browser/ns_marks.h"
#include "browser/ns_net.h"
#include "browser/ns_session.h"
#include "browser/ns_site.h"

#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_entrylist.h"
#include "ui/uui_findbar.h"
#include "ui/uui_focus.h"
#include "ui/uui_menubar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_signin.h"
#include "ui/uui_textbox.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_webview.h"
#include "ui/uui_siteperm.h"
#include "ui/ulog.h"
#include "lib/ugrow.h"
#include "lib/usiteperm.h"
#include "lib/usetting.h"
#include "rt/sys.h"
#include "win_proto.h"   // WIN_TAB_*, WIN_TABS_MAX, WIN_TITLE_LEN

#define HOME_URL "file:///usr/share/netsurf/start.html"
// What the browser KEEPS, in /var/lib like every app's kept state
// (docs/filesystem-layout.md): bookmarks and history here, the cookie
// jar in /bin/webnet.
#define STATE_DIR    "/var/lib/browser"

enum { ID_TOOLS = 1, ID_ADDRESS, ID_TOOLS_R, ID_PAGE, ID_CTX, ID_FIND, ID_ADDRBAR, ID_CARD,
       ID_DLPANEL, ID_DLLIST, ID_SIDE_TABS, ID_SIDE_SEARCH, ID_SIDE_LIST, ID_DIALOG, ID_ASK };
enum { CMD_BACK = 1, CMD_FORWARD, CMD_RELOAD, CMD_HOME, CMD_DOWNLOADS, CMD_MENU,
       CMD_NEWTAB, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET, CMD_FIND,
       CMD_BOOKMARK, CMD_SIDE_BOOKMARKS, CMD_SIDE_HISTORY };
// What uapp_post() carries: a content process wrote, or webnet did.
enum { POST_WEBVIEW = 1, POST_NET };

static struct uapp *g_app;

// --- tabs: one page view, and so one content process, each ------------------

static struct uui_webview *g_tabs[WIN_TABS_MAX];
static int g_ntabs, g_cur = -1;
static char g_label[WIN_TABS_MAX][WIN_TITLE_LEN];
static int g_zoom_logged[WIN_TABS_MAX];

static struct uui_webview *active(void) { return g_cur >= 0 ? g_tabs[g_cur] : NULL; }

// The session is saved a moment after it changes, not on every change:
// a page's title and address arrive one after another.
#define SESSION_SAVE_NS 2000000000ull
static int g_session_dirty;
static unsigned long long g_session_saved_ns;

static void session_save(void) {
    if (ns_session_save(STATE_DIR, g_tabs, g_ntabs, g_cur) != 0) ulogf("browser: session not saved\n");
    g_session_dirty = 0;
    g_session_saved_ns = sys_monotonic_ns();
}

static const char *tab_name(const struct uui_webview *v) {
    if (v->title[0]) return v->title;
    if (v->url[0]) return v->url;
    return "New tab";
}

static void sync_tabs(void) {
    if (!g_app || !g_ntabs) return;
    const char *labels[WIN_TABS_MAX];
    int icons[WIN_TABS_MAX];
    for (int i = 0; i < g_ntabs; i++) {
        strlcpy(g_label[i], tab_name(g_tabs[i]), sizeof g_label[i]);
        labels[i] = g_label[i];
        icons[i] = g_tabs[i]->stopped ? WIN_TAB_ICON_STOPPED : WIN_TAB_ICON_PAGE;
    }
    uapp_set_tab_icons(g_app, icons, g_ntabs);
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
static struct uui_permask g_ask;   // "Scripts blocked", under its badge
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
    struct uui_webview *v = active();
    if (!v) return UUI_MI_DISABLED;
    switch (code) {
    case CMD_BACK:      return v->can_back ? 0 : UUI_MI_DISABLED;
    case CMD_FORWARD:   return v->can_forward ? 0 : UUI_MI_DISABLED;
    case CMD_DOWNLOADS: return g_dlp.open ? UUI_MI_CHECKED : 0;
    }
    return 0;
}

static void sync_chrome(void) {
    struct uui_webview *v = active();
    int loading = v && v->st.loading && !v->stopped;
    if (loading != g_shown_loading) {
        g_shown_loading = loading;
        const struct uui_toolbar_item *items = loading ? TOOLS_LOADING : TOOLS;
        g_tools.items = items;
    }
    // The address shows the page's -- unless the person is typing in it.
    if (v && !g_address.active) {
        uui_textbox_set_text(&g_address, v->url);
    }
    int state = v ? v->site.state : NS_SITE_NONE, zoom = v ? v->st.zoom : 100;
    int starred = v && ns_bookmarked(v->url);
    int blocked = v && !v->st.loading && !v->site.js && v->site.scripts > 0;
    int js = v ? v->site.js : 1, scripts = v ? v->site.scripts : 0;
    static int logged_js = -1, logged_scripts = -1;
    if (state != g_addr.state || zoom != g_addr.zoom || starred != g_addr.starred ||
        blocked != g_addr.scripts_blocked || js != logged_js || scripts != logged_scripts) {
        g_addr.state = state;
        g_addr.zoom = zoom;
        g_addr.starred = starred;
        g_addr.scripts_blocked = blocked;
        logged_js = js;
        logged_scripts = scripts;
        ns_addr_sync(&g_addr);
        ulogf("browser: site %d zoom %d scripts %d %d\n", state, zoom, js, scripts);
    }
}

static void side_refresh(void);

static void find_report(void);

static int tab_of(const struct uui_webview *v) {
    for (int i = 0; i < g_ntabs; i++) if (g_tabs[i] == v) return i;
    return -1;
}

// What a page's content process said, through the widget.
static void wv_changed(struct uui_webview *v, unsigned what) {
    int i = tab_of(v);
    if (i < 0) return;
    if (v == active()) {
        sync_chrome();
        // A page that finished loading is the newest in History.
        if ((what & UUI_WV_STATE) && !v->st.loading && g_side_open &&
            g_side_tabs.selected == NS_MARKS_HISTORY)
            side_refresh();
        if ((what & UUI_WV_STATE) && v->st.zoom != g_zoom_logged[i]) {
            g_zoom_logged[i] = v->st.zoom;
            ulogf("browser: zoom %d\n", v->st.zoom);
        }
        if (what & UUI_WV_FIND) find_report();
    }
    if (what & (UUI_WV_TITLE | UUI_WV_URL | UUI_WV_STOPPED)) sync_tabs();
    if (what & (UUI_WV_TITLE | UUI_WV_URL)) g_session_dirty = 1;
    if (g_app) uapp_redraw(g_app);
}

static void open_tab(const char *url);

static void wv_open_tab(struct uui_webview *v, const char *url) {
    (void)v;
    open_tab(url);
}

static void wv_visited(struct uui_webview *v, const char *url, const char *title) {
    (void)v;
    ns_marks_visited(url, title);
}

// Hosts whose https did not answer this session (WV_DOWNGRADED): tried
// over http first from then on, as Chrome keeps its fallback list.
// The hosts let down to http this session, grown (lib/ugrow.h): it held
// 32 and stopped remembering without a word. A host is the 128 the
// webview hands over.
static char (*g_http_hosts)[128];
static int g_nhttp_hosts, g_http_hosts_cap;
// `--upgrade-local`: a test's way to have an address and port tried over
// https too, against a server on this machine that speaks only http.
static int g_upgrade_local;

static int http_host(const char *in) {
    size_t n = strcspn(in, "/?#");
    for (int i = 0; i < g_nhttp_hosts; i++)
        if (strlen(g_http_hosts[i]) == n && !strncasecmp(g_http_hosts[i], in, n)) return 1;
    return 0;
}

static void wv_downgraded(struct uui_webview *v, const char *host) {
    (void)v;
    ulogf("browser: https fallback %s\n", host);
    if (http_host(host)) return;
    if (!UGROW(g_http_hosts, g_http_hosts_cap, g_nhttp_hosts + 1)) {
        ulogf("browser: out of memory, %s not remembered as http\n", host);
        return;
    }
    snprintf(g_http_hosts[g_nhttp_hosts++], sizeof g_http_hosts[0], "%s", host);
}

static const struct uui_webview_host HOST = {
    .changed = wv_changed,
    .open_tab = wv_open_tab,
    .visited = wv_visited,
    .downgraded = wv_downgraded,
};

// --- navigation ---------------------------------------------------------------

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
// port other than 443 -- libhttp's own guess -- or one whose https did not
// answer before), and anything else is a search. `*upgraded` says the
// https was this function's choice, which may fall back. The content
// process parses it; a URL it cannot make goes nowhere.
static const char *typed_url(const char *in, int *upgraded) {
    static char url[WEBVIEW_URL_MAX];   // static: past the ring-3 frame budget
    *upgraded = 0;
    while (*in == ' ') in++;
    if (strstr(in, "://") || !strncmp(in, "about:", 6) || !strncmp(in, "data:", 5) ||
        !strncmp(in, "resource:", 9)) {
        strlcpy(url, in, sizeof url);
    } else if (in[0] == '/') {
        snprintf(url, sizeof url, "file://%s", in);
    } else if (looks_like_host(in)) {
        const char *colon = strchr(in, ':');
        int odd_port = colon && strncmp(colon, ":443", 4) != 0;
        *upgraded = !http_host(in) && (g_upgrade_local || !(local_host(in) || odd_port));
        snprintf(url, sizeof url, "%s://%s", *upgraded ? "https" : "http", in);
    } else {
        search_url(in, url, sizeof url);
    }
    return url;
}

static void go(const char *text) {
    struct uui_webview *v = active();
    if (!v) return;
    int upgraded;
    const char *url = typed_url(text, &upgraded);
    // about:crash TYPED, never followed from a page: the engine faults on
    // purpose, as chrome://crash does -- how the stopped page is tried.
    if (!strcmp(url, "about:crash")) {
        uui_webview_crash(v);
        return;
    }
    if (upgraded) uui_webview_navigate_upgraded(v, url);
    else uui_webview_navigate(v, url);
}

static void place_page(void);
static void find_close(void);
static void login_show_next(void);
static void login_hide(void);

static void select_tab(int i) {
    if (i < 0 || i >= g_ntabs) return;
    if (g_find_open && i != g_cur) find_close();
    ns_card_close(&g_card);
    struct uui_webview *old = active();
    g_cur = i;
    if (old && old != g_tabs[i]) uui_webview_set_visible(old, 0);
    if (old != g_tabs[i]) login_hide();   // a tab's sign-in waits for the tab
    place_page();
    uui_webview_set_visible(g_tabs[i], 1);
    login_show_next();
    g_shown_loading = -1;
    g_address.active = 0;
    g_session_dirty = 1;
    sync_chrome();
    sync_tabs();
    if (g_app) uapp_redraw(g_app);
}

// A new tab, shown, with nothing loaded yet. NULL when there is no room.
static struct uui_webview *new_tab(void) {
    if (g_ntabs >= WIN_TABS_MAX) return NULL;
    struct uui_webview *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    uui_webview_init(v, &HOST, NULL);
    v->net = ns_net_service();
    g_zoom_logged[g_ntabs] = 100;
    g_tabs[g_ntabs++] = v;
    select_tab(g_ntabs - 1);
    return v;
}

static void open_tab(const char *url) {
    struct uui_webview *v = new_tab();
    if (v) uui_webview_navigate(v, url);
}

static void close_tab(int i) {
    if (i < 0 || i >= g_ntabs) return;
    if (g_ntabs == 1) {               // the last tab: the window goes with it
        uapp_quit(g_app, 0);
        return;
    }
    struct uui_webview *v = g_tabs[i];
    uui_webview_destroy(v);
    free(v);
    memmove(&g_tabs[i], &g_tabs[i + 1], (size_t)(g_ntabs - i - 1) * sizeof g_tabs[0]);
    memmove(&g_zoom_logged[i], &g_zoom_logged[i + 1], (size_t)(g_ntabs - i - 1) * sizeof g_zoom_logged[0]);
    g_ntabs--;
    int keep = g_cur > i ? g_cur - 1 : g_cur;
    if (keep >= g_ntabs) keep = g_ntabs - 1;
    g_cur = -1;   // select_tab's "old" is gone
    select_tab(keep);
}

// --- find in page and zoom ----------------------------------------------------

static void find_report(void) {
    struct uui_webview *v = active();
    g_find.matches = v ? v->find.matches : 0;
    g_find.current = v ? v->find.current : 0;
    ulogf("browser: find %d of %d\n", g_find.current, g_find.matches);
}

static void find_run(int dir) {
    if (active()) uui_webview_find(active(), uui_findbar_query(&g_find), dir);
}

static void place_find(void) {
    struct uui_webview *v = active();
    if (!v) return;
    int fw = 0, fh = 0, pw = v->w - uui_sbar_width();
    uui_findbar_natural_size(&g_find, &fw, &fh);
    int cap = 34 * ugfx_char_advance('n');   // a field, not a banner
    int room = pw - 16;
    if (fw > cap) fw = cap;
    if (fw > room) fw = room;
    if (fw < 1) fw = 1;
    uui_findbar_set_geometry(&g_find, v->x + pw - 8 - fw, v->y + 6, fw, fh);
}

enum { W_CTX, W_ROOT, W_FIND, W_CARD, W_ASK, W_DLP, W_DIALOG, W_COUNT };
static struct uui_item g_widgets[W_COUNT];

// ONE POPUP AT A TIME, as a Wayland popup grab allows and every
// desktop's menus behave: opening the menu, the lock's card or the
// Downloads flyout closes the other two.
static void popups_close(void) {
    uui_menubar_close(&g_ctx);
    ns_card_close(&g_card);
    uui_permask_close(&g_ask);
    ns_dlpanel_close(&g_dlp);
}

static void downloads_toggle(void) {
    if (g_dlp.open) {
        ns_dlpanel_close(&g_dlp);
        return;
    }
    popups_close();
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
    struct uui_webview *v = active();
    if (!v || !v->url[0]) return;
    int on = ns_bookmark_toggle(v->url, v->title);
    ulogf("browser: bookmark %d\n", on);
    sync_chrome();
    if (g_side_tabs.selected == NS_MARKS_BOOKMARKS) side_refresh();
}

// A download started or moved on. One that STARTS opens the flyout, as
// Chrome's bubble and Firefox's panel do, so a click that downloads is
// seen to have done something.
void nsapp_download_changed(int started) {
    if (!g_app) return;
    // Not over a menu or card the person has open: the row is there
    // when they next open the flyout.
    if (started && !g_dlp.open && !g_card.open && !uui_menubar_is_open(&g_ctx)) downloads_toggle();
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
    if (active()) uui_webview_find_clear(active());
    uui_focus_set(&g_focus, 1);
}

// The new level comes back from the page as UUI_WV_STATE, logged there.
static void zoom(int dir) {
    if (active()) uui_webview_zoom(active(), dir);
}

// --- JavaScript, per site (J2 + J3 on the JavaScript canvas) ------------------
//
// The exceptions are kept here (lib/usiteperm.h) and pushed whole to every
// content process, which decides each page itself (lib/webview_proto.h's
// WV_PERMS); a change reloads the page it was made on.

static struct usiteperm g_perms;

static int js_default_on(void) {
    char v[8];
    return usetting_get("browser.javascript", v, sizeof v) < 0 || strcmp(v, "off") != 0;
}

static void perms_push(void) {
    static char list[16384];
    if (usiteperm_list(&g_perms, "javascript", list, sizeof list) < 0) list[0] = '\0';
    uui_webview_set_perms(list);
}

static void js_set(const char *host, int value) {
    struct uui_webview *v = active();
    if (!host[0] || usiteperm_set(&g_perms, host, "javascript", value) != 0) {
        ulogf("browser: javascript for %s not saved\n", host);
        return;
    }
    ulogf("browser: javascript %s %d\n", host, value);
    perms_push();
    if (v) uui_webview_reload(v);
}

static void site_of(struct ns_site *site, const struct uui_webview *v) {
    int logins, cookies = ns_net_cookies(v->site.host, &logins);
    ns_site_from(site, &v->site, cookies, logins);
    site->js_perm = usiteperm_get(&g_perms, v->site.host, "javascript");
    site->js_default_on = js_default_on();
}

// "Scripts blocked", under its badge: why, and Allow on this site.
static void ask_toggle(void) {
    int x, y, w, h;
    if (g_ask.open) {
        uui_permask_close(&g_ask);
        return;
    }
    struct uui_webview *v = active();
    if (!v || !ns_addr_part_rect(&g_addr, NS_ADDR_SCRIPTS, &x, &y, &w, &h)) return;
    popups_close();
    char count[64];
    snprintf(count, sizeof count, "This page has %d script%s.", v->site.scripts,
             v->site.scripts == 1 ? "" : "s");
    int blocked_here = usiteperm_get(&g_perms, v->site.host, "javascript") == USITEPERM_BLOCK;
    const char *why = blocked_here ? "JavaScript is blocked on this site."
                                   : "JavaScript is off in System Settings.";
    // A page with no host (a file) has no site to allow it on.
    uui_permask_open(&g_ask, "Scripts on this page were blocked", count, why,
                     v->site.host[0] ? "Settings" : "", v->site.host[0] ? "Allow on this site" : "Settings",
                     x + w, y + h + 4, uapp_width(g_app), uapp_height(g_app));
    ulogf("browser: ask open %d\n", v->site.scripts);
}

static void ask_answer(int e) {
    struct uui_webview *v = active();
    if (!v) return;
    int settings = e == UUI_PERMASK_SECONDARY || (e == UUI_PERMASK_PRIMARY && !v->site.host[0]);
    if (settings) uapp_spawn(g_app, "/bin/wm/system/settings", "browser.javascript");
    else if (e == UUI_PERMASK_PRIMARY) js_set(v->site.host, USITEPERM_ALLOW);
}

// The lock's card, under the chip; a second click on the chip closes it.
static void card_toggle(void) {
    int x, y, w, h;
    if (g_card.open) {
        ns_card_close(&g_card);
        return;
    }
    if (!ns_addr_part_rect(&g_addr, NS_ADDR_CHIP, &x, &y, &w, &h)) return;
    popups_close();
    struct uui_webview *v = active();
    if (!v) return;
    struct ns_site site;
    site_of(&site, v);
    ns_card_open(&g_card, &site, x, y + h + 4, uapp_width(g_app), uapp_height(g_app));
    ulogf("browser: card %d cookies %d chain %d tls %s logins %d\n", site.state, site.cookies,
          site.chain_depth, site.version[0] ? site.version : "-", site.logins);
}

static void card_clear(void) {
    struct uui_webview *v = active();
    if (!v) return;
    int n = ns_net_cookies_clear(v->site.host);
    ulogf("browser: cookies cleared %d\n", n < 0 ? 0 : n);
    struct ns_site site;
    site_of(&site, v);
    ns_card_open(&g_card, &site, g_card.x, g_card.y, uapp_width(g_app), uapp_height(g_app));
    ulogf("browser: card %d cookies %d chain %d tls %s logins %d\n", site.state, site.cookies,
          site.chain_depth, site.version[0] ? site.version : "-", site.logins);
}

static void open_menu(void) {
    popups_close();
    int n = 0;
    snprintf(g_reset_label, sizeof g_reset_label, "Reset zoom (%d%%)", active() ? active()->st.zoom : 100);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("New tab", CMD_NEWTAB, "Ctrl+T", "tb-new", UTHEME_ACT_CREATE);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_SEP;
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("Zoom in", CMD_ZOOM_IN, "Ctrl+=", "tb-zoom-in", UTHEME_ACT_VIEW);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("Zoom out", CMD_ZOOM_OUT, "Ctrl+-", "tb-zoom-out", UTHEME_ACT_VIEW);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON(g_reset_label, CMD_ZOOM_RESET, "Ctrl+0", "tb-actual", UTHEME_ACT_VIEW);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_SEP;
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("Find in page...", CMD_FIND, "Ctrl+F", "tb-find", UTHEME_ACT_NAV);
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_SEP;
    g_menu_items[n++] = (struct uui_menu_item)UUI_MENU_ICON(ns_bookmarked(active() ? active()->url : "")
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
    struct uui_webview *v = active();
    if (!v) return;
    switch (code) {
    case CMD_BACK:    uui_webview_back(v); break;
    case CMD_FORWARD: uui_webview_forward(v); break;
    case CMD_RELOAD:
        if (v->st.loading && !v->stopped) uui_webview_stop(v);
        else uui_webview_reload(v);
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
// Slot 1 is whichever tab's view is shown (place_page()).
static struct uui_focusable g_focusables[] = {
    { .widget = &g_address, .ops = &uui_textbox_ops },
    { .widget = NULL,       .ops = &uui_webview_ops },
    { .widget = &g_side_search, .ops = &uui_textbox_ops },
};

// The shown tab's view into the page's place in the layout and the
// focus ring, at the size the page area has.
static void place_page(void) {
    struct uui_webview *v = active(), *was = g_focusables[1].widget;
    if (!v) return;
    if (was && was != v && g_focus.current == 1) uui_webview_ops.set_focused(was, 0);
    PAGE_ROW[1].widget = v;
    g_focusables[1].widget = v;
    if (g_focus.current == 1) uui_webview_ops.set_focused(v, 1);
    if (g_app) uui_layout_run(&ROOT_L, 0, 0, uapp_width(g_app), uapp_height(g_app));
}

// --- HTTP logins: webnet asks (ns_net.h), a sign-in over the tab answers --------

// Asks not yet answered, oldest first; the dialog shows the first one
// whose tab is in front.
// The sign-ins asked for and not yet answered, oldest first, grown
// (lib/ugrow.h); a `struct login *` lives for one call.
static struct login { uint32_t id; int pid, retry, secure; char host[128], realm[128]; } *g_login;
static int g_nlogin, g_login_cap;
static uint32_t g_login_shown;   // the ask the dialog is up for; 0 when none
// The user name last given, and for which host: a retry starts with it,
// so only the password is typed again. Never the password.
static char g_login_user[128], g_login_user_host[128];
static struct uui_dialog g_dialog;
static struct uui_signin g_signin;

static void login_drop(uint32_t id) {
    for (int i = 0; i < g_nlogin; i++)
        if (g_login[i].id == id) {
            memmove(&g_login[i], &g_login[i + 1], (size_t)(g_nlogin - i - 1) * sizeof g_login[0]);
            g_nlogin--;
            return;
        }
}

static int tab_of_pid(int pid) {
    for (int i = 0; i < g_ntabs; i++) if (pid && g_tabs[i]->pid == pid) return i;
    return -1;
}

static void login_hide(void) {
    if (!g_login_shown) return;
    uui_dialog_set_body(&g_dialog, NULL, 0, 0);
    uui_dialog_close(&g_dialog);
    g_login_shown = 0;
}

static void login_show_next(void) {
    if (g_login_shown || uui_dialog_is_open(&g_dialog)) return;
    for (int i = 0; i < g_nlogin; i++) {
        struct login *l = &g_login[i];
        int t = tab_of_pid(l->pid);
        if (t < 0) {   // its page is gone: nobody to sign in for
            ns_net_login_answer(l->id, NULL, NULL);
            login_drop(l->id);
            i--;
            continue;
        }
        if (t != g_cur) continue;
        char asks[192], area[192];
        snprintf(asks, sizeof asks, "%s asks for a user name and password.", l->host);
        snprintf(area, sizeof area, "It calls this area \"%s\".", l->realm);
        struct uui_signin_spec spec = {
            .title = "Sign in",
            .lines = { asks, l->realm[0] ? area : "Remembered for this site until the browser closes.",
                       l->realm[0] ? "Remembered for this site until the browser closes." : NULL },
            .user = strcmp(g_login_user_host, l->host) ? NULL : g_login_user,
            .note = l->secure ? NULL : "Not secure: the password crosses the network unencrypted.",
            .error = l->retry ? "Wrong user name or password." : NULL,
        };
        struct uui_webview *v = active();
        uui_dialog_set_bounds(&g_dialog, v->x, v->y, v->w, v->h);
        uui_signin_open(&g_signin, &g_dialog, &spec);
        g_login_shown = l->id;
        ulogf("browser: signin open %s retry %d secure %d\n", l->host, l->retry, l->secure);
        if (g_app) uapp_redraw(g_app);
        return;
    }
}

void nsapp_login_ask(uint32_t id, int pid, int retry, int secure, const char *host, const char *realm) {
    if (!UGROW(g_login, g_login_cap, g_nlogin + 1)) {
        ulogf("browser: out of memory, the sign-in for %s is cancelled\n", host);
        ns_net_login_answer(id, NULL, NULL);
        return;
    }
    struct login *l = &g_login[g_nlogin++];
    l->id = id;
    l->pid = pid;
    l->retry = retry;
    l->secure = secure;
    snprintf(l->host, sizeof l->host, "%s", host);
    snprintf(l->realm, sizeof l->realm, "%s", realm);
    login_show_next();
}

void nsapp_login_gone(uint32_t id) {
    if (id == g_login_shown) login_hide();
    login_drop(id);
    login_show_next();
    if (g_app) uapp_redraw(g_app);
}

static void login_answer(int code) {
    char user[128], pass[256];
    int keep;
    int r = uui_signin_answer(&g_signin, code, user, sizeof user, pass, sizeof pass, &keep);
    if (r == UUI_SIGNIN_REASKED) return;
    uint32_t id = g_login_shown;
    g_login_shown = 0;
    if (r == UUI_SIGNIN_GIVEN)
        for (int i = 0; i < g_nlogin; i++)
            if (g_login[i].id == id) {
                snprintf(g_login_user, sizeof g_login_user, "%s", user);
                snprintf(g_login_user_host, sizeof g_login_user_host, "%s", g_login[i].host);
            }
    ns_net_login_answer(id, r == UUI_SIGNIN_GIVEN ? user : NULL, pass);
    memset(pass, 0, sizeof pass);
    ulogf("browser: signin %s\n", r == UUI_SIGNIN_GIVEN ? "given" : "cancelled");
    login_drop(id);
    login_show_next();
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    int code = 0;
    if (id == ID_DIALOG) {
        int c = uui_dialog_take_code(&g_dialog);
        if (c > 0 && g_login_shown) login_answer(c);
        uapp_redraw(a);
        return;
    }
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
        case NS_ADDR_SCRIPTS: ask_toggle(); break;
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
        switch (ns_card_take(&g_card)) {
        case NS_CARD_CLEAR: card_clear(); break;
        case NS_CARD_JS:
            if (active()) js_set(active()->site.host, ns_card_js(&g_card));
            break;
        }
    } else if (id == ID_ASK) {
        ask_answer(uui_permask_take(&g_ask));
    } else if (id == ID_PAGE && g_find_open) {
        uui_textbox_set_active(&g_find.field, 0);   // the page took the keyboard back
    }
    if (code > 0) command(code);
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    // The sign-in first, and every key is its: behind it is a page.
    if (uui_dialog_is_open(&g_dialog)) {
        uui_dialog_key(&g_dialog, key);
        int c = uui_dialog_take_code(&g_dialog);
        if (c > 0 && g_login_shown) login_answer(c);
        uapp_redraw(a);
        return;
    }
    struct uui_webview *v = active();
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
        if (v) uui_webview_reload(v);
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
    } else if (v && v->st.loading && key == 0x1B) {
        uui_webview_stop(v);
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

// Tooltips: the toolbars' hover is timed (uui_toolbar_tick()); and the
// session, saved once it has been still for a moment.
static int on_tick(struct uapp *a) {
    (void)a;
    if (g_session_dirty && sys_monotonic_ns() - g_session_saved_ns >= SESSION_SAVE_NS) session_save();
    return uui_toolbar_tick(&g_tools) | uui_toolbar_tick(&g_tools_r);
}

static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    uapp_log_layout(a, "browser");
    struct uui_webview *v = active();
    if (v)
        // The LENGTH, and at most 72 bytes of the address: a layout line
        // is 160 bytes, and one cut by vsnprintf() loses its newline and
        // runs into the next. A test reads the real length from `len`.
        uapp_logf_layout("browser: tab %d/%d loading %d extent %d %d scroll %d %d len %d loads %d "
                         "url %.72s\n", g_cur, g_ntabs, v->st.loading, v->st.ext_w, v->st.ext_h,
                         v->st.sx, v->st.sy, (int)strlen(v->url), v->st.loads, v->url);
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

static int g_weak;
static const char *g_first_url;   // a page named on the command line

// The tabs of last time, each unloaded on its page until it is shown --
// only the one in front starts a content process. 1 if there were any.
static int session_restore(void) {
    static struct ns_session_tab tabs[WIN_TABS_MAX];   // 50 entries each: off the stack
    int front, n = ns_session_load(STATE_DIR, tabs, WIN_TABS_MAX, &front);
    for (int i = 0; i < n; i++) {
        struct uui_webview *v = i ? new_tab() : active();
        if (v) uui_webview_history_set(v, tabs[i].e, tabs[i].n, tabs[i].at);
    }
    ns_session_free(tabs, n);
    if (n) {
        ulogf("browser: %d tabs restored\n", n);
        select_tab(front);
    }
    return n > 0;
}

static void on_open(struct uapp *a) {
    g_app = a;
    uui_menubar_set_bounds(&g_ctx, 0, 0, uapp_width(a), uapp_height(a));
    if (uui_webview_setup(a, POST_WEBVIEW) != 0) ulogf("browser: no page channel\n");
    if (!ns_net_start(a, POST_NET, g_weak)) ulogf("browser: no network -- local pages only\n");
    place_page();
    // A page asked for opens in a tab of its own beside the restored ones,
    // as Chrome and Firefox do.
    int restored = session_restore();
    if (g_first_url && restored) open_tab(g_first_url);
    else if (!restored) uui_webview_navigate(active(), g_first_url ? g_first_url : HOME_URL);
    uui_focus_set(&g_focus, 1);
}

static int on_close(struct uapp *a) {
    (void)a;
    session_save();
    ns_marks_fini();
    for (int i = 0; i < g_ntabs; i++) uui_webview_destroy(g_tabs[i]);
    return 1;
}

// A content process or the network process wrote (each has a thread that
// posts; the reading happens here, on the main thread).
static int on_user(struct uapp *a, int a0, int a1) {
    if (a0 == POST_WEBVIEW) return uui_webview_on_post(a, a0, a1);
    if (a0 == POST_NET) return ns_net_on_post();
    return 0;
}

int main(int argc, char **argv) {
    if (!ugfx_font_init()) return 2;
    // `--weak-entropy` is https on a test guest with no virtio-rng, as
    // wget's flag; `--search-url` points searches at a server on this
    // machine, `--upgrade-local` tries such a server over https first.
    // Anything else is a page to open.
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--weak-entropy")) g_weak = 1;
        else if (!strcmp(argv[i], "--upgrade-local")) g_upgrade_local = 1;
        else if (!strcmp(argv[i], "--search-url") && i + 1 < argc && strstr(argv[i + 1], "%s"))
            g_search = argv[++i];
        else if (argv[i][0] != '-') g_first_url = argv[i];
    }
    if (!ns_marks_init(STATE_DIR)) ulogf("browser: bookmarks could not be loaded\n");
    if (usiteperm_load(&g_perms, STATE_DIR "/permissions") != 0)
        ulogf("browser: site permissions could not be loaded\n");
    perms_push();   // before the first tab's content process says hello

    uui_toolbar_init(&g_tools, TOOLS, (int)(sizeof TOOLS / sizeof TOOLS[0]));
    g_tools.item_flags = tool_flags;
    uui_toolbar_init(&g_tools_r, TOOLS_R, (int)(sizeof TOOLS_R / sizeof TOOLS_R[0]));
    g_tools_r.item_flags = tool_flags;
    uui_textbox_init(&g_address, "");
    uui_textbox_set_max(&g_address, WEBVIEW_URL_MAX - 1);   // a URL, not a 127-byte value
    g_address.placeholder = "Type a web address or a path";
    uui_focus_init(&g_focus, g_focusables, (int)(sizeof g_focusables / sizeof g_focusables[0]));

    ROW[0] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &g_tools, .id = ID_TOOLS,
                                .name = "tools" };
    ns_addr_init(&g_addr, &g_address, ID_ADDRESS, "address");
    g_addr.show_star = 1;
    if (!new_tab()) return 1;   // after the address frame: showing a tab syncs it
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
    PAGE_ROW[1] = (struct uui_item){ .ops = &uui_webview_ops, .widget = active(), .id = ID_PAGE,
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
    g_widgets[W_ASK] = (struct uui_item){ .ops = &uui_permask_ops, .widget = &g_ask, .id = ID_ASK,
                                          .name = "ask" };
    ns_dlpanel_init(&g_dlp, ID_DLLIST);
    g_widgets[W_DLP] = (struct uui_item){ .ops = &ns_dlpanel_ops, .widget = &g_dlp, .id = ID_DLPANEL,
                                          .name = "dlpanel" };
    // LAST: a modal is offered every press first, and draws over all.
    uui_dialog_init(&g_dialog);
    g_widgets[W_DIALOG] = (struct uui_item){ .ops = &uui_dialog_ops, .widget = &g_dialog, .id = ID_DIALOG,
                                             .name = "dialog" };
    uui_menubar_init(&g_ctx, 0, 0);   // no bar of its own: open_below() supplies the rows
    uui_findbar_init(&g_find);
    g_find.pill = 1;

    struct uapp_desc desc = {
        .title        = "Web Browser",
        .app_id       = "browser",
        .on_size      = on_size,
        .layout       = &ROOT_L,
        .widgets      = g_widgets,
        .widget_count = W_COUNT,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_tab       = on_tab,
        .on_draw_over = on_draw_over,
        .on_tick      = on_tick,
        .on_user      = on_user,
        .tick_ms      = 250,
        .on_open      = on_open,
        .on_resize    = on_resize,
        .on_close     = on_close,
        .focus        = &g_focus,
        .flags        = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
