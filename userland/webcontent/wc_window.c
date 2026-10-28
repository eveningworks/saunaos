// The content process's one page: NetSurf's window, misc and fetch
// tables, painted into a shm buffer the embedding app maps, and driven by
// the app's records (lib/webview_proto.h).
//
// THE SURFACE (docs/browser-design.md, stage 5): a buffer the size of the
// view's page area, made here and granted to the app. A FRAME tells the
// app what moved (a scroll) and what to copy; nothing is painted again
// until the app acks it, so the app never copies a half-drawn frame. A
// scroll is a blit of what is already there plus a paint of the strip
// that came into view.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "utils/errors.h"
#include "webcontent/ns_find.h"
#include "webcontent/ns_plot.h"
#include "webcontent/ns_sched.h"
#include "webcontent/wc.h"
#include "lib/ufileurl.h"

#include "netsurf/browser_window.h"
#include "netsurf/fetch.h"
#include "netsurf/keypress.h"
#include "netsurf/misc.h"
#include "netsurf/mouse.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "utils/nsurl.h"
#include "content/hlcache.h"
#include "netsurf/content.h"
#include "content/handlers/html/box.h"
#include "content/handlers/html/box_inspect.h"
#include "content/handlers/html/private.h"
#include "desktop/browser_private.h"   // loading_parameters: where a load is going

#include "rt/sys.h"
#include "syscall_abi.h"
#include "ui/ugfx.h"
#include "win_proto.h"   // WIN_CURSOR_*
#include "keyboard.h"    // KEY_*

#define RESOURCE_DIR "/usr/share/netsurf/"
// Pointer movement, in pixels, that turns a press into a drag (a text
// selection); under it, the release is a click.
#define DRAG_SLOP 4

struct gui_window *g_wc_win;

// --- the buffer ------------------------------------------------------------

static struct {
    uint32_t *px;
    int w, h;              // also the view's page area
    uint64_t bytes;
    uint32_t gen;
    char name[32];
} g_buf;

static int g_visible = 1;
// The app's line of text, in pixels: what a wheel notch and an arrow key
// scroll by three of. The app's font, since this process has none of
// the session's.
static int g_line = 16;
static int g_await_ack;    // a FRAME the app has not copied yet
static uint32_t g_seq;
static int g_dx, g_dy;     // scrolled since the last FRAME, not yet blitted

// A WV_GO's load: its commit REPLACES the app's current entry instead of
// adding one, and once THAT load has finished the page scrolls to where
// the entry was left -- not at the first throbber stop, which a new
// process sees before its first page. A go that failed leaves g_going,
// so the next page committed replaces the entry that never loaded.
static int g_going, g_go_committed, g_restore, g_restore_x, g_restore_y;

static int scroll_to(struct gui_window *gw, int x, int y);

static void restore_scroll(void *p) {
    struct gui_window *gw = p;
    if (g_restore && gw == g_wc_win) scroll_to(gw, g_restore_x, g_restore_y);
    g_restore = 0;
}

// What must be painted, in VIEW pixels; x1 <= x0 for nothing.
static int g_dirty_x0, g_dirty_y0, g_dirty_x1, g_dirty_y1;

static void dirty(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_buf.w) x1 = g_buf.w;
    if (y1 > g_buf.h) y1 = g_buf.h;
    if (x1 <= x0 || y1 <= y0) return;
    if (g_dirty_x1 <= g_dirty_x0) {
        g_dirty_x0 = x0, g_dirty_y0 = y0, g_dirty_x1 = x1, g_dirty_y1 = y1;
        return;
    }
    if (x0 < g_dirty_x0) g_dirty_x0 = x0;
    if (y0 < g_dirty_y0) g_dirty_y0 = y0;
    if (x1 > g_dirty_x1) g_dirty_x1 = x1;
    if (y1 > g_dirty_y1) g_dirty_y1 = y1;
}

// The links in view are worth telling the app about again.
static int g_links_dirty;

static void dirty_all(void) {
    dirty(0, 0, g_buf.w, g_buf.h);
    g_links_dirty = 1;
}

// A new buffer at w x h, granted to the app and announced. The old one's
// name and this process's mapping go now; the app's mapping keeps its
// pages until the app lets go.
static int buffer_make(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    uint64_t bytes = ((uint64_t)w * (uint64_t)h * 4 + 4095) & ~4095ull;
    char nm[32];
    snprintf(nm, sizeof nm, WEBVIEW_BUF_FMT, sys_getpid(), g_buf.gen + 1);
    sys_shm_unlink(nm);
    int fd = sys_shm_open(nm, bytes, SHM_CREATE | SHM_EXCL);
    if (fd < 0) return 0;
    sys_shm_grant(nm, g_wc_app.pg->server_pid);
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) {
        sys_shm_unlink(nm);
        return 0;
    }
    if (g_buf.px) {
        sys_munmap(g_buf.px, g_buf.bytes);
        sys_shm_unlink(g_buf.name);
    }
    g_buf.px = p;
    g_buf.w = w;
    g_buf.h = h;
    g_buf.bytes = bytes;
    g_buf.gen++;
    snprintf(g_buf.name, sizeof g_buf.name, "%s", nm);
    struct wv_buffer b = { g_buf.gen, w, h };
    wc_send(WV_BUFFER, &b, sizeof b);
    g_dx = g_dy = 0;
    g_dirty_x1 = g_dirty_x0;
    dirty_all();
    return 1;
}

// --- scrolling ---------------------------------------------------------------

static void clamp_scroll(struct gui_window *gw) {
    int max_x = gw->ext_w - g_buf.w, max_y = gw->ext_h - g_buf.h;
    if (gw->sx > max_x) gw->sx = max_x;
    if (gw->sy > max_y) gw->sy = max_y;
    if (gw->sx < 0) gw->sx = 0;
    if (gw->sy < 0) gw->sy = 0;
}

// Moves the view to (x, y), clamped. What was on screen is blitted at the
// next paint; only the strips that came into view are dirtied. 1 if it moved.
static int scroll_to(struct gui_window *gw, int x, int y) {
    int ox = gw->sx, oy = gw->sy;
    gw->sx = x;
    gw->sy = y;
    clamp_scroll(gw);
    int dx = gw->sx - ox, dy = gw->sy - oy;
    if (!dx && !dy) return 0;
    // The pending damage moves with the content it covers.
    if (g_dirty_x1 > g_dirty_x0) {
        g_dirty_x0 -= dx, g_dirty_x1 -= dx;
        g_dirty_y0 -= dy, g_dirty_y1 -= dy;
        int x0 = g_dirty_x0, y0 = g_dirty_y0, x1 = g_dirty_x1, y1 = g_dirty_y1;
        g_dirty_x1 = g_dirty_x0;
        dirty(x0, y0, x1, y1);
    }
    g_dx += dx;
    g_dy += dy;
    g_links_dirty = 1;
    if (dy > 0) dirty(0, g_buf.h - dy, g_buf.w, g_buf.h);
    if (dy < 0) dirty(0, 0, g_buf.w, -dy);
    if (dx > 0) dirty(g_buf.w - dx, 0, g_buf.w, g_buf.h);
    if (dx < 0) dirty(0, 0, -dx, g_buf.h);
    return 1;
}

static int scroll_by(struct gui_window *gw, int dx, int dy) {
    return scroll_to(gw, gw->sx + dx, gw->sy + dy);
}

// The pending scroll applied to the buffer itself: rows moved in place.
static void blit_pending(void) {
    int dx = g_dx, dy = g_dy;
    g_dx = g_dy = 0;
    if (!dx && !dy) return;
    if (abs(dx) >= g_buf.w || abs(dy) >= g_buf.h) {
        dirty_all();
        return;
    }
    int w = g_buf.w - abs(dx), h = g_buf.h - abs(dy);
    int sx = dx > 0 ? dx : 0, dxp = dx > 0 ? 0 : -dx;
    int sy = dy > 0 ? dy : 0, dyp = dy > 0 ? 0 : -dy;
    uint32_t *px = g_buf.px;
    if (dy > 0) {
        for (int r = 0; r < h; r++)
            memmove(px + (size_t)(dyp + r) * (size_t)g_buf.w + dxp,
                    px + (size_t)(sy + r) * (size_t)g_buf.w + sx, (size_t)w * 4);
    } else {
        for (int r = h - 1; r >= 0; r--)
            memmove(px + (size_t)(dyp + r) * (size_t)g_buf.w + dxp,
                    px + (size_t)(sy + r) * (size_t)g_buf.w + sx, (size_t)w * 4);
    }
}

// --- painting ----------------------------------------------------------------

// Title and URL changed since the last flush: sent there, BEFORE the
// state, so the app never sees a new address beside the old page's
// "loaded" state and extent -- one snapshot per flush.
static int g_title_dirty, g_url_dirty;
static struct wv_state g_sent_state;
static struct wv_site g_sent_site;
static int g_state_valid, g_site_valid;
static char g_sent_status[WC_STATUS_MAX];

static void paint(void) {
    struct gui_window *gw = g_wc_win;
    if (g_await_ack || !g_buf.px || !g_visible) return;
    int dx = g_dx, dy = g_dy;
    if (!dx && !dy && g_dirty_x1 <= g_dirty_x0) return;
    blit_pending();
    struct wv_frame f;
    memset(&f, 0, sizeof f);
    f.seq = ++g_seq;
    f.gen = g_buf.gen;
    f.dx = dx;
    f.dy = dy;
    if (g_dirty_x1 > g_dirty_x0) {
        int x0 = g_dirty_x0, y0 = g_dirty_y0, x1 = g_dirty_x1, y1 = g_dirty_y1;
        struct ugfx_surface s = ugfx_surface_for_pixels(g_buf.px, g_buf.w, g_buf.h);
        // White under the page: an unstyled page's canvas, and what
        // shows before the first layout.
        ugfx_fill_rect(&s, x0, y0, x1 - x0, y1 - y0, ugfx_rgb(255, 255, 255));
        if (gw && gw->bw) {
            struct ns_target t = { &s, x0, y0, x1, y1 };
            struct redraw_context ctx;
            ns_plot_context(&ctx, &t);
            struct rect clip = { x0, y0, x1, y1 };
            browser_window_redraw(gw->bw, -gw->sx, -gw->sy, &clip, &ctx);
        }
        f.nrects = 1;
        f.r[0] = (struct wv_rect){ x0, y0, x1 - x0, y1 - y0 };
        g_dirty_x1 = g_dirty_x0;
    }
    wc_send(WV_FRAME, &f, sizeof f);
    g_await_ack = 1;
}

// The page's link boxes, by a walk of the laid-out tree -- each text box
// inside an <a>, where box_coords() puts it, scaled by the zoom: those in
// view first, then the rest, up to WV_LINKS_MAX.
static void send_links(struct gui_window *gw) {
    static char buf[UBULK_REC_MAX], later[UBULK_REC_MAX];
    static uint32_t last_hash;
    struct hlcache_handle *h = browser_window_get_content(gw->bw);
    if (!h || content_get_type(h) != CONTENT_HTML) return;
    html_content *html = (html_content *)hlcache_handle_get_content(h);
    struct box *b = html ? html->layout : NULL;
    float scale = browser_window_get_scale(gw->bw);
    size_t n = 0, nl = 0;
    int count = 0;
    while (b && count < WV_LINKS_MAX) {
        if (b->href && (b->text || b->object) && b->width > 0 && b->height > 0) {
            int x, y;
            box_coords(b, &x, &y);
            struct wv_link l = { (int)(x * scale) - gw->sx, (int)(y * scale) - gw->sy,
                                 (int)(b->width * scale), (int)(b->height * scale), 0 };
            int seen = l.x + l.w > 0 && l.y + l.h > 0 && l.x < g_buf.w && l.y < g_buf.h;
            const char *u = nsurl_access(b->href);
            size_t ul = strlen(u);
            if (ul > 512) ul = 512;
            l.href_len = (int32_t)ul;
            char *to = seen ? buf : later;
            size_t *at = seen ? &n : &nl;
            if (*at + sizeof l + ul <= sizeof buf) {
                memcpy(to + *at, &l, sizeof l);
                memcpy(to + *at + sizeof l, u, ul);
                *at += sizeof l + ul;
                count++;
            }
        }
        // Depth first: children, then the next sibling, then up.
        if (b->children) b = b->children;
        else {
            while (b && !b->next) b = b->parent;
            if (b) b = b->next;
        }
    }
    if (n + nl > sizeof buf) nl = sizeof buf - n;
    memcpy(buf + n, later, nl);
    n += nl;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; i++) hash = (hash ^ (uint8_t)buf[i]) * 16777619u;
    if (hash == last_hash) return;
    last_hash = hash;
    wc_send(WV_LINKS, buf, (uint32_t)n);
}

void wc_window_flush(void) {
    paint();
    struct gui_window *gw = g_wc_win;
    if (!gw) return;
    if (g_url_dirty) wc_send_str(WV_URL, gw->url);
    if (g_title_dirty) wc_send_str(WV_TITLE, gw->title);
    g_url_dirty = g_title_dirty = 0;
    struct wv_state st;
    memset(&st, 0, sizeof st);
    st.loading = gw->loading;
    st.ext_w = gw->ext_w;
    st.ext_h = gw->ext_h;
    st.sx = gw->sx;
    st.sy = gw->sy;
    st.caret_x = gw->caret_x - gw->sx;
    st.caret_y = gw->caret_y - gw->sy;
    st.caret_h = gw->caret_h;
    st.pointer = gw->pointer;
    st.zoom = wc_zoom_pct();
    st.loads = gw->loads;
    if (!g_state_valid || memcmp(&st, &g_sent_state, sizeof st)) {
        g_sent_state = st;
        g_state_valid = 1;
        wc_send(WV_STATE, &st, sizeof st);
    }
    if (strcmp(gw->status, g_sent_status)) {
        snprintf(g_sent_status, sizeof g_sent_status, "%s", gw->status);
        wc_send_str(WV_STATUS, gw->status);
    }
    if (g_links_dirty && !gw->loading && gw->bw && !g_await_ack) {
        g_links_dirty = 0;
        send_links(gw);
    }
    static struct wv_site site;   // 600 bytes: off the ring-3 frame
    wc_site_get(gw, &site);
    if (!g_site_valid || memcmp(&site, &g_sent_site, sizeof site)) {
        g_sent_site = site;
        g_site_valid = 1;
        wc_send(WV_SITE, &site, sizeof site);
    }
}

// --- the window table ------------------------------------------------------

// The URL a window is loading or showing, or NULL.
static const char *loading_url(struct browser_window *bw) {
    if (bw->loading_parameters.url) return nsurl_access(bw->loading_parameters.url);
    if (bw->loading_content) return nsurl_access(hlcache_handle_get_url(bw->loading_content));
    return NULL;
}

// A SECOND WINDOW -- a link that opens a new one -- is the APP's tab to
// make, in a process of its own. NetSurf creates the window and THEN
// navigates it (browser_window.c's target handling), both inside one
// call, so the address is known a moment later: a scheduled look reads
// where it is going, tells the app, and closes it.
struct shadow { struct gui_window *gw; int tries; };

static void shadow_check(void *p) {
    struct shadow *sh = p;
    const char *url = sh->gw->bw ? loading_url(sh->gw->bw) : NULL;
    if (!url && ++sh->tries < 10) {
        ns_schedule(10, shadow_check, sh);
        return;
    }
    if (url) wc_send_str(WV_OPEN_TAB, url);
    if (sh->gw->bw) browser_window_destroy(sh->gw->bw);   // gw_destroy() frees the window
    free(sh);
}

static struct gui_window *gw_create(struct browser_window *bw, struct gui_window *existing,
                                    gui_window_create_flags flags) {
    (void)existing;
    (void)flags;
    struct gui_window *gw = calloc(1, sizeof *gw);
    if (!gw) return NULL;
    gw->bw = bw;
    gw->pointer = WIN_CURSOR_DEFAULT;
    if (!g_wc_win) {
        g_wc_win = gw;
        return gw;
    }
    struct shadow *sh = calloc(1, sizeof *sh);
    if (!sh) { free(gw); return NULL; }
    sh->gw = gw;
    ns_schedule(0, shadow_check, sh);
    return gw;
}

static void gw_destroy(struct gui_window *gw) {
    if (gw == g_wc_win) g_wc_win = NULL;
    free(gw);
}

static nserror gw_invalidate(struct gui_window *gw, const struct rect *r) {
    (void)r;   // the whole view: a page is redrawn from its own content cache
    if (gw == g_wc_win) dirty_all();
    return NSERROR_OK;
}

static bool gw_get_scroll(struct gui_window *gw, int *sx, int *sy) {
    *sx = gw->sx;
    *sy = gw->sy;
    return true;
}

static nserror gw_set_scroll(struct gui_window *gw, const struct rect *r) {
    if (gw == g_wc_win) scroll_to(gw, r->x0, r->y0);
    return NSERROR_OK;
}

static nserror gw_get_dimensions(struct gui_window *gw, int *w, int *h) {
    (void)gw;
    *w = g_buf.w;
    *h = g_buf.h;
    return NSERROR_OK;
}

static char g_visited_url[WC_URL_MAX];

// A load that finished is a page for History: its title and address.
static void report_visited(struct gui_window *gw) {
    if (!gw->url[0] || !strcmp(gw->url, g_visited_url)) return;
    if (strncmp(gw->url, "http:", 5) && strncmp(gw->url, "https:", 6) && strncmp(gw->url, "file:", 5))
        return;
    snprintf(g_visited_url, sizeof g_visited_url, "%s", gw->url);
    static char buf[sizeof(struct wv_visited) + WC_TITLE_MAX + WC_URL_MAX];
    struct wv_visited v = { (int32_t)strlen(gw->title) };
    size_t ul = strlen(gw->url);
    memcpy(buf, &v, sizeof v);
    memcpy(buf + sizeof v, gw->title, (size_t)v.title_len);
    memcpy(buf + sizeof v + (size_t)v.title_len, gw->url, ul);
    wc_send(WV_VISITED, buf, (uint32_t)(sizeof v + (size_t)v.title_len + ul));
}

static nserror gw_event(struct gui_window *gw, enum gui_window_event ev) {
    if (gw != g_wc_win) return NSERROR_OK;   // a window on its way to being a tab
    switch (ev) {
    case GW_EVENT_UPDATE_EXTENT:
        browser_window_get_extents(gw->bw, true, &gw->ext_w, &gw->ext_h);
        clamp_scroll(gw);
        g_links_dirty = 1;
        break;
    case GW_EVENT_REMOVE_CARET:
        gw->caret_h = 0;
        break;
    case GW_EVENT_START_THROBBER:
        gw->loading = 1;
        // A view with no network (Help) cannot load the web: the app is
        // told, and the page stays where it was.
        if (!g_wc_has_net && gw == g_wc_win) {
            const char *url = loading_url(gw->bw);
            if (url && (!strncmp(url, "http:", 5) || !strncmp(url, "https:", 6))) {
                wc_send_str(WV_EXTERNAL, url);
                browser_window_stop(gw->bw);
            }
        }
        // A process started for a LOCAL page can read files through the
        // app; a page it leaves for the web must not inherit that, so the
        // app loads it in a fresh process (Chrome's process swap). Before
        // a byte of the new page is parsed: the load is stopped here.
        if (g_wc_files && gw == g_wc_win) {
            const char *url = loading_url(gw->bw);
            if (url && strncmp(url, "file:", 5) && strncmp(url, "about:", 6) &&
                strncmp(url, "resource:", 9) && strncmp(url, "data:", 5)) {
                wc_send_str(WV_SWAP, url);
                browser_window_stop(gw->bw);
            }
        }
        break;
    case GW_EVENT_STOP_THROBBER:
        gw->loading = 0;
        g_links_dirty = 1;
        if (gw == g_wc_win) {
            gw->loads++;
            gw->js = wc_js_allowed(gw->url);
            gw->scripts = wc_js_count_scripts(gw->bw);
        }
        // NetSurf sets the busy pointer for a load and leaves resetting
        // it to the next motion; one held still would keep the ring.
        if (gw->pointer == WIN_CURSOR_WAIT) gw->pointer = WIN_CURSOR_DEFAULT;
        report_visited(gw);
        // NetSurf sets ITS history's scroll right after this event, so
        // the app's entry's goes in a moment later.
        if (g_restore && g_go_committed) ns_schedule(0, restore_scroll, gw);
        g_go_committed = 0;
        break;
    case GW_EVENT_NEW_CONTENT:
    case GW_EVENT_SCROLL_START:
        gw->sx = gw->sy = 0;
        g_dx = g_dy = 0;
        dirty_all();
        break;
    default:
        break;
    }
    return NSERROR_OK;
}

static void gw_set_title(struct gui_window *gw, const char *title) {
    snprintf(gw->title, sizeof gw->title, "%s", title ? title : "");
    if (gw == g_wc_win) g_title_dirty = 1;
}

static nserror gw_set_url(struct gui_window *gw, struct nsurl *url) {
    snprintf(gw->url, sizeof gw->url, "%s", nsurl_access(url));
    if (gw == g_wc_win) g_url_dirty = 1;
    return NSERROR_OK;
}

// NetSurf's status line carries progress ("Fetching, Loading") as well
// as a hovered link; only a LINK is shown over the page, as browsers do
// (the rest is the throbber's job), and a link is what has a scheme.
static void gw_set_status(struct gui_window *gw, const char *text) {
    const char *t = text ? text : "";
    if (!strstr(t, "://") && strncmp(t, "about:", 6) && strncmp(t, "mailto:", 7)) t = "";
    snprintf(gw->status, sizeof gw->status, "%s", t);
}

static void gw_set_pointer(struct gui_window *gw, enum gui_pointer_shape shape) {
    int c = WIN_CURSOR_DEFAULT;
    switch (shape) {
    case GUI_POINTER_POINT:       c = WIN_CURSOR_HAND; break;
    case GUI_POINTER_CARET:       c = WIN_CURSOR_TEXT; break;
    case GUI_POINTER_WAIT:
    case GUI_POINTER_PROGRESS:    c = WIN_CURSOR_WAIT; break;
    case GUI_POINTER_MOVE:        c = WIN_CURSOR_MOVE; break;
    case GUI_POINTER_CROSS:       c = WIN_CURSOR_CROSSHAIR; break;
    case GUI_POINTER_NO_DROP:
    case GUI_POINTER_NOT_ALLOWED: c = WIN_CURSOR_NOT_ALLOWED; break;
    case GUI_POINTER_LEFT:
    case GUI_POINTER_RIGHT:       c = WIN_CURSOR_RESIZE_H; break;
    case GUI_POINTER_UP:
    case GUI_POINTER_DOWN:        c = WIN_CURSOR_RESIZE_V; break;
    default:                      break;
    }
    gw->pointer = c;
}

static void gw_place_caret(struct gui_window *gw, int x, int y, int h, const struct rect *clip) {
    (void)clip;
    gw->caret_x = x;
    gw->caret_y = y;
    gw->caret_h = h;
}

static struct gui_window_table g_window = {
    .create = gw_create,
    .destroy = gw_destroy,
    .invalidate = gw_invalidate,
    .get_scroll = gw_get_scroll,
    .set_scroll = gw_set_scroll,
    .get_dimensions = gw_get_dimensions,
    .event = gw_event,
    .set_title = gw_set_title,
    .set_url = gw_set_url,
    .set_status = gw_set_status,
    .set_pointer = gw_set_pointer,
    .place_caret = gw_place_caret,
};
struct gui_window_table *wc_window_table = &g_window;

// --- misc and fetch -------------------------------------------------------

static struct gui_misc_table g_misc = {
    .schedule = ns_schedule,
};
struct gui_misc_table *wc_misc_table = &g_misc;

// What a file: URL is, by its extension -- what monkey and every other
// front end answer from a table; NetSurf sniffs the content where it may.
// THE SAME TABLE the app uses for the local files it reads for us
// (lib/ufileurl.h), so a page and its own subresources are one type.
static const char *ft_filetype(const char *path) { return ufileurl_mime(path); }

// resource:<name> -- NetSurf's own stylesheets and pages, at
// /usr/share/netsurf (docs/filesystem-layout.md).
static struct nsurl *ft_resource_url(const char *path) {
    char full[512];
    struct stat st;
    snprintf(full, sizeof full, RESOURCE_DIR "%s", path);
    if (stat(full, &st) != 0) return NULL;
    char url[560];
    snprintf(url, sizeof url, "file://%s", full);
    struct nsurl *u = NULL;
    return nsurl_create(url, &u) == NSERROR_OK ? u : NULL;
}

static struct gui_fetch_table g_fetch = {
    .filetype = ft_filetype,
    .get_resource_url = ft_resource_url,
};
struct gui_fetch_table *wc_fetch_table = &g_fetch;

// --- zoom ------------------------------------------------------------------

static const int ZOOM_STEPS[] = { 25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200,
                                   250, 300, 400, 500 };
#define ZOOM_COUNT ((int)(sizeof ZOOM_STEPS / sizeof ZOOM_STEPS[0]))

int wc_zoom_pct(void) {
    if (!g_wc_win || !g_wc_win->bw) return 100;
    return (int)(browser_window_get_scale(g_wc_win->bw) * 100.0f + 0.5f);
}

int wc_zoom(int dir) {
    if (!g_wc_win || !g_wc_win->bw) return 100;
    int pct = wc_zoom_pct(), to = 100;
    if (dir > 0) {
        to = ZOOM_STEPS[ZOOM_COUNT - 1];
        for (int i = 0; i < ZOOM_COUNT; i++)
            if (ZOOM_STEPS[i] > pct) { to = ZOOM_STEPS[i]; break; }
    } else if (dir < 0) {
        to = ZOOM_STEPS[0];
        for (int i = ZOOM_COUNT - 1; i >= 0; i--)
            if (ZOOM_STEPS[i] < pct) { to = ZOOM_STEPS[i]; break; }
    }
    if (to != pct) browser_window_set_scale(g_wc_win->bw, (float)to / 100.0f, true);
    return to;
}

// --- input -----------------------------------------------------------------

static struct {
    int pressed, dragged;
    int press_x, press_y;      // view pixels
} g_ptr;

// Is there TEXT under the page point (px, py)? NetSurf leaves plain
// text to the window's default arrow (BROWSER_POINTER_AUTO), where
// Chrome and Firefox show the I-beam that says it can be selected; the
// deepest box under the point says, as NetSurf's own hit tests ask it.
static int over_text(struct gui_window *gw, int px, int py) {
    struct hlcache_handle *h = browser_window_get_content(gw->bw);
    if (!h) return 0;
    content_type type = content_get_type(h);
    if (type == CONTENT_TEXTPLAIN) return 1;
    if (type != CONTENT_HTML) return 0;
    html_content *html = (html_content *)hlcache_handle_get_content(h);
    struct box *box = html ? html->layout : NULL;
    if (!box) return 0;
    float scale = browser_window_get_scale(gw->bw);
    int x = (int)(px / scale), y = (int)(py / scale), bx = 0, by = 0, text = 0;
    while ((box = box_at_point(&html->unit_len_ctx, box, x, y, &bx, &by)))
        text = box->text != NULL && box->object == NULL;
    return text;
}

static void on_mouse(struct gui_window *gw, const struct wv_mouse *m) {
    int px = m->x + gw->sx, py = m->y + gw->sy;
    switch (m->kind) {
    case WV_MOUSE_PRESS:
        g_ptr.pressed = 1;
        g_ptr.dragged = 0;
        g_ptr.press_x = m->x;
        g_ptr.press_y = m->y;
        browser_window_mouse_click(gw->bw, BROWSER_MOUSE_PRESS_1, px, py);
        break;
    case WV_MOUSE_MOVE: {
        if (g_ptr.pressed && !g_ptr.dragged &&
            (abs(m->x - g_ptr.press_x) > DRAG_SLOP || abs(m->y - g_ptr.press_y) > DRAG_SLOP)) {
            browser_window_mouse_click(gw->bw, BROWSER_MOUSE_DRAG_1, g_ptr.press_x + gw->sx,
                                       g_ptr.press_y + gw->sy);
            g_ptr.dragged = 1;
        }
        int state = g_ptr.dragged ? (BROWSER_MOUSE_DRAG_ON | BROWSER_MOUSE_HOLDING_1) : 0;
        browser_window_mouse_track(gw->bw, state, px, py);
        if (gw->pointer == WIN_CURSOR_DEFAULT && over_text(gw, px, py)) gw->pointer = WIN_CURSOR_TEXT;
        break;
    }
    case WV_MOUSE_RELEASE:
        if (!g_ptr.pressed) break;
        g_ptr.pressed = 0;
        if (!g_ptr.dragged) browser_window_mouse_click(gw->bw, BROWSER_MOUSE_CLICK_1, px, py);
        else browser_window_mouse_track(gw->bw, 0, px, py);
        g_ptr.dragged = 0;
        break;
    case WV_MOUSE_LEAVE:
        gw->status[0] = '\0';
        break;
    }
}

static void on_wheel(struct gui_window *gw, const struct wv_wheel *w) {
    int step = g_line * 3;
    // A box under the pointer that scrolls (an overflow:auto div, a
    // textarea) takes the wheel first, as everywhere on the web.
    if (w->x >= 0 &&
        browser_window_scroll_at_point(gw->bw, w->x + gw->sx, w->y + gw->sy, 0, -w->notches * step))
        return;
    scroll_by(gw, 0, -w->notches * step);
}

// SaunaOS's keys as NetSurf's. 0 for one it has no code for.
static uint32_t ns_key(int key, unsigned mods) {
    switch (key) {
    case '\b':            return NS_KEY_DELETE_LEFT;
    case KEY_DELETE:      return NS_KEY_DELETE_RIGHT;
    case '\n': case '\r': return NS_KEY_NL;
    case '\t':            return (mods & KEY_MOD_SHIFT) ? NS_KEY_SHIFT_TAB : NS_KEY_TAB;
    case 0x1B:            return NS_KEY_ESCAPE;
    case KEY_ARROW_LEFT:  return (mods & KEY_MOD_CTRL) ? NS_KEY_WORD_LEFT : NS_KEY_LEFT;
    case KEY_ARROW_RIGHT: return (mods & KEY_MOD_CTRL) ? NS_KEY_WORD_RIGHT : NS_KEY_RIGHT;
    case KEY_ARROW_UP:    return NS_KEY_UP;
    case KEY_ARROW_DOWN:  return NS_KEY_DOWN;
    case KEY_HOME:        return (mods & KEY_MOD_CTRL) ? NS_KEY_TEXT_START : NS_KEY_LINE_START;
    case KEY_END:         return (mods & KEY_MOD_CTRL) ? NS_KEY_TEXT_END : NS_KEY_LINE_END;
    case KEY_PAGE_UP:     return NS_KEY_PAGE_UP;
    case KEY_PAGE_DOWN:   return NS_KEY_PAGE_DOWN;
    }
    // Ctrl+A/C/V/X/Z/Y arrive as their control codes, which are
    // NetSurf's own for select-all, copy, paste, cut, undo and redo.
    if (key == 1 || key == 3 || key == 22 || key == 24) return (uint32_t)key;
    if (key == 26) return NS_KEY_UNDO;
    if (key == 25) return NS_KEY_REDO;
    if (IS_PRINTABLE_KEY(key)) return (uint32_t)key;
    // Past Latin-1 a key IS its codepoint (NetSurf takes UCS-4), except
    // the Private Use Area the special keys live in and the surrogates --
    // ui/uui_widget.h's uui_key_is_text(), which this process does not link.
    if (key > 0xFF && key <= 0x10FFFF && !(key >= 0xD800 && key <= 0xDFFF) &&
        !(key >= 0xE000 && key <= 0xF8FF))
        return (uint32_t)key;
    return 0;
}

// The page first; what it does not take scrolls, and Esc stops a load --
// here rather than in the app, since a form field may want Esc.
static void on_key(struct gui_window *gw, const struct wv_key *k) {
    uint32_t nk = ns_key(k->key, k->mods);
    if (nk && browser_window_key_press(gw->bw, nk)) return;
    int line = g_line * 3, page = g_buf.h - g_line * 2;
    switch (k->key) {
    case KEY_ARROW_UP:    scroll_by(gw, 0, -line); break;
    case KEY_ARROW_DOWN:  scroll_by(gw, 0, line); break;
    case KEY_ARROW_LEFT:  scroll_by(gw, -line, 0); break;
    case KEY_ARROW_RIGHT: scroll_by(gw, line, 0); break;
    case KEY_PAGE_UP:     scroll_by(gw, 0, -page); break;
    case KEY_PAGE_DOWN:
    case ' ':             scroll_by(gw, 0, (k->mods & KEY_MOD_SHIFT) ? -page : page); break;
    case KEY_HOME:        scroll_to(gw, gw->sx, 0); break;
    case KEY_END:         scroll_to(gw, gw->sx, gw->ext_h); break;
    case 0x1B:            if (gw->loading) browser_window_stop(gw->bw); break;
    }
}

// A string record's bytes as a C string, cut to `cap`.
static void str_of(const struct ubulk_msg *m, size_t skip, char *out, size_t cap) {
    size_t n = m->len > skip ? m->len - skip : 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, (const char *)m->data + skip, n);
    out[n] = '\0';
}

// --- back/forward: the app's list (lib/webview_proto.h, WV_COMMIT) ---------------

// NetSurf adding a page to ITS history is the moment a page is committed;
// a link-time hook (as box_coords), never an edit to the vendored source.
nserror __real_browser_window_history_add(struct browser_window *bw, struct hlcache_handle *content,
                                          lwc_string *frag_id);
nserror __wrap_browser_window_history_add(struct browser_window *bw, struct hlcache_handle *content,
                                          lwc_string *frag_id) {
    nserror err = __real_browser_window_history_add(bw, content, frag_id);
    struct gui_window *gw = g_wc_win;
    if (!gw || bw != gw->bw || !content) return err;
    struct nsurl *u = hlcache_handle_get_url(content), *full = NULL;
    if (frag_id && nsurl_refragment(u, frag_id, &full) == NSERROR_OK) u = full;
    static char buf[sizeof(struct wv_commit) + WC_URL_MAX];
    struct wv_commit c = { g_going };
    size_t n = strlen(nsurl_access(u));
    if (n > WC_URL_MAX) n = WC_URL_MAX;
    memcpy(buf, &c, sizeof c);
    memcpy(buf + sizeof c, nsurl_access(u), n);
    wc_send(WV_COMMIT, buf, (uint32_t)(sizeof c + n));
    if (full) nsurl_unref(full);
    if (!g_going) g_restore = 0;
    g_go_committed = g_going;
    g_going = 0;
    return err;
}

static void navigate(const char *url) {
    struct gui_window *gw = g_wc_win;
    struct nsurl *u;
    if (nsurl_create(url, &u) != NSERROR_OK) return;
    if (gw && gw->bw)
        browser_window_navigate(gw->bw, u, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
    else
        browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND, u, NULL, NULL, NULL);
    nsurl_unref(u);
}

static char g_downgrade[WC_URL_MAX];

static void downgrade_now(void *p) {
    (void)p;
    navigate(g_downgrade);
}

void wc_window_downgrade(const char *https_url) {
    // https://host[:443]/rest -> http://host/rest: the default port goes
    // with the scheme.
    const char *rest = https_url + (strncmp(https_url, "https://", 8) ? 0 : 8);
    size_t hl = strcspn(rest, "/?#");
    char host[256];
    snprintf(host, sizeof host, "%.*s", (int)hl, rest);
    size_t n = strlen(host);
    if (n > 4 && !strcmp(host + n - 4, ":443")) host[n - 4] = '\0';
    snprintf(g_downgrade, sizeof g_downgrade, "http://%s%s", host, rest + hl);
    wc_send_str(WV_DOWNGRADED, host);
    ns_schedule(0, downgrade_now, NULL);   // not from inside NetSurf's fetch poll
}

void wc_window_handle(const struct ubulk_msg *m) {
    struct gui_window *gw = g_wc_win;
    int ready = gw && gw->bw;
    switch (m->type) {
    case WV_NAVIGATE: {
        static char url[WC_URL_MAX];
        str_of(m, 0, url, sizeof url);
        g_going = g_go_committed = g_restore = 0;
        wc_net_upgrade(NULL);
        navigate(url);
        break;
    }
    case WV_NAVIGATE_UPGRADED: {
        static char url[WC_URL_MAX];
        str_of(m, 0, url, sizeof url);
        g_going = g_go_committed = g_restore = 0;
        struct nsurl *u;
        if (nsurl_create(url, &u) == NSERROR_OK) {   // as the fetcher will be asked for it
            wc_net_upgrade(nsurl_access(u));
            nsurl_unref(u);
        }
        navigate(url);
        break;
    }
    case WV_GO: {
        static char url[WC_URL_MAX];
        struct wv_go g;
        if (m->len < sizeof g) break;
        memcpy(&g, m->data, sizeof g);
        str_of(m, sizeof g, url, sizeof url);
        g_going = g_restore = 1;
        g_go_committed = 0;
        wc_net_upgrade(NULL);
        g_restore_x = g.sx;
        g_restore_y = g.sy;
        navigate(url);
        break;
    }
    case WV_RELOAD:  if (ready) browser_window_reload(gw->bw, true); break;
    case WV_STOP:    if (ready) browser_window_stop(gw->bw); break;
    case WV_RESIZE: {
        struct wv_size s;
        if (m->len < sizeof s) break;
        memcpy(&s, m->data, sizeof s);
        if (s.line > 0) g_line = s.line;
        if (s.w == g_buf.w && s.h == g_buf.h && g_buf.px) break;
        if (!buffer_make(s.w, s.h)) break;
        if (ready) {
            clamp_scroll(gw);
            browser_window_schedule_reformat(gw->bw);
        }
        break;
    }
    case WV_MOUSE:
        if (ready && m->len >= sizeof(struct wv_mouse)) on_mouse(gw, m->data);
        break;
    case WV_WHEEL:
        if (ready && m->len >= sizeof(struct wv_wheel)) on_wheel(gw, m->data);
        break;
    case WV_KEY:
        if (ready && m->len >= sizeof(struct wv_key)) on_key(gw, m->data);
        break;
    case WV_SCROLL_TO:
        if (ready && m->len >= sizeof(struct wv_point)) {
            const struct wv_point *p = m->data;
            scroll_to(gw, p->x, p->y);
        }
        break;
    case WV_ZOOM:
        if (ready && m->len >= sizeof(struct wv_int)) wc_zoom(((const struct wv_int *)m->data)->v);
        break;
    case WV_FIND: {
        if (!ready || m->len < sizeof(struct wv_find)) break;
        char q[256];
        str_of(m, sizeof(struct wv_find), q, sizeof q);
        ns_find(gw, q, ((const struct wv_find *)m->data)->dir);
        struct wv_find_result r = { ns_find_current(), ns_find_matches() };
        wc_send(WV_FIND_RESULT, &r, sizeof r);
        break;
    }
    case WV_FIND_CLEAR:
        ns_find_clear(gw);
        break;
    case WV_FRAME_ACK:
        if (m->len >= sizeof(struct wv_int) && (uint32_t)((const struct wv_int *)m->data)->v == g_seq)
            g_await_ack = 0;
        break;
    case WV_VISIBLE:
        if (m->len >= sizeof(struct wv_int)) {
            int v = ((const struct wv_int *)m->data)->v != 0;
            if (v && !g_visible) dirty_all();
            g_visible = v;
        }
        break;
    case WV_SCHEME_DATA:
        wc_scheme_data(m);
        break;
    case WV_PERMS:
        wc_js_set_perms((const char *)m->data, m->len);
        break;
    case WV_CRASH:
        // What a person typed as about:crash (Chrome's chrome://crash):
        // a real fault, so the crash path is the one a real one takes.
        *(volatile int *)0 = 0;
        break;
    }
}
