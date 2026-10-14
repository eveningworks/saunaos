// A layer stack -- see ui/uui_layerlist.h.
#include "ui/uui_layerlist.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include "lib/icon_cache.h"
#include "lib/udclick.h"
#include "rt/sys.h"
#include <stdio.h>

#define DRAG_PX 4   // how far a press moves before it is a drag

int uui_layerlist_row_h(void) { return ugfx_char_h() * 2 + 6; }

void uui_layerlist_init(struct uui_layerlist *l) {
    *l = (struct uui_layerlist){ 0 };
    uui_sbar_init(&l->sb);
    l->hot = l->press = l->eye_press = l->last_row = -1;
}

enum uui_layer_event uui_layerlist_take(struct uui_layerlist *l, int *row, int *to) {
    enum uui_layer_event e = l->ev;
    if (row) *row = l->ev_row;
    if (to) *to = l->ev_to;
    l->ev = UUI_LAYER_NONE;
    return e;
}

// --- geometry: visual rows count from the TOP ---------------------------

static int visible_rows(const struct uui_layerlist *l) {
    int rh = uui_layerlist_row_h();
    return rh > 0 ? l->h / rh : 0;
}

static void clamp_top(struct uui_layerlist *l) {
    int max = l->count - visible_rows(l);
    if (l->top > max) l->top = max;
    if (l->top < 0) l->top = 0;
}

static struct uui_sbar *sbar(struct uui_layerlist *l) {
    clamp_top(l);
    uui_sbar_place(&l->sb, l->x + l->w - uui_sbar_width(), l->y, l->h);
    uui_sbar_set(&l->sb, l->count, visible_rows(l), l->top);
    return &l->sb;
}

static int rows_w(struct uui_layerlist *l) {
    return l->w - (uui_sbar_shown(sbar(l)) ? uui_sbar_width() : 0);
}

static int row_of_visual(const struct uui_layerlist *l, int v) { return l->count - 1 - v; }

// The bottom-first row at a point, or -1.
static int row_at(struct uui_layerlist *l, int cx, int cy) {
    if (!uui_hit(l->x, l->y, rows_w(l), l->h, cx, cy)) return -1;
    int v = l->top + (cy - l->y) / uui_layerlist_row_h();
    return v >= 0 && v < l->count ? row_of_visual(l, v) : -1;
}

static void row_rect(struct uui_layerlist *l, int row, int *x, int *y, int *w, int *h) {
    int v = l->count - 1 - row;
    *x = l->x;
    *y = l->y + (v - l->top) * uui_layerlist_row_h();
    *w = rows_w(l);
    *h = uui_layerlist_row_h();
}

static int eye_w(void) { return ugfx_char_h() + 10; }

static int on_eye(struct uui_layerlist *l, int row, int cx) {
    (void)row;
    return cx < l->x + eye_w();
}

// The gap a drag at `cy` would drop into, as a visual slot 0..count
// (0 above the top row).
static int gap_at(struct uui_layerlist *l, int cy) {
    int rh = uui_layerlist_row_h();
    int g = l->top + (cy - l->y + rh / 2) / rh;
    return g < 0 ? 0 : g > l->count ? l->count : g;
}

// --- drawing ------------------------------------------------------------

static uint32_t over_check(uint32_t px, int sx, int sy) {
    unsigned a = px >> 24;
    uint32_t chk = ((sx >> 2) + (sy >> 2)) & 1 ? 0xCCCCCC : 0xFFFFFF;
    if (a == 255) return px & 0xFFFFFF;
    if (a == 0) return chk;
    return ugfx_blend(chk, px & 0xFFFFFF, (uint8_t)a);
}

// The layer, fitted into (x, y, w, h) at its aspect, nearest neighbour,
// over a checkerboard, in a hairline frame.
static void thumb(struct ugfx_surface *s, const struct uimg *im, int x, int y, int w, int h) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0) return;
    int tw = w, th = (int)((long long)im->h * w / im->w);
    if (th > h) { th = h; tw = (int)((long long)im->w * h / im->h); }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    int tx = x + (w - tw) / 2, ty = y + (h - th) / 2;
    ugfx_fill_rect(s, tx - 1, ty - 1, tw + 2, th + 2, UTHEME_OUTLINE);
    for (int j = 0; j < th; j++) {
        const uint32_t *src = im->px + (size_t)((long long)j * im->h / th) * im->w;
        for (int i = 0; i < tw; i++)
            ugfx_fill_rect(s, tx + i, ty + j, 1, 1, over_check(src[(long long)i * im->w / tw], i, j));
    }
}

static void ll_draw(struct ugfx_surface *s, const void *w) {
    struct uui_layerlist *l = (struct uui_layerlist *)w;
    int rh = uui_layerlist_row_h(), rw = rows_w(l), lh = ugfx_char_h();
    int r = uui_control_radius(0);
    for (int v = l->top; v < l->count && (v - l->top) * rh < l->h; v++) {
        int row = row_of_visual(l, v);
        const struct uui_layerrow *it = &l->rows[row];
        int x = l->x, y = l->y + (v - l->top) * rh;
        int cur = row == l->current;
        enum uui_state st = row == l->press && !l->dragging ? UUI_STATE_PRESSED
                          : row == l->hot ? UUI_STATE_HOVER : UUI_STATE_REST;
        uint32_t ink = cur ? UTHEME_ACCENT_TEXT : UTHEME_TEXT;
        if (cur) uui_fill_round_rect(s, x + 1, y + 1, rw - 2, rh - 2, r, uui_state_bg(UTHEME_ACCENT, st));
        else if (st != UUI_STATE_REST) uui_fill_round_rect(s, x + 1, y + 1, rw - 2, rh - 2, r, uui_state_bg(UTHEME_PANEL_BG, st));
        // The eye: open while shown, struck while hidden.
        const struct uimg *eye = icon_get(it->hidden ? "tb-hide" : "tb-show", lh);
        if (eye) ugfx_blit_tinted(s, x + (eye_w() - eye->w) / 2, y + (rh - eye->h) / 2, eye->w, eye->h, eye->px, eye->w, ink);
        int th = rh - 8, tw = th * 4 / 3;
        int tx = x + eye_w();
        thumb(s, it->img, tx, y + 4, tw, th);
        if (it->text) {
            // Krita's and GIMP's mark: a T in the thumbnail's corner.
            int bs = lh;
            const struct uimg *t = icon_get("tb-text", bs - 4);
            uui_fill_round_rect(s, tx + tw - bs, y + 4 + th - bs, bs, bs, uui_control_radius(2), UTHEME_ACCENT);
            if (t) ugfx_blit_tinted(s, tx + tw - bs + 2, y + 4 + th - bs + 2, t->w, t->h, t->px, t->w, UTHEME_ACCENT_TEXT);
        }
        int nx = tx + tw + 8;
        char pct[8];
        int op = it->opacity < 0 ? 0 : it->opacity > 255 ? 255 : it->opacity;
        snprintf(pct, sizeof pct, "%d%%", (op * 100 + 127) / 255);
        int pw = ugfx_text_width(pct);
        uint32_t name_ink = it->hidden && !cur ? ugfx_blend(UTHEME_PANEL_BG, ink, 140) : ink;
        int ty = y + (rh - lh) / 2;
        uint32_t ground = UGFX_TRANSPARENT;
        ugfx_draw_string_clipped(s, nx, ty, x + rw - 8 - pw - 8 - nx, it->name ? it->name : "", name_ink, ground);
        ugfx_draw_string_clipped(s, x + rw - 8 - pw, ty, pw + 1, pct, name_ink, ground);
    }
    // Where a dragged row would go.
    if (l->dragging && l->press >= 0) {
        int gy = l->y + (l->drop_at - l->top) * rh;
        if (gy < l->y) gy = l->y;
        if (gy > l->y + l->h - 2) gy = l->y + l->h - 2;
        ugfx_fill_rect(s, l->x + 2, gy - 1, rw - 4, 2, UTHEME_ACCENT);
    }
    uui_sbar_draw(sbar(l), s, UTHEME_PANEL_BG, UTHEME_TEXT);
}

// --- input --------------------------------------------------------------

static int ll_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_layerlist *l = w;
    int r = uui_sbar_press(sbar(l), cx, cy);
    if (r & UUI_SBAR_MOVED) l->top = l->sb.top;
    if (r & UUI_SBAR_TOOK) return 1;
    int row = row_at(l, cx, cy);
    l->press = l->eye_press = -1;
    l->dragging = 0;
    if (row < 0) return uui_hit(l->x, l->y, l->w, l->h, cx, cy);
    if (on_eye(l, row, cx)) l->eye_press = row;
    else { l->press = row; l->press_y = cy; }
    return 1;
}

static int ll_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_layerlist *l = w;
    if (buttons) {
        if (l->sb.grab >= 0) {
            int r = uui_sbar_motion(sbar(l), cx, cy, buttons);
            if (r & UUI_SBAR_MOVED) l->top = l->sb.top;
            return (r & (UUI_SBAR_MOVED | UUI_SBAR_REDRAW)) != 0;
        }
        if (l->press < 0) return 0;
        if (!l->dragging && (cy - l->press_y > DRAG_PX || l->press_y - cy > DRAG_PX)) l->dragging = 1;
        if (!l->dragging) return 0;
        int g = gap_at(l, cy);
        if (g == l->drop_at) return 0;
        l->drop_at = g;
        return 1;
    }
    int r = uui_sbar_motion(sbar(l), cx, cy, 0);
    int row = (r & UUI_SBAR_TOOK) ? -1 : row_at(l, cx, cy);
    if (row == l->hot) return (r & UUI_SBAR_REDRAW) != 0;
    l->hot = row;
    return 1;
}

static int ll_release(void *w, int cx, int cy) {
    struct uui_layerlist *l = w;
    if (l->sb.grab >= 0) { uui_sbar_release(&l->sb); return 1; }
    int row = row_at(l, cx, cy);
    if (l->eye_press >= 0) {
        if (row == l->eye_press && on_eye(l, row, cx)) { l->ev = UUI_LAYER_EYE; l->ev_row = row; }
    } else if (l->press >= 0 && l->dragging) {
        // A gap above visual row g is bottom-first index count - g; moving
        // a row out from below that gap shifts it down by one.
        int to = l->count - l->drop_at;
        if (to > l->press) to--;
        if (to != l->press) { l->ev = UUI_LAYER_MOVE; l->ev_row = l->press; l->ev_to = to; }
    } else if (l->press >= 0 && row == l->press) {
        unsigned long long now = sys_monotonic_ns();
        int dbl = row == l->last_row && now - l->last_ns < udclick_ns();
        l->ev = dbl ? UUI_LAYER_RENAME : UUI_LAYER_PICK;
        l->ev_row = row;
        l->last_row = dbl ? -1 : row;
        l->last_ns = now;
    }
    int had = l->press >= 0 || l->eye_press >= 0;
    l->press = l->eye_press = -1;
    l->dragging = 0;
    return had;
}

static int ll_wheel(void *w, int notches) {
    struct uui_layerlist *l = w;
    int before = l->top;
    l->top -= notches;
    clamp_top(l);
    return l->top != before;
}

static void ll_natural(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = ugfx_char_w() * 24;
    *oh = uui_layerlist_row_h() * 4;
}

static void ll_geometry(void *w, int x, int y, int width, int height) {
    struct uui_layerlist *l = w;
    l->x = x; l->y = y; l->w = width; l->h = height;
    clamp_top(l);
}

static void ll_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_layerlist *l = w;
    *x = l->x; *y = l->y; *ow = l->w; *oh = l->h;
}

// The WHOLE RECT, not the rows: the scrollbar's strip is the widget's too.
static int ll_hit(const void *w, int cx, int cy) {
    const struct uui_layerlist *l = w;
    return uui_hit(l->x, l->y, l->w, l->h, cx, cy);
}

static void ll_describe(const void *w, const struct uui_describe *d) {
    struct uui_layerlist *l = (struct uui_layerlist *)w;
    uui_describe_int(d, "count", l->count);
    uui_describe_int(d, "current", l->current);
    for (int i = 0; i < l->count; i++) {
        int x, y, rw, rh;
        row_rect(l, i, &x, &y, &rw, &rh);
        if (y + rh <= l->y || y >= l->y + l->h) continue;
        uui_describe_rect_i(d, "row", i, x, y, rw, rh);
        uui_describe_rect_i(d, "eye", i, x, y, eye_w(), rh);
    }
}

const struct uui_widget_ops uui_layerlist_ops = {
    .natural_size = ll_natural,
    .set_geometry = ll_geometry,
    .bounds       = ll_bounds,
    .draw         = ll_draw,
    .hit          = ll_hit,
    .press        = ll_press,
    .motion       = ll_motion,
    .release      = ll_release,
    .wheel        = ll_wheel,
    .describe     = ll_describe,
};
