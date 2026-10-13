// NetSurf's window, misc and fetch tables, and the page widget that shows
// the active window (ns_view.h).
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "utils/errors.h"
#include "browser/ns_plot.h"
#include "browser/ns_sched.h"
#include "browser/ns_view.h"

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

#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "win_proto.h"   // WIN_CURSOR_*

#define RESOURCE_DIR "/usr/share/netsurf/"
// Pointer movement, in pixels, that turns a press into a drag (a text
// selection); under it, the release is a click.
#define DRAG_SLOP 4

// --- the window table ----------------------------------------------------

static struct gui_window *gw_create(struct browser_window *bw, struct gui_window *existing,
                                    gui_window_create_flags flags) {
    struct gui_window *gw = calloc(1, sizeof *gw);
    if (!gw) return NULL;
    gw->bw = bw;
    gw->pointer = WIN_CURSOR_DEFAULT;
    nsapp_window_added(gw, existing, !existing || (flags & GW_CREATE_FOREGROUND));
    return gw;
}

static void gw_destroy(struct gui_window *gw) {
    nsapp_window_removed(gw);
    free(gw);
}

static nserror gw_invalidate(struct gui_window *gw, const struct rect *r) {
    (void)r;   // the whole view: a page is redrawn from its own content cache
    if (gw == nsapp_active()) nsapp_redraw();
    return NSERROR_OK;
}

static bool gw_get_scroll(struct gui_window *gw, int *sx, int *sy) {
    *sx = gw->sx;
    *sy = gw->sy;
    return true;
}

static void clamp_scroll(struct gui_window *gw) {
    struct ns_view *v = nsapp_view();
    int max_x = gw->ext_w - ns_view_page_w(v), max_y = gw->ext_h - v->h;
    if (gw->sx > max_x) gw->sx = max_x;
    if (gw->sy > max_y) gw->sy = max_y;
    if (gw->sx < 0) gw->sx = 0;
    if (gw->sy < 0) gw->sy = 0;
}

static nserror gw_set_scroll(struct gui_window *gw, const struct rect *r) {
    gw->sx = r->x0;
    gw->sy = r->y0;
    clamp_scroll(gw);
    if (gw == nsapp_active()) nsapp_redraw();
    return NSERROR_OK;
}

static nserror gw_get_dimensions(struct gui_window *gw, int *w, int *h) {
    (void)gw;
    struct ns_view *v = nsapp_view();
    *w = ns_view_page_w(v);
    *h = v->h;
    return NSERROR_OK;
}

static nserror gw_event(struct gui_window *gw, enum gui_window_event ev) {
    switch (ev) {
    case GW_EVENT_UPDATE_EXTENT:
        browser_window_get_extents(gw->bw, true, &gw->ext_w, &gw->ext_h);
        clamp_scroll(gw);
        break;
    case GW_EVENT_REMOVE_CARET:
        gw->caret_h = 0;
        break;
    case GW_EVENT_START_THROBBER:
        gw->loading = 1;
        nsapp_window_changed(gw);
        break;
    case GW_EVENT_STOP_THROBBER:
        gw->loading = 0;
        // NetSurf sets the busy pointer for a load and leaves resetting
        // it to the next motion; one held still would keep the ring.
        if (gw->pointer == WIN_CURSOR_WAIT) gw->pointer = WIN_CURSOR_DEFAULT;
        nsapp_window_changed(gw);
        break;
    case GW_EVENT_NEW_CONTENT:
    case GW_EVENT_SCROLL_START:
        gw->sx = gw->sy = 0;
        break;
    default:
        break;
    }
    if (gw == nsapp_active()) nsapp_redraw();
    return NSERROR_OK;
}

static void gw_set_title(struct gui_window *gw, const char *title) {
    strlcpy(gw->title, title ? title : "", sizeof gw->title);
    nsapp_window_changed(gw);
}

static nserror gw_set_url(struct gui_window *gw, struct nsurl *url) {
    strlcpy(gw->url, nsurl_access(url), sizeof gw->url);
    nsapp_window_changed(gw);
    return NSERROR_OK;
}

// NetSurf's status line carries progress ("Fetching, Loading") as well
// as a hovered link; only a LINK is shown over the page, as browsers do
// (the rest is the throbber's job), and a link is what has a scheme.
static void gw_set_status(struct gui_window *gw, const char *text) {
    const char *t = text ? text : "";
    if (!strstr(t, "://") && strncmp(t, "about:", 6) && strncmp(t, "mailto:", 7)) t = "";
    if (!strcmp(gw->status, t)) return;
    strlcpy(gw->status, t, sizeof gw->status);
    if (gw == nsapp_active()) nsapp_redraw();
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
    if (gw == nsapp_active()) nsapp_redraw();
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
struct gui_window_table *ns_window_table = &g_window;

// --- misc and fetch -------------------------------------------------------

static struct gui_misc_table g_misc = {
    .schedule = ns_schedule,
};
struct gui_misc_table *ns_misc_table = &g_misc;

// What a file: URL is, by its extension -- what monkey and every other
// front end answer from a table; NetSurf sniffs the content where it may.
static const char *ft_filetype(const char *path) {
    static const struct { const char *ext, *type; } T[] = {
        { "html", "text/html" }, { "htm", "text/html" }, { "xhtml", "application/xhtml+xml" },
        { "css", "text/css" }, { "js", "application/javascript" },
        { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
        { "gif", "image/gif" }, { "bmp", "image/bmp" }, { "ico", "image/x-icon" },
        { "svg", "image/svg+xml" }, { "qoi", "image/qoi" },
        { "txt", "text/plain" }, { "md", "text/plain" }, { "conf", "text/plain" },
    };
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash))
        for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
            if (!strcasecmp(dot + 1, T[i].ext)) return T[i].type;
    return "text/plain";
}

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
struct gui_fetch_table *ns_fetch_table = &g_fetch;

// --- the page widget -----------------------------------------------------

void ns_view_init(struct ns_view *v) {
    memset(v, 0, sizeof *v);
    uui_sbar_init(&v->sb);
    v->hover_x = v->hover_y = -1;
}

int ns_view_page_w(const struct ns_view *v) {
    int w = v->w - uui_sbar_width();
    return w > 1 ? w : 1;
}

static void sync_bar(struct ns_view *v) {
    struct gui_window *gw = nsapp_active();
    uui_sbar_place(&v->sb, v->x + v->w - uui_sbar_width(), v->y, v->h);
    uui_sbar_set(&v->sb, gw ? gw->ext_h : 0, v->h, gw ? gw->sy : 0);
    v->sb.step = ugfx_char_h() * 3;
}

int ns_view_scroll_by(struct ns_view *v, int dx, int dy) {
    struct gui_window *gw = nsapp_active();
    if (!gw) return 0;
    int ox = gw->sx, oy = gw->sy;
    gw->sx += dx;
    gw->sy += dy;
    clamp_scroll(gw);
    sync_bar(v);
    return gw->sx != ox || gw->sy != oy;
}

static const int ZOOM_STEPS[] = { 25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200,
                                   250, 300, 400, 500 };
#define ZOOM_COUNT ((int)(sizeof ZOOM_STEPS / sizeof ZOOM_STEPS[0]))

int ns_view_zoom_pct(struct gui_window *gw) {
    if (!gw || !gw->bw) return 100;
    return (int)(browser_window_get_scale(gw->bw) * 100.0f + 0.5f);
}

int ns_view_zoom(struct gui_window *gw, int dir) {
    if (!gw || !gw->bw) return 100;
    int pct = ns_view_zoom_pct(gw), to = 100;
    if (dir > 0) {
        to = ZOOM_STEPS[ZOOM_COUNT - 1];
        for (int i = 0; i < ZOOM_COUNT; i++)
            if (ZOOM_STEPS[i] > pct) { to = ZOOM_STEPS[i]; break; }
    } else if (dir < 0) {
        to = ZOOM_STEPS[0];
        for (int i = ZOOM_COUNT - 1; i >= 0; i--)
            if (ZOOM_STEPS[i] < pct) { to = ZOOM_STEPS[i]; break; }
    }
    if (to != pct) browser_window_set_scale(gw->bw, (float)to / 100.0f, true);
    return to;
}

static void v_natural_size(const void *w, int *out_w, int *out_h) {
    (void)w;
    *out_w = ugfx_char_advance('n') * 40;
    *out_h = ugfx_char_h() * 10;
}

static void v_set_geometry(void *w, int x, int y, int width, int height) {
    struct ns_view *v = w;
    int reflow = width != v->w || height != v->h;
    v->x = x;
    v->y = y;
    v->w = width;
    v->h = height;
    sync_bar(v);
    struct gui_window *gw = nsapp_active();
    if (reflow && gw) browser_window_schedule_reformat(gw->bw);
}

static void v_bounds(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct ns_view *v = w;
    *x = v->x;
    *y = v->y;
    *out_w = v->w;
    *out_h = v->h;
}

// A hovered link's address in the view's bottom-left corner, over the
// page -- where every browser puts it, and gone when the pointer is.
static void draw_status(struct ugfx_surface *s, const struct ns_view *v, const char *text) {
    int pad = ugfx_char_advance('n') / 2 + 2;
    int h = ugfx_char_h() + 6;
    int max = v->w * 2 / 3;
    int w = ugfx_text_width(text) + 2 * pad;
    if (w > max) w = max;
    int x = v->x, y = v->y + v->h - h;
    ugfx_fill_rect(s, x, y, w, h, UTHEME_PANEL_BG);
    ugfx_draw_rect(s, x - 1, y, w + 1, h + 1, UTHEME_OUTLINE);
    ugfx_draw_string_elided(s, x + pad, y + 3, w - 2 * pad, text, UTHEME_TEXT, UTHEME_PANEL_BG);
}

static void v_draw(struct ugfx_surface *s, const void *w) {
    struct ns_view *v = (struct ns_view *)w;
    struct gui_window *gw = nsapp_active();
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, v->x, v->y, v->w, v->h);
    // White under the page: what an unstyled page's canvas is, and what
    // shows for the instant before the first layout.
    ugfx_fill_rect(s, v->x, v->y, v->w, v->h, ugfx_rgb(255, 255, 255));
    if (gw && gw->bw) {
        struct ns_target t = { s, v->x, v->y, v->x + ns_view_page_w(v), v->y + v->h };
        struct redraw_context ctx;
        ns_plot_context(&ctx, &t);
        struct rect clip = { t.cx0, t.cy0, t.cx1, t.cy1 };
        browser_window_redraw(gw->bw, v->x - gw->sx, v->y - gw->sy, &clip, &ctx);
        ugfx_clip_restore(s, &saved);
        ugfx_clip_intersect(s, v->x, v->y, v->w, v->h);
        if (gw->caret_h && v->focused)
            ugfx_fill_rect(s, v->x + gw->caret_x - gw->sx, v->y + gw->caret_y - gw->sy, 1,
                           gw->caret_h, ugfx_rgb(0, 0, 0));
        sync_bar(v);
        uui_sbar_draw(&v->sb, s, ugfx_rgb(255, 255, 255), UTHEME_TEXT);
        if (gw->status[0] && v->hover_x >= 0) draw_status(s, v, gw->status);
    }
    ugfx_clip_restore(s, &saved);
}

static int inside(const struct ns_view *v, int cx, int cy) {
    return cx >= v->x && cy >= v->y && cx < v->x + v->w && cy < v->y + v->h;
}

static int v_hit(const void *w, int cx, int cy) { return inside(w, cx, cy); }

static int bar_result(struct ns_view *v, int r) {
    struct gui_window *gw = nsapp_active();
    if ((r & UUI_SBAR_MOVED) && gw) {
        gw->sy = v->sb.top;
        clamp_scroll(gw);
    }
    return r != 0;
}

static void page_xy(const struct ns_view *v, const struct gui_window *gw, int cx, int cy,
                    int *px, int *py) {
    *px = cx - v->x + gw->sx;
    *py = cy - v->y + gw->sy;
}

static int v_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct ns_view *v = w;
    struct gui_window *gw = nsapp_active();
    if (!inside(v, cx, cy)) return 0;
    sync_bar(v);
    if (uui_sbar_hit(&v->sb, cx, cy)) return bar_result(v, uui_sbar_press(&v->sb, cx, cy)) | 1;
    if (!gw) return 1;
    int px, py;
    page_xy(v, gw, cx, cy, &px, &py);
    v->pressed = 1;
    v->dragged = 0;
    v->press_x = cx;
    v->press_y = cy;
    browser_window_mouse_click(gw->bw, BROWSER_MOUSE_PRESS_1, px, py);
    return 1;
}

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

static int v_motion(void *w, int cx, int cy, unsigned buttons) {
    struct ns_view *v = w;
    struct gui_window *gw = nsapp_active();
    if (v->sb.grab >= 0) return bar_result(v, uui_sbar_motion(&v->sb, cx, cy, buttons));
    int changed = 0;
    if (inside(v, cx, cy) || v->pressed) {
        changed |= bar_result(v, uui_sbar_motion(&v->sb, cx, cy, buttons));
        v->hover_x = cx;
        v->hover_y = cy;
        if (gw) {
            int px, py;
            page_xy(v, gw, cx, cy, &px, &py);
            if (v->pressed && !v->dragged &&
                (abs(cx - v->press_x) > DRAG_SLOP || abs(cy - v->press_y) > DRAG_SLOP)) {
                int ox, oy;
                page_xy(v, gw, v->press_x, v->press_y, &ox, &oy);
                browser_window_mouse_click(gw->bw, BROWSER_MOUSE_DRAG_1, ox, oy);
                v->dragged = 1;
            }
            int state = v->dragged ? (BROWSER_MOUSE_DRAG_ON | BROWSER_MOUSE_HOLDING_1) : 0;
            browser_window_mouse_track(gw->bw, state, px, py);
            if (gw->pointer == WIN_CURSOR_DEFAULT && over_text(gw, px, py)) gw->pointer = WIN_CURSOR_TEXT;
        }
    } else if (v->hover_x >= 0) {
        v->hover_x = v->hover_y = -1;
        changed |= uui_sbar_leave(&v->sb) != 0;
        changed = 1;   // the status overlay goes with the pointer
    }
    return changed;
}

static int v_release(void *w, int cx, int cy) {
    struct ns_view *v = w;
    struct gui_window *gw = nsapp_active();
    if (v->sb.grab >= 0) return bar_result(v, uui_sbar_release(&v->sb)) | 1;
    if (!v->pressed) return 0;
    v->pressed = 0;
    if (!gw) return 1;
    int px, py;
    page_xy(v, gw, cx, cy, &px, &py);
    if (!v->dragged) browser_window_mouse_click(gw->bw, BROWSER_MOUSE_CLICK_1, px, py);
    else browser_window_mouse_track(gw->bw, 0, px, py);
    v->dragged = 0;
    return 1;
}

static int v_wheel(void *w, int notches) {
    struct ns_view *v = w;
    struct gui_window *gw = nsapp_active();
    if (!gw) return 0;
    int step = ugfx_char_h() * 3;
    // A box under the pointer that scrolls (an overflow:auto div, a
    // textarea) takes the wheel first, as everywhere on the web.
    if (v->hover_x >= 0) {
        int px, py;
        page_xy(v, gw, v->hover_x, v->hover_y, &px, &py);
        if (browser_window_scroll_at_point(gw->bw, px, py, 0, -notches * step)) return 1;
    }
    return ns_view_scroll_by(v, 0, -notches * step);
}

// toy-os's keys as NetSurf's. 0 for one it has no code for.
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
    return 0;
}

static int v_key(void *w, int key, unsigned mods) {
    struct ns_view *v = w;
    struct gui_window *gw = nsapp_active();
    if (!gw) return 0;
    uint32_t k = ns_key(key, mods);
    // A focused form field or a selection takes the key first.
    if (k && browser_window_key_press(gw->bw, k)) return 1;
    // ...and what it did not want scrolls the page.
    int line = ugfx_char_h() * 3, page = v->h - ugfx_char_h() * 2;
    switch (key) {
    case KEY_ARROW_UP:    return ns_view_scroll_by(v, 0, -line) | 1;
    case KEY_ARROW_DOWN:  return ns_view_scroll_by(v, 0, line) | 1;
    case KEY_ARROW_LEFT:  return ns_view_scroll_by(v, -line, 0) | 1;
    case KEY_ARROW_RIGHT: return ns_view_scroll_by(v, line, 0) | 1;
    case KEY_PAGE_UP:     return ns_view_scroll_by(v, 0, -page) | 1;
    case KEY_PAGE_DOWN:
    case ' ':             return ns_view_scroll_by(v, 0, (mods & KEY_MOD_SHIFT) ? -page : page) | 1;
    case KEY_HOME:        return ns_view_scroll_by(v, 0, -gw->sy) | 1;
    case KEY_END:         return ns_view_scroll_by(v, 0, gw->ext_h) | 1;
    }
    return 0;
}

static int v_accepts_focus(const void *w) { (void)w; return 1; }
static void v_set_focused(void *w, int focused) { ((struct ns_view *)w)->focused = focused; }

static int v_cursor(const void *w, int cx, int cy) {
    const struct ns_view *v = w;
    struct gui_window *gw = nsapp_active();
    if (!gw || uui_sbar_hit(&v->sb, cx, cy)) return WIN_CURSOR_DEFAULT;
    return gw->pointer;
}

const struct uui_widget_ops ns_view_ops = {
    .natural_size = v_natural_size,
    .set_geometry = v_set_geometry,
    .bounds = v_bounds,
    .draw = v_draw,
    .hit = v_hit,
    .press = v_press,
    .motion = v_motion,
    .release = v_release,
    .wheel = v_wheel,
    .key = v_key,
    .accepts_focus = v_accepts_focus,
    .set_focused = v_set_focused,
    .cursor = v_cursor,
};
