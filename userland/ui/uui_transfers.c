// A list of file transfers -- see ui/uui_transfers.h.
#include <stdio.h>
#include <string.h>

#include "lib/human.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include "ui/uui_transfers.h"
#include "lib/icon_cache.h"
#include "win_proto.h"   // WIN_CURSOR_*

static int pad(void) { return ugfx_char_advance('n') + 2; }
static int icon_w(void) { return ugfx_char_advance('W') * 3 + 4; }

int uui_transfers_row_h(void) { return ugfx_char_h() * 2 + 18; }

void uui_transfers_init(struct uui_transfers *t) {
    memset(t, 0, sizeof *t);
    t->hot = t->armed = -1;
}

#define MAX_ACTIONS 2

int uui_transfers_take(struct uui_transfers *t, int *row) {
    int e = t->event;
    if (row) *row = t->event_row;
    t->event = UUI_XFER_EV_NONE;
    return e;
}

// The actions a row's state allows, left to right; Cancel is always the
// rightmost, so a pointer parked on it stays on it as Pause becomes Resume.
static int actions_of(const struct uui_xfer *x, int out[MAX_ACTIONS]) {
    switch (x->state) {
    case UUI_XFER_RUNNING:
        if (!x->can_pause) { out[0] = UUI_XFER_EV_CANCEL; return 1; }
        out[0] = UUI_XFER_EV_PAUSE; out[1] = UUI_XFER_EV_CANCEL; return 2;
    case UUI_XFER_PAUSED:
        out[0] = UUI_XFER_EV_RESUME; out[1] = UUI_XFER_EV_CANCEL; return 2;
    case UUI_XFER_QUEUED:  out[0] = UUI_XFER_EV_CANCEL; return 1;
    case UUI_XFER_DONE:    out[0] = UUI_XFER_EV_SHOW;   return 1;
    case UUI_XFER_FAILED:  out[0] = UUI_XFER_EV_RETRY;  return 1;
    }
    return 0;
}

// Action `k` of row `row` (actions_of's order), laid out from the right.
static void action_rect(const struct uui_transfers *t, int row, int k, int *x, int *y, int *w, int *h) {
    int acts[MAX_ACTIONS], n = actions_of(&t->items[row], acts);
    int rh = uui_transfers_row_h(), right = t->x + t->w - pad();
    *h = ugfx_char_h() + 8;
    for (int i = n - 1; i >= k; i--) {
        *w = acts[i] == UUI_XFER_EV_RETRY ? ugfx_text_width("Retry") + 2 * pad() : *h;
        *x = right - *w;
        right = *x - 2;
    }
    *y = t->y + row * rh + (rh - *h) / 2;
}

// The leftmost action's left edge: where the text must stop.
static int actions_left(const struct uui_transfers *t, int row) {
    int acts[MAX_ACTIONS], x, y, w, h;
    if (!actions_of(&t->items[row], acts)) return t->x + t->w - pad();
    action_rect(t, row, 0, &x, &y, &w, &h);
    return x;
}

static void amount(const struct uui_xfer *x, unsigned long long n, char *out, size_t cap) {
    if (x->items) snprintf(out, cap, "%llu", n);
    else human_size_iec(out, cap, n);
}

// "about 4 s left", "about 3 min left", "about 2 h 10 min left": a
// guess, said as one, to the minute once it is that long.
static void time_left(unsigned long long secs, char *out, size_t cap) {
    if (secs < 60)        snprintf(out, cap, "about %llu s left", secs ? secs : 1);
    else if (secs < 3600) snprintf(out, cap, "about %llu min left", (secs + 30) / 60);
    else snprintf(out, cap, "about %llu h %llu min left", secs / 3600, (secs % 3600) / 60);
}

void uui_xfer_status(const struct uui_xfer *it, char *out, int cap) {
    char got[24], total[24], figs[96], rate[32] = "", left[40] = "";
    size_t c = (size_t)cap;
    amount(it, it->got, got, sizeof got);
    amount(it, it->total, total, sizeof total);
    if (it->total) snprintf(figs, sizeof figs, "%s of %s%s", got, total, it->items ? " items" : "");
    else snprintf(figs, sizeof figs, "%s%s so far", got, it->items ? " items" : "");
    if (it->state == UUI_XFER_RUNNING && it->rate && !it->items) {
        char r[24];
        human_size_iec(r, sizeof r, it->rate);
        snprintf(rate, sizeof rate, " \xb7 %s/s", r);
        if (it->total > it->got) {
            char l[32];
            time_left((it->total - it->got) / it->rate, l, sizeof l);
            snprintf(left, sizeof left, " \xb7 %s", l);
        }
    }
    const char *sep = it->note[0] ? " \xb7 " : "";
    switch (it->state) {
    case UUI_XFER_RUNNING:   snprintf(out, c, "%s%s%s%s%s", figs, rate, left, sep, it->note); break;
    case UUI_XFER_PAUSED:    snprintf(out, c, "Paused \xb7 %s%s%s", figs, sep, it->note); break;
    case UUI_XFER_QUEUED:    snprintf(out, c, "Waiting \xb7 starts when the one before it is done"); break;
    case UUI_XFER_DONE:      snprintf(out, c, "%s, done", got); break;
    case UUI_XFER_FAILED:    snprintf(out, c, "Failed: %s", it->why[0] ? it->why : "it stopped"); break;
    case UUI_XFER_CANCELLED: snprintf(out, c, "Cancelled"); break;
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

static void draw_action(struct ugfx_surface *s, const struct uui_transfers *t, int row, int k, int a) {
    int x, y, w, h;
    action_rect(t, row, k, &x, &y, &w, &h);
    int hot = t->hot == row && t->hot_action == a, armed = t->armed == row && t->armed_action == a;
    uint32_t base = a == UUI_XFER_EV_RETRY ? UTHEME_BUTTON_BG : UTHEME_WHITE;
    uint32_t bg = armed ? uui_state_bg(base, UUI_STATE_PRESSED) : hot ? uui_state_bg(base, UUI_STATE_HOVER) : base;
    if (bg != UTHEME_WHITE) uui_fill_round_rect(s, x, y, w, h, uui_control_radius(4), bg);
    uint32_t ink = utheme_current()->text_muted;
    int cx = x + w / 2, cy = y + h / 2, r = h / 4;
    if (a == UUI_XFER_EV_CANCEL) {
        ugfx_draw_line(s, cx - r, cy - r, cx + r, cy + r, ink, GEOM_AA);
        ugfx_draw_line(s, cx - r, cy + r, cx + r, cy - r, ink, GEOM_AA);
    } else if (a == UUI_XFER_EV_PAUSE) {
        int bw = r / 2 + 1;
        ugfx_fill_rect(s, cx - r + 1, cy - r, bw, 2 * r + 1, ink);
        ugfx_fill_rect(s, cx + r - bw, cy - r, bw, 2 * r + 1, ink);
    } else if (a == UUI_XFER_EV_RESUME) {
        int xs[3] = { cx - r + 1, cx - r + 1, cx + r + 1 }, ys[3] = { cy - r, cy + r + 1, cy };
        ugfx_fill_polygon(s, xs, ys, 3, ink);
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
        const struct uimg *ic = it->icon ? icon_get(it->icon, rh - 12) : 0;
        if (ic) ugfx_blit_alpha(s, t->x + p + (iw - ic->w) / 2, y + (rh - ic->h) / 2, ic->w, ic->h, ic->px, ic->w);
        else draw_file_icon(s, t->x + p, y + 4, iw, rh - 8, it->name, UTHEME_OUTLINE, UTHEME_PANEL_BG);
        int tx = t->x + p + iw + p, room = actions_left(t, i) - p - tx;
        uint32_t name_ink = it->state == UUI_XFER_CANCELLED || it->state == UUI_XFER_FAILED ? muted : UTHEME_TEXT;
        ugfx_draw_string_elided(s, tx, y + 5, room, it->name, name_ink, bg);
        int sy = y + 5 + ugfx_char_h() + 3;
        if (it->state == UUI_XFER_RUNNING || it->state == UUI_XFER_PAUSED) {
            int bw = room > 8 ? room : 8;
            ugfx_fill_rect(s, tx, sy, bw, 4, UTHEME_TAB_REST);
            if (it->total) {
                int fill = (int)((unsigned long long)bw * (it->got > it->total ? it->total : it->got) / it->total);
                // A paused bar keeps its length and loses its colour.
                ugfx_fill_rect(s, tx, sy, fill, 4, it->state == UUI_XFER_PAUSED ? muted : UTHEME_ACCENT);
            }
            sy += 7;
        }
        char status[200];
        uui_xfer_status(it, status, sizeof status);
        uint32_t ink = it->state == UUI_XFER_FAILED ? utheme_action(UTHEME_ACT_DANGER) : muted;
        ugfx_draw_string_elided(s, tx, sy, room, status, ink, bg);
        int acts[MAX_ACTIONS], n = actions_of(it, acts);
        for (int k = 0; k < n; k++) draw_action(s, t, i, k, acts[k]);
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

// The row at a point, and the action under it there (0: none).
static int row_at(const struct uui_transfers *t, int cx, int cy, int *on_action) {
    *on_action = 0;
    if (cx < t->x || cx >= t->x + t->w || cy < t->y) return -1;
    int row = (cy - t->y) / uui_transfers_row_h();
    if (row >= t->count) return -1;
    int acts[MAX_ACTIONS], n = actions_of(&t->items[row], acts);
    for (int k = 0; k < n; k++) {
        int x, y, w, h;
        action_rect(t, row, k, &x, &y, &w, &h);
        if (uui_hit(x, y, w, h, cx, cy)) *on_action = acts[k];
    }
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
    t->armed_action = on;
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
    if (t->armed >= 0 && row == t->armed && on && on == t->armed_action) {
        t->event = on;
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
        const struct uui_xfer *it = &t->items[i];
        uui_describe_rect_i(d, "row", i, t->x, t->y + i * uui_transfers_row_h(), t->w, uui_transfers_row_h());
        int acts[MAX_ACTIONS], n = actions_of(it, acts);
        // The rightmost (Cancel, Show, Retry) as `action`, as before; a
        // second one by its event code as `action2`.
        for (int k = 0; k < n; k++) {
            action_rect(t, i, k, &x, &y, &wd, &h);
            if (k == n - 1) uui_describe_rect_i(d, "action", i, x, y, wd, h);
            else uui_describe_rect_ij(d, "action2", i, acts[k], x, y, wd, h);
        }
        char st[64];
        snprintf(st, sizeof st, "%d %d %llu %llu %llu", i, it->state, it->got, it->total, it->rate);
        uui_describe_str(d, "state", st);
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
