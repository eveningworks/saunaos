#ifndef ULIB_WEBVIEW_PROTO_H
#define ULIB_WEBVIEW_PROTO_H

#include <stdint.h>

// The contract between ui/uui_webview.h (in the embedding app) and
// /bin/webcontent (the engine, one process per view) --
// docs/browser-design.md, stage 5. Records over lib/ubulk.h: the APP is
// the server, on WEBVIEW_SERVICE_FMT, and each content process a client.
// A record's `id` is unused here (one view per content process); a
// string payload is its bytes WITHOUT a terminator.
//
// MIT, like the app: /bin/webcontent is GPL and includes this, which is
// the direction that is allowed.

#define WEBVIEW_SERVICE_FMT "wv%d"       // the embedding app's pid (no dot: lib/uchan.h)
#define WEBVIEW_BUF_FMT     "wvb.%d.%u"  // the content process's pid, a generation
#define WEBVIEW_VERSION     1
#define WEBVIEW_URL_MAX     8192         // a URL as long as browsers meet in practice

// --- app -> content --------------------------------------------------

enum {
    WV_NAVIGATE = 1,   // string: a URL, already made from what was typed
    WV_RELOAD,
    WV_STOP,
    WV_BACK,
    WV_FORWARD,
    WV_RESIZE,         // struct wv_size: the page area, without the scrollbar
    WV_MOUSE,          // struct wv_mouse
    WV_WHEEL,          // struct wv_wheel
    WV_KEY,            // struct wv_key
    WV_SCROLL_TO,      // struct wv_point: from the scrollbar, page pixels
    WV_ZOOM,           // struct wv_int: +1, -1, 0 to reset
    WV_FIND,           // struct wv_find then the query's bytes
    WV_FIND_CLEAR,
    WV_FRAME_ACK,      // struct wv_int: the FRAME's seq, copied out
    WV_SCHEME_DATA,    // id: the request; struct wv_scheme then bytes (see below)
    WV_CRASH,          // the about:crash a person TYPED (Chrome's chrome://crash)
    WV_VISIBLE,        // struct wv_int: 0 when the view's tab is in the background
};

// --- content -> app --------------------------------------------------

enum {
    WV_HELLO = 100,    // struct wv_int: WEBVIEW_VERSION
    WV_BUFFER,         // struct wv_buffer: a new pixel buffer to map
    WV_FRAME,          // struct wv_frame
    WV_TITLE,          // string
    WV_URL,            // string
    WV_STATUS,         // string: a hovered link, or empty
    WV_STATE,          // struct wv_state
    WV_SITE,           // struct wv_site
    WV_FIND_RESULT,    // struct wv_find_result
    WV_OPEN_TAB,       // string: a link that asked for a new window
    WV_VISITED,        // struct wv_visited then the URL's bytes
    WV_SCHEME_GET,     // id: the request; string: an app: URL to answer
    WV_EXTERNAL,       // string: an http(s) link in a view with no network
    WV_LINKS,          // the links in view: struct wv_link each, its href after it
};

// A link on the page, in VIEW pixels (outside the view when scrolled
// off) -- what a test clicks, and an accessibility tree's hyperlinks in
// miniature. Sent after a change, those in view first, at most
// WV_LINKS_MAX, `href_len` bytes of address after each.
#define WV_LINKS_MAX 64
struct wv_link { int32_t x, y, w, h, href_len; };

struct wv_int   { int32_t v; };
struct wv_size  { int32_t w, h, line; };   // line: the app's text line, for scroll steps
struct wv_point { int32_t x, y; };

// A pointer event, in VIEW pixels (the app does not know the scroll).
enum { WV_MOUSE_PRESS, WV_MOUSE_MOVE, WV_MOUSE_RELEASE, WV_MOUSE_LEAVE };
struct wv_mouse { int32_t kind, x, y; uint32_t mods, buttons; };
struct wv_wheel { int32_t x, y, notches; };     // x -1: not over the page
struct wv_key   { int32_t key; uint32_t mods; };
struct wv_find  { int32_t dir; };               // 0 afresh, +1 next, -1 previous

// A new buffer: "wvb.<pid>.<gen>", w x h 0xAARRGGBB words, stride w.
// The content process made it and granted it to the app.
struct wv_buffer { uint32_t gen; int32_t w, h; };

// What changed since the last FRAME. The app applies them IN ORDER:
// first moves its copy by (dx, dy) -- content that was at y moves to
// y - dy, a scroll -- then copies each rect from buffer `gen`. The
// content process paints nothing more into that buffer until the app
// answers WV_FRAME_ACK with `seq`.
#define WV_FRAME_RECTS 8
struct wv_rect { int32_t x, y, w, h; };
struct wv_frame {
    uint32_t seq, gen;
    int32_t dx, dy;
    int32_t nrects;
    struct wv_rect r[WV_FRAME_RECTS];
};

// The little things that change together, sent whole whenever any does.
struct wv_state {
    int32_t loading, can_back, can_forward;
    int32_t ext_w, ext_h;      // the laid-out page
    int32_t sx, sy;            // its scroll offset
    int32_t caret_x, caret_y, caret_h;   // view pixels; caret_h 0 = none
    int32_t pointer;           // WIN_CURSOR_*
    int32_t zoom;              // percent
};

// What the address field and the lock's card say (ns_site.h's states).
enum {
    WV_SITE_NONE, WV_SITE_INTERNAL, WV_SITE_FILE, WV_SITE_INSECURE,
    WV_SITE_OVERRIDE, WV_SITE_ISSUES, WV_SITE_SECURE,
};
struct wv_cert {
    char subject_cn[128], subject_org[128];
    char issuer_cn[128], issuer_org[128];
    int32_t nb_year, nb_mon, nb_day, na_year, na_mon, na_day;
};
struct wv_site {
    int32_t state;
    char host[128];
    char version[24], cipher[64];   // empty when unknown
    int32_t chain_depth;
    int32_t have_leaf;
    struct wv_cert leaf;
};

struct wv_find_result { int32_t current, matches; };

// A page the person reached, for History: once per load that finished.
struct wv_visited { int32_t title_len; };   // then the title's bytes, then the URL's

// An app: response, in pieces: the first carries the MIME type
// (`mime_len` bytes of it lead the data), the last has `done` set; a
// `status` other than 200 is an error with no body.
struct wv_scheme { int32_t status, done, mime_len; };

#endif
