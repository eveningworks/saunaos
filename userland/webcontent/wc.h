#ifndef WEBCONTENT_WC_H
#define WEBCONTENT_WC_H

// /bin/webcontent's own state, shared by its files -- the engine half
// of docs/browser-design.md's stage 5. ONE page per process: NetSurf's
// gui_window is a single static window, painted into a shm buffer the
// embedding app maps (lib/webview_proto.h), and told about the person's
// input by that app's records.
#include <stdint.h>
#include "lib/ubulk.h"
#include "lib/webview_proto.h"

struct browser_window;

#define WC_TITLE_MAX  256
#define WC_URL_MAX    8192
#define WC_STATUS_MAX 512

struct gui_window {
    struct browser_window *bw;
    int sx, sy;                    // scroll offset, page pixels
    int ext_w, ext_h;              // the laid-out page
    int caret_x, caret_y, caret_h; // page pixels; caret_h 0: none
    int loading;
    int pointer;                   // WIN_CURSOR_*
    char title[WC_TITLE_MAX];
    char url[WC_URL_MAX];
    char status[WC_STATUS_MAX];
};

// The process's one window, or NULL before NetSurf made it.
extern struct gui_window *g_wc_win;
// The embedding app (always) and the network process (only when the
// app gave one -- a Help view has none).
extern struct ubulk_client g_wc_app;
extern struct ubulk_client g_wc_net;
extern int g_wc_has_net;

// To the app. A failed send is a gone app, which the main loop notices.
void wc_send(uint32_t type, const void *data, uint32_t len);
void wc_send_str(uint32_t type, const char *s);
void wc_send_id(uint32_t type, uint32_t id, const void *data, uint32_t len);

// --- wc_window.c ---------------------------------------------------------
extern struct gui_window_table *wc_window_table;
extern struct gui_misc_table *wc_misc_table;
extern struct gui_fetch_table *wc_fetch_table;
// A record from the app about the page: input, navigation, a resize.
void wc_window_handle(const struct ubulk_msg *m);
// Paints what is dirty into the buffer and sends a FRAME, unless the last
// one is not acked yet; then sends the state and site if they changed.
void wc_window_flush(void);
// An upgraded navigation's https could not be reached: load its http twin
// (lib/webview_proto.h, WV_NAVIGATE_UPGRADED). Later, from the main loop.
void wc_window_downgrade(const char *https_url);

// --- wc_site.c -----------------------------------------------------------
void wc_site_get(struct gui_window *gw, struct wv_site *out);

// --- wc_net.c ------------------------------------------------------------
// The remote transport for backends/netsurf/fetch_http.c, over g_wc_net.
void wc_net_init(void);
// Drains the network process's records into the fetcher; called by its
// poll and by the main loop when the network rang.
void wc_net_pump(void);
// Tells the network process to save fetch `id`'s response as `name`.
void wc_net_download(uint32_t id, const char *name);
// The https URL of an upgraded navigation, or NULL for none: its fetch is
// flagged for webnet, and a failure before any header falls back.
void wc_net_upgrade(const char *url);

// --- wc_scheme.c ---------------------------------------------------------
// The app: scheme, answered by the embedding app.
int wc_scheme_register(void);
void wc_scheme_data(const struct ubulk_msg *m);

// --- wc_download.c -------------------------------------------------------
extern struct gui_download_table *wc_download_table;

// --- the page widget's zoom (wc_window.c) ---------------------------------
int wc_zoom(int dir);
int wc_zoom_pct(void);

#endif
