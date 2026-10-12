#ifndef BROWSER_NS_VIEW_H
#define BROWSER_NS_VIEW_H

// The Web Browser's page: NetSurf's gui_window (one per tab) and the
// widget that shows the active one -- draws it through ns_plot.h, scrolls
// it, and hands it the pointer and the keyboard.
//
// **IN-PROCESS FOR NOW, SHAPED TO MOVE.** docs/browser-design.md's stage
// 5 turns this widget into `uui_webview`, with the engine in a content
// process. Everything the app learns about a page arrives through the
// nsapp_*() hooks below rather than by reaching into NetSurf, so that
// move changes this file and not the app.
#include "ui/uui_sbar.h"
#include "ui/uui_widget.h"

struct browser_window;

#define NS_TITLE_MAX  256
// A URL as long as browsers meet in practice: a search results page or a
// signed download link runs past 2 KiB. Each tab is heap-allocated.
#define NS_URL_MAX    8192
#define NS_STATUS_MAX 512

struct gui_window {
    struct browser_window *bw;
    int sx, sy;                    // scroll offset, page pixels
    int ext_w, ext_h;              // the laid-out page
    int caret_x, caret_y, caret_h; // caret_h 0: none
    int loading;
    int pointer;                   // WIN_CURSOR_* for over the page
    char title[NS_TITLE_MAX];
    char url[NS_URL_MAX];
    char status[NS_STATUS_MAX];    // a hovered link's address, or ""
};

struct ns_view {
    int x, y, w, h;
    struct uui_sbar sb;
    int focused;
    int pressed, dragged;          // button 1 down on the page; moved far enough to drag
    int press_x, press_y;
    int hover_x, hover_y;          // last pointer position over the page, -1 when away
};

extern const struct uui_widget_ops ns_view_ops;
void ns_view_init(struct ns_view *v);

// The page area NetSurf lays out into: the view less its scrollbar strip.
int ns_view_page_w(const struct ns_view *v);

// Scrolls the active page by (dx, dy) page pixels, clamped. 1 if it moved.
int ns_view_scroll_by(struct ns_view *v, int dx, int dy);

// NetSurf's tables, for netsurf_register().
extern struct gui_window_table *ns_window_table;
extern struct gui_misc_table *ns_misc_table;
extern struct gui_fetch_table *ns_fetch_table;

// --- what the app provides (gui/apps/browser.c) -------------------------
//
// A window NetSurf made: `existing` is the window it was opened from (a
// link opened in a new tab), NULL for the first; `foreground` says to
// show it now rather than open it behind.
void nsapp_window_added(struct gui_window *gw, struct gui_window *existing, int foreground);
void nsapp_window_removed(struct gui_window *gw);
// The title, address, loading state or status of `gw` changed.
void nsapp_window_changed(struct gui_window *gw);
struct gui_window *nsapp_active(void);
struct ns_view *nsapp_view(void);
void nsapp_redraw(void);

#endif
