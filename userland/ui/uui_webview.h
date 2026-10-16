#ifndef UUI_WEBVIEW_H
#define UUI_WEBVIEW_H

// A WEB PAGE IN A WIDGET, drawn by another process: each view starts its
// own /bin/webcontent (NetSurf), which paints the page into shared memory
// this widget copies from, and hears the person's input as records
// (lib/webview_proto.h). docs/browser-design.md, stage 5. WKWebView,
// WebView2, Android's WebView -- an embeddable view is a CLIENT of an
// engine process, never the engine.
//
// THIS LINKS NO NETSURF. The engine is GPL-2.0 and parses hostile input;
// the widget is MIT and lives in libuapp, so Help, System Update's
// release notes and the Web Browser embed pages without either. A page
// that crashes the engine costs its process: the view keeps the last
// frame, greyed, under a "This page stopped." bar with Reload and View
// report (C3).
//
// WIRING: call uui_webview_setup() once, with a post id; hand every
// on_user post carrying that id to uui_webview_on_post(). The widget
// keeps a channel and a listener thread for all of this app's views.
#include <stdint.h>
#include "lib/webview_proto.h"
#include "ui/uui_infobar.h"
#include "ui/uui_sbar.h"
#include "ui/uui_widget.h"

struct uapp;
struct uui_webview;

// What changed, for host->changed().
enum {
    UUI_WV_TITLE   = 1 << 0,
    UUI_WV_URL     = 1 << 1,
    UUI_WV_STATE   = 1 << 2,   // loading, back/forward, zoom, scroll
    UUI_WV_SITE    = 1 << 3,
    UUI_WV_FIND    = 1 << 4,
    UUI_WV_STOPPED = 1 << 5,   // the engine died: the C3 view is up
    UUI_WV_STATUS  = 1 << 6,
    UUI_WV_LINKS   = 1 << 7,   // the page's links moved (v->link)
};

// The embedding app's side. Every member is optional.
struct uui_webview_host {
    void (*changed)(struct uui_webview *v, unsigned what);
    // A link asked for a new window: the app opens a tab (or not).
    void (*open_tab)(struct uui_webview *v, const char *url);
    // A page finished loading, for History.
    void (*visited)(struct uui_webview *v, const char *url, const char *title);
    // An http(s) link in a view with no network: the app hands it on.
    void (*external)(struct uui_webview *v, const char *url);
    // An app: URL to answer, with uui_webview_scheme_reply().
    void (*scheme)(struct uui_webview *v, uint32_t id, const char *url);
    // An upgraded navigation's https did not answer, and `host` was loaded
    // over http instead (uui_webview_navigate_upgraded()).
    void (*downgraded)(struct uui_webview *v, const char *host);
};

// One page of a view's back/forward list. 50 at most, Chrome's
// kMaxSessionHistoryEntries: the oldest falls off the front.
#define UUI_WV_HISTORY_MAX 50
struct uui_wv_entry {
    char *url, *title;   // malloc'd; title may be ""
    int sx, sy;          // where the page was scrolled when left
};

struct uui_webview {
    int x, y, w, h;
    const struct uui_webview_host *host;
    void *user;                    // the app's
    const char *net;               // the network process's channel; NULL: local pages only
    int scheme;                    // ask host->scheme for app: URLs

    // --- read-only to the app ---
    int pid;                       // the content process, 0 when none
    int stopped;                   // the engine died: C3
    int nomem;                     // ...because the page ran out of memory
    int unloaded;                  // let go to save memory; reloads when shown
    char title[256];
    char url[WEBVIEW_URL_MAX];
    char status[512];
    struct wv_state st;
    int can_back, can_forward;     // from hist[], below
    struct wv_site site;
    struct wv_find_result find;
    char report[96];               // the crash report, when the kernel wrote one
    // BACK/FORWARD IS KEPT HERE, NOT IN THE ENGINE, so a crashed or
    // unloaded page still has its history (lib/webview_proto.h, WV_COMMIT).
    // uui_webview_history_set() replaces it -- a restored session.
    struct uui_wv_entry hist[UUI_WV_HISTORY_MAX];
    int nhist, hist_at;

    // --- OWNED ---
    int ready;                     // the content process said hello
    int files;                     // ...was started for a local page (--files)
    int visible;
    unsigned long long shown_ns;   // when last shown, for unloading the oldest
    char *pending_nav;             // a URL to load once ready
    int pending_upgraded;          // ...as uui_webview_navigate_upgraded() asked
    int pending_go;                // ...or hist[hist_at], once ready
    int hist_live;                 // live_s* are hist[hist_at]'s page's
    int live_sx, live_sy;          // its scroll when last at rest
    uint32_t *copy;                // the page as last copied: what is drawn
    int cw, ch;
    const uint32_t *buf;           // the content process's buffer, mapped read-only
    int bw, bh;
    uint32_t gen;
    uint64_t bbytes;
    struct uui_sbar sb;
    struct uui_infobar bar;        // C3
    int focused, pressed, bar_pressed;
    int hover_x, hover_y;          // -1 when away
    int sent_w, sent_h;            // the size the content process was told
    struct uui_webview *next;      // every view of this app
    // The links in view, as the content process last reported them.
    struct { int x, y, w, h; char href[160]; } link[WV_LINKS_MAX];
    int nlinks;
};

// Once per app, before any view: opens this app's channel and starts its
// listener, which posts `post_id` (as a0) when a content process writes.
// 0, or -1.
int  uui_webview_setup(struct uapp *a, int post_id);
// From desc.on_user, for a post whose a0 is the setup's id. Repaints if
// it returns 1.
int  uui_webview_on_post(struct uapp *a, int a0, int a1);

// Is the engine installed? A NOGPL build has no /bin/webcontent; an app
// with another way to show its document uses that instead.
int  uui_webview_available(void);

void uui_webview_init(struct uui_webview *v, const struct uui_webview_host *host, void *user);
// Ends the content process and frees the view's memory.
void uui_webview_destroy(struct uui_webview *v);

// Loads `url`, starting the content process if the view has none.
void uui_webview_navigate(struct uui_webview *v, const char *url);
// An https URL made from a host typed without a scheme: one that cannot
// be reached at all falls back to http by itself (Chrome's HTTPS-Upgrades;
// lib/webview_proto.h, WV_NAVIGATE_UPGRADED), and host->downgraded hears.
void uui_webview_navigate_upgraded(struct uui_webview *v, const char *url);
// Reloads -- and restarts a stopped or unloaded view on its last URL.
void uui_webview_reload(struct uui_webview *v);
void uui_webview_stop(struct uui_webview *v);
// A step through the view's own list; works on a stopped or unloaded
// view too, which starts a content process for the page.
void uui_webview_back(struct uui_webview *v);
void uui_webview_forward(struct uui_webview *v);
// Replaces the view's list with copies of `n` entries, `at` current, and
// leaves the view unloaded on that page: it loads when shown.
void uui_webview_history_set(struct uui_webview *v, const struct uui_wv_entry *e, int n, int at);
// Notes where the current page is scrolled, so hist[] is up to date
// before the app saves it.
void uui_webview_history_snapshot(struct uui_webview *v);
void uui_webview_zoom(struct uui_webview *v, int dir);   // +1, -1, 0 resets
// Find in page: `dir` 0 searches afresh, +1/-1 step; the answer arrives
// as UUI_WV_FIND in v->find.
void uui_webview_find(struct uui_webview *v, const char *query, int dir);
void uui_webview_find_clear(struct uui_webview *v);
// about:crash, as a person typed it: the engine faults on purpose.
void uui_webview_crash(struct uui_webview *v);

// A tab brought forward (1) or sent behind (0). A view shown again after
// being unloaded reloads here.
void uui_webview_set_visible(struct uui_webview *v, int visible);
// The most content processes this app keeps; past it, the view shown
// longest ago is unloaded. 0 derives it from the machine's memory.
void uui_webview_set_limit(int max_live);
// The sites' JavaScript exceptions, for every view: "host\tallow|block\n"
// lines (lib/usiteperm.h's usiteperm_list()), copied. A page created after
// it follows it; a view's `site.js` and `site.scripts` say what its page
// did. NULL or "" is none -- the browser.javascript setting alone.
void uui_webview_set_perms(const char *list);

// The answer to host->scheme: `status` 200 with `mime` and the bytes, or
// 404 with none. May be called in pieces; `done` on the last.
void uui_webview_scheme_reply(struct uui_webview *v, uint32_t id, int status, const char *mime,
                              const void *data, uint32_t len, int done);

// A DOCUMENT PAGE from Markdown -- Help's, the release notes' -- in the
// theme's document look (the stage 5 canvas's H2: serif headings, a
// readable measure): lib/umd.h's HTML inside <html> with a stylesheet
// made from the live theme. Returns a malloc'd page and its length in
// `*len`, or NULL.
struct umd_html_opts;
char *uui_webview_markdown_page(const char *title, const char *md, int md_len,
                                const struct umd_html_opts *opts, int *len);

extern const struct uui_widget_ops uui_webview_ops;

#endif
