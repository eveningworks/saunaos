// A scrolling list of entries under headings -- see ui/uui_entrylist.h.
#include <string.h>

#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_entrylist.h"
#include "ui/uui_primitives.h"
#include "win_proto.h"   // WIN_CURSOR_*

static int entry_h(void) { return ugfx_char_h() * 2 + 10; }
static int heading_h(void) { return ugfx_char_h() + 12; }
static int pad(void) { return ugfx_char_advance('n') + 2; }
static int row_h(const struct uui_entry *r) { return r->heading ? heading_h() : entry_h(); }

static int total_h(const struct uui_entrylist *l) {
    int t = 0;
    for (int i = 0; i < l->count; i++) t += row_h(&l->rows[i]);
    return t;
}

// The row's top in content coordinates, scrolled.
static int row_y(const struct uui_entrylist *l, int row) {
    int y = l->y - l->top;
    for (int i = 0; i < row; i++) y += row_h(&l->rows[i]);
    return y;
}

static int list_w(const struct uui_entrylist *l) {
    return l->w - (uui_sbar_shown(&l->sb) ? uui_sbar_width() : 0);
}

static void remove_rect(const struct uui_entrylist *l, int row, int *x, int *y, int *w, int *h) {
    *h = *w = ugfx_char_h() + 6;
    *x = l->x + list_w(l) - pad() / 2 - *w;
    *y = row_y(l, row) + (entry_h() - *h) / 2;
}

static void sync_bar(struct uui_entrylist *l) {
    uui_sbar_place(&l->sb, l->x + l->w - uui_sbar_width(), l->y, l->h);
    uui_sbar_set(&l->sb, total_h(l), l->h, l->top);
    l->top = l->sb.top;
    l->sb.step = entry_h();
}

void uui_entrylist_init(struct uui_entrylist *l, const char *empty) {
    memset(l, 0, sizeof *l);
    uui_sbar_init(&l->sb);
    l->empty = empty;
    l->hot = l->armed = -1;
}

void uui_entrylist_reset(struct uui_entrylist *l) {
    l->top = 0;
    l->hot = l->armed = -1;
    l->hot_remove = l->armed_remove = 0;
    sync_bar(l);
}

int uui_entrylist_take(struct uui_entrylist *l, int *row) {
    int e = l->event;
    if (row) *row = l->event_row;
    l->event = UUI_ENTRY_EV_NONE;
    return e;
}

static int row_at(const struct uui_entrylist *l, int cx, int cy, int *on_remove) {
    *on_remove = 0;
    if (!uui_hit(l->x, l->y, list_w(l), l->h, cx, cy)) return -1;
    int y = l->y - l->top;
    for (int i = 0; i < l->count; i++) {
        int h = row_h(&l->rows[i]);
        if (cy >= y && cy < y + h) {
            if (l->rows[i].heading) return -1;
            if (l->removable) {
                int bx, by, bw, bh;
                remove_rect(l, i, &bx, &by, &bw, &bh);
                *on_remove = uui_hit(bx, by, bw, bh, cx, cy);
            }
            return i;
        }
        y += h;
    }
    return -1;
}

static void e_draw(struct ugfx_surface *s, const void *w) {
    struct uui_entrylist *l = (struct uui_entrylist *)w;
    sync_bar(l);
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, l->x, l->y, l->w, l->h);
    uint32_t ground = UTHEME_PANEL_BG, muted = utheme_current()->text_muted;
    ugfx_fill_rect(s, l->x, l->y, l->w, l->h, ground);
    int lw = list_w(l), p = pad();
    if (!l->count && l->empty)
        ugfx_draw_string_elided(s, l->x + p, l->y + p, lw - 2 * p, l->empty, muted, ground);
    for (int i = 0; i < l->count; i++) {
        const struct uui_entry *r = &l->rows[i];
        int y = row_y(l, i), h = row_h(r);
        if (y + h <= l->y || y >= l->y + l->h) continue;
        if (r->heading) {
            ugfx_draw_string_elided(s, l->x + p, y + h - ugfx_char_h() - 3, lw - 2 * p, r->title, muted, ground);
            continue;
        }
        uint32_t bg = ground;
        if (l->hot == i) {
            bg = uui_state_bg(ground, l->armed == i && !l->armed_remove ? UUI_STATE_PRESSED : UUI_STATE_HOVER);
            uui_fill_round_rect(s, l->x + 2, y + 1, lw - 4, h - 2, uui_select_radius(4), bg);
        }
        int room = lw - 2 * p;
        if (l->removable && l->hot == i) {
            int bx, by, bw, bh;
            remove_rect(l, i, &bx, &by, &bw, &bh);
            room = bx - (l->x + p) - p / 2;
            if (l->hot_remove)
                uui_fill_round_rect(s, bx, by, bw, bh, uui_control_radius(4), uui_state_bg(bg, UUI_STATE_HOVER));
            int cx = bx + bw / 2, cy = by + bh / 2, k = bh / 4;
            ugfx_draw_line(s, cx - k, cy - k, cx + k, cy + k, muted, GEOM_AA);
            ugfx_draw_line(s, cx - k, cy + k, cx + k, cy - k, muted, GEOM_AA);
        }
        ugfx_draw_string_elided(s, l->x + p, y + 5, room, r->title, UTHEME_TEXT, bg);
        ugfx_draw_string_elided(s, l->x + p, y + 5 + ugfx_char_h() + 1, room, r->sub, muted, bg);
    }
    uui_sbar_draw(&l->sb, s, ground, UTHEME_TEXT);
    ugfx_clip_restore(s, &saved);
}

static void e_natural(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = ugfx_char_advance('n') * 28;
    *oh = entry_h() * 4;
}

static void e_geometry(void *w, int x, int y, int width, int height) {
    struct uui_entrylist *l = w;
    l->x = x;
    l->y = y;
    l->w = width;
    l->h = height;
    sync_bar(l);
}

static void e_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_entrylist *l = w;
    *x = l->x;
    *y = l->y;
    *ow = l->w;
    *oh = l->h;
}

// The whole rect: the bar is part of the list (userland/CLAUDE.md).
static int e_hit(const void *w, int cx, int cy) {
    const struct uui_entrylist *l = w;
    return uui_hit(l->x, l->y, l->w, l->h, cx, cy);
}

static int bar_moved(struct uui_entrylist *l, int r) {
    if (r & UUI_SBAR_MOVED) l->top = l->sb.top;
    return r != 0;
}

static int e_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_entrylist *l = w;
    sync_bar(l);
    if (uui_sbar_hit(&l->sb, cx, cy)) return bar_moved(l, uui_sbar_press(&l->sb, cx, cy)) | 1;
    int on;
    l->armed = row_at(l, cx, cy, &on);
    l->armed_remove = on;
    return 1;
}

static int e_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_entrylist *l = w;
    if (l->sb.grab >= 0) return bar_moved(l, uui_sbar_motion(&l->sb, cx, cy, buttons));
    int changed = bar_moved(l, uui_sbar_motion(&l->sb, cx, cy, buttons));
    int on;
    int row = row_at(l, cx, cy, &on);
    if (row != l->hot || on != l->hot_remove) {
        l->hot = row;
        l->hot_remove = on;
        changed = 1;
    }
    return changed;
}

static int e_release(void *w, int cx, int cy) {
    struct uui_entrylist *l = w;
    if (l->sb.grab >= 0) return bar_moved(l, uui_sbar_release(&l->sb)) | 1;
    int on;
    int row = row_at(l, cx, cy, &on);
    if (l->armed >= 0 && row == l->armed && on == l->armed_remove) {
        l->event = on ? UUI_ENTRY_EV_REMOVE : UUI_ENTRY_EV_OPEN;
        l->event_row = row;
    }
    l->armed = -1;
    return 1;
}

static int e_wheel(void *w, int notches) {
    struct uui_entrylist *l = w;
    int old = l->top;
    uui_sbar_set(&l->sb, total_h(l), l->h, l->top - notches * entry_h() * 2);
    l->top = l->sb.top;
    return l->top != old;
}

static int e_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_DEFAULT;
}

// Each entry row, for a test that clicks one; headings are not rows a
// click can take, so they are not listed.
static void e_describe(const void *w, const struct uui_describe *d) {
    const struct uui_entrylist *l = w;
    uui_describe_int(d, "count", l->count);
    for (int i = 0; i < l->count; i++) {
        if (l->rows[i].heading) continue;
        int y = row_y(l, i);
        if (y + entry_h() <= l->y || y >= l->y + l->h) continue;
        uui_describe_rect_i(d, "row", i, l->x, y, list_w(l), entry_h());
        if (l->removable) {
            int bx, by, bw, bh;
            remove_rect(l, i, &bx, &by, &bw, &bh);
            uui_describe_rect_i(d, "remove", i, bx, by, bw, bh);
        }
    }
}

const struct uui_widget_ops uui_entrylist_ops = {
    .natural_size = e_natural,
    .set_geometry = e_geometry,
    .bounds       = e_bounds,
    .draw         = e_draw,
    .hit          = e_hit,
    .press        = e_press,
    .motion       = e_motion,
    .release      = e_release,
    .wheel        = e_wheel,
    .cursor       = e_cursor,
    .describe     = e_describe,
};
