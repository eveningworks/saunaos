// A list of file transfers -- see ui/uui_transfers.h.
#include <stdio.h>
#include <string.h>

#include "lib/human.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include "ui/uui_transfers.h"
#include "win_proto.h"   // WIN_CURSOR_*

static int pad(void) { return ugfx_char_advance('n') + 2; }
static int icon_w(void) { return ugfx_char_advance('W') * 3 + 4; }

int uui_transfers_row_h(void) { return ugfx_char_h() * 2 + 18; }

void uui_transfers_init(struct uui_transfers *t) {
    memset(t, 0, sizeof *t);
    t->hot = t->armed = -1;
}

int uui_transfers_take(struct uui_transfers *t, int *row) {
    int e = t->event;
    if (row) *row = t->event_row;
    t->event = UUI_XFER_EV_NONE;
    return e;
}

// The action a row's state allows, and its label for a text button
// (NULL: an icon button).
static int action_of(const struct uui_xfer *x) {
    switch (x->state) {
    case UUI_XFER_RUNNING: return UUI_XFER_EV_CANCEL;
    case UUI_XFER_DONE:    return UUI_XFER_EV_SHOW;
    case UUI_XFER_FAILED:  return UUI_XFER_EV_RETRY;
    }
    return UUI_XFER_EV_NONE;
}

static void action_rect(const struct uui_transfers *t, int row, int *x, int *y, int *w, int *h) {
    const struct uui_xfer *it = &t->items[row];
    int rh = uui_transfers_row_h();
    *h = ugfx_char_h() + 8;
    *w = action_of(it) == UUI_XFER_EV_RETRY ? ugfx_text_width("Retry") + 2 * pad() : *h;
    *x = t->x + t->w - pad() - *w;
    *y = t->y + row * rh + (rh - *h) / 2;
}

static void row_status(const struct uui_xfer *it, char *out, size_t cap) {
    char got[24], total[24];
    human_size_iec(got, sizeof got, it->got);
    human_size_iec(total, sizeof total, it->total);
    switch (it->state) {
    case UUI_XFER_RUNNING:
        if (it->total) snprintf(out, cap, "%s of %s", got, total);
        else snprintf(out, cap, "%s so far", got);
        break;
    case UUI_XFER_DONE:      snprintf(out, cap, "%s, done", got); break;
    case UUI_XFER_FAILED:    snprintf(out, cap, "Failed: %s", it->why[0] ? it->why : "it stopped"); break;
    case UUI_XFER_CANCELLED: snprintf(out, cap, "Cancelled"); break;
    default:                 out[0] = '\0';
    }
}

// A page with its corner folded, and the file's extension on it.
static void draw_file_icon(struct ugfx_surface *s, int x, int y, int w, int h, const char *name,
                           uint32_t ink, uint32_t paper) {
    int fold = w / 3;
    ugfx_fill_rect(s, x, y, w, h, ink);
    ugfx_fill_rect(s, x + 1, y + 1, w - 2, h - 2, paper);
    ugfx_fill_rect(s, x + w - fold, y, fold, fold, UTHEME_WHITE);
    ugfx_draw_line(s, x + w - fold, y, x + w - 1, y + fold - 1, ink, GEOM_ALIASED);
    const char *dot = strrchr(name, '.');
    if (dot && dot[1]) {
        char ext[4] = { 0 };
        for (int i = 0; i < 3 && dot[1 + i]; i++) {
            char c = dot[1 + i];
            ext[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
        }
        int tw = ugfx_text_width(ext);
        ugfx_draw_string_clipped(s, x + (w - tw) / 2 > x + 1 ? x + (w - tw) / 2 : x + 1,
                                 y + h - ugfx_char_h() - 3, w - 2, ext, ink, paper);
    }
}

static void draw_action(struct ugfx_surface *s, const struct uui_transfers *t, int row) {
    const struct uui_xfer *it = &t->items[row];
    int a = action_of(it);
    if (!a) return;
    int x, y, w, h;
    action_rect(t, row, &x, &y, &w, &h);
    int hot = t->hot == row && t->hot_action, armed = t->armed == row;
    uint32_t base = a == UUI_XFER_EV_RETRY ? UTHEME_BUTTON_BG : UTHEME_WHITE;
    uint32_t bg = armed ? uui_state_bg(base, UUI_STATE_PRESSED) : hot ? uui_state_bg(base, UUI_STATE_HOVER) : base;
    if (bg != UTHEME_WHITE) uui_fill_round_rect(s, x, y, w, h, uui_control_radius(4), bg);
    uint32_t ink = utheme_current()->text_muted;
    int cx = x + w / 2, cy = y + h / 2, r = h / 4;
    if (a == UUI_XFER_EV_CANCEL) {
        ugfx_draw_line(s, cx - r, cy - r, cx + r, cy + r, ink, GEOM_AA);
        ugfx_draw_line(s, cx - r, cy + r, cx + r, cy - r, ink, GEOM_AA);
    } else if (a == UUI_XFER_EV_SHOW) {
        // A folder: a tab on the left of a box.
        int fw = h / 2 + 2, fh = h / 3 + 2, fx = cx - fw / 2, fy = cy - fh / 2 + 1;
        ugfx_draw_rect(s, fx, fy, fw, fh, ink);
        ugfx_fill_rect(s, fx, fy - 2, fw / 2, 2, ink);
    } else {
        ugfx_draw_string(s, x + pad(), y + (h - ugfx_char_h()) / 2, "Retry", UTHEME_TEXT, bg);
    }
}

static void t_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_transfers *t = w;
    int rh = uui_transfers_row_h(), p = pad(), iw = icon_w();
    uint32_t muted = utheme_current()->text_muted;
    for (int i = 0; i < t->count; i++) {
        const struct uui_xfer *it = &t->items[i];
        int y = t->y + i * rh;
        uint32_t bg = t->hot == i ? uui_state_bg(UTHEME_WHITE, UUI_STATE_HOVER) : UTHEME_WHITE;
        ugfx_fill_rect(s, t->x, y, t->w, rh, bg);
        draw_file_icon(s, t->x + p, y + 4, iw, rh - 8, it->name, UTHEME_OUTLINE, UTHEME_PANEL_BG);
        int ax, ay, aw, ah;
        action_rect(t, i, &ax, &ay, &aw, &ah);
        int tx = t->x + p + iw + p, room = ax - p - tx;
        uint32_t name_ink = it->state == UUI_XFER_CANCELLED || it->state == UUI_XFER_FAILED ? muted : UTHEME_TEXT;
        ugfx_draw_string_elided(s, tx, y + 5, room, it->name, name_ink, bg);
        int sy = y + 5 + ugfx_char_h() + 3;
        if (it->state == UUI_XFER_RUNNING) {
            int bw = room > 8 ? room : 8;
            ugfx_fill_rect(s, tx, sy, bw, 4, UTHEME_TAB_REST);
            if (it->total) {
                int fill = (int)((unsigned long long)bw * (it->got > it->total ? it->total : it->got) / it->total);
                ugfx_fill_rect(s, tx, sy, fill, 4, UTHEME_ACCENT);
            }
            sy += 7;
        }
        char status[160];
        row_status(it, status, sizeof status);
        uint32_t ink = it->state == UUI_XFER_FAILED ? utheme_action(UTHEME_ACT_DANGER) : muted;
        ugfx_draw_string_elided(s, tx, sy, room, status, ink, bg);
        draw_action(s, t, i);
    }
}

static void t_natural(const void *w, int *ow, int *oh) {
    const struct uui_transfers *t = w;
    *ow = ugfx_char_advance('n') * 40;
    *oh = (t->count ? t->count : 1) * uui_transfers_row_h();
}

static void t_geometry(void *w, int x, int y, int width, int height) {
    struct uui_transfers *t = w;
    t->x = x;
    t->y = y;
    t->w = width;
    t->h = height;
}

static void t_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_transfers *t = w;
    *x = t->x;
    *y = t->y;
    *ow = t->w;
    *oh = t->h;
}

static int row_at(const struct uui_transfers *t, int cx, int cy, int *on_action) {
    *on_action = 0;
    if (cx < t->x || cx >= t->x + t->w || cy < t->y) return -1;
    int row = (cy - t->y) / uui_transfers_row_h();
    if (row >= t->count) return -1;
    int x, y, w, h;
    action_rect(t, row, &x, &y, &w, &h);
    *on_action = action_of(&t->items[row]) && uui_hit(x, y, w, h, cx, cy);
    return row;
}

static int t_hit(const void *w, int cx, int cy) {
    const struct uui_transfers *t = w;
    return uui_hit(t->x, t->y, t->w, t->h, cx, cy);
}

static int t_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_transfers *t = w;
    int on;
    int row = row_at(t, cx, cy, &on);
    t->armed = on ? row : -1;
    return row >= 0;
}

static int t_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_transfers *t = w;
    int on;
    int row = row_at(t, cx, cy, &on);
    if (row == t->hot && on == t->hot_action) return 0;
    t->hot = row;
    t->hot_action = on;
    return 1;
}

static int t_release(void *w, int cx, int cy) {
    struct uui_transfers *t = w;
    int on;
    int row = row_at(t, cx, cy, &on);
    if (t->armed >= 0 && row == t->armed && on) {
        t->event = action_of(&t->items[row]);
        t->event_row = row;
    }
    t->armed = -1;
    return 1;
}

static int t_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_DEFAULT;
}

// Each row, and its action's rect, for a test that clicks one.
static void t_describe(const void *w, const struct uui_describe *d) {
    const struct uui_transfers *t = w;
    int x, y, wd, h;
    uui_describe_int(d, "count", t->count);
    for (int i = 0; i < t->count; i++) {
        uui_describe_rect_i(d, "row", i, t->x, t->y + i * uui_transfers_row_h(), t->w, uui_transfers_row_h());
        if (action_of(&t->items[i])) {
            action_rect(t, i, &x, &y, &wd, &h);
            uui_describe_rect_i(d, "action", i, x, y, wd, h);
        }
    }
}

const struct uui_widget_ops uui_transfers_ops = {
    .natural_size = t_natural,
    .set_geometry = t_geometry,
    .bounds       = t_bounds,
    .draw         = t_draw,
    .hit          = t_hit,
    .press        = t_press,
    .motion       = t_motion,
    .release      = t_release,
    .cursor       = t_cursor,
    .describe     = t_describe,
};
