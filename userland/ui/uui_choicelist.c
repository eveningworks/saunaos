// choicelist -- see ui/uui_choicelist.h.
#include "ui/uui_choicelist.h"
#include "ui/uui_primitives.h"
#include "ui/uui_describe.h"
#include "ui/utheme.h"
#include "keyboard.h"
#include <string.h>

static int pad(void) { return ugfx_char_h() / 2; }
static int row_h(void) { return ugfx_char_h() * 2 + pad() * 2 + 4; }
static int gap(void) { return 4; }
static int marker(void) { int m = ugfx_char_h() * 3 / 4; return m < 10 ? 10 : m; }

void uui_choicelist_init(struct uui_choicelist *l, const struct uui_choice *items, int count,
                         int selected) {
    memset(l, 0, sizeof *l);
    l->items = items;
    l->count = count;
    l->selected = selected;
    l->hovered = l->armed = -1;
}

static int tags_w(const char *tags) {
    if (!tags) return 0;
    int w = 0;
    char word[24];
    for (const char *p = tags; *p; ) {
        while (*p == ' ') p++;
        int n = 0;
        while (p[n] && p[n] != ' ' && n < (int)sizeof word - 1) n++;
        if (!n) break;
        memcpy(word, p, (size_t)n);
        word[n] = 0;
        w += ugfx_text_width(word) + ugfx_char_w() + pad();
        p += n;
    }
    return w;
}

void uui_choicelist_natural_size(const struct uui_choicelist *l, int *out_w, int *out_h) {
    int w = 0;
    for (int i = 0; i < l->count; i++) {
        const struct uui_choice *c = &l->items[i];
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        int top = ugfx_text_width(c->title ? c->title : "");
        ugfx_set_font(was);
        top += tags_w(c->tags);
        int desc = c->desc ? ugfx_text_width(c->desc) : 0;
        int line = top > desc ? top : desc;
        if (c->value) line += ugfx_text_width(c->value) + ugfx_char_w() * 2;
        if (line > w) w = line;
    }
    if (out_w) *out_w = w + marker() + pad() * 4;
    if (out_h) *out_h = l->count * row_h() + (l->count > 1 ? (l->count - 1) * gap() : 0);
}

void uui_choicelist_set_geometry(struct uui_choicelist *l, int x, int y, int w, int h) {
    l->x = x; l->y = y; l->w = w; l->h = h;
}

static int row_y(const struct uui_choicelist *l, int i) { return l->y + i * (row_h() + gap()); }

int uui_choicelist_at(const struct uui_choicelist *l, int cx, int cy) {
    if (cx < l->x || cx >= l->x + l->w) return -1;
    for (int i = 0; i < l->count; i++) {
        int y = row_y(l, i);
        if (cy >= y && cy < y + row_h()) return i;
    }
    return -1;
}

void uui_choicelist_draw(struct ugfx_surface *s, const struct uui_choicelist *l) {
    uint32_t fg = UTHEME_TEXT, muted = ugfx_blend(UTHEME_TEXT, UTHEME_WHITE, 110);
    if (l->disabled) fg = uui_state_bg(fg, UUI_STATE_DISABLED);
    int r = utheme_radius(UTHEME_R_SELECTION);
    for (int i = 0; i < l->count; i++) {
        const struct uui_choice *c = &l->items[i];
        int y = row_y(l, i), h = row_h();
        int sel = i == l->selected;
        uint32_t bg = UTHEME_WHITE;
        if (sel) bg = ugfx_blend(UTHEME_WHITE, UTHEME_ACCENT, 28);
        else if (i == l->hovered && !l->disabled) bg = uui_state_bg(UTHEME_WHITE, UUI_STATE_HOVER);
        uui_fill_round_rect(s, l->x, y, l->w, h, r, sel ? UTHEME_ACCENT : UTHEME_BORDER);
        uui_fill_round_rect(s, l->x + 1, y + 1, l->w - 2, h - 2, r > 1 ? r - 1 : r, bg);

        // The radio: a circle says "one of these".
        int m = marker(), mx = l->x + pad(), my = y + pad() + (ugfx_char_h() - m) / 2;
        uint32_t ring = sel && !l->disabled ? UTHEME_ACCENT : uui_state_bg(fg, UUI_STATE_DISABLED);
        uui_fill_round_rect(s, mx, my, m, m, UUI_CAPSULE, ring);
        uui_fill_round_rect(s, mx + 1, my + 1, m - 2, m - 2, UUI_CAPSULE, UTHEME_WHITE);
        if (sel) {
            int in = m / 4 > 1 ? m / 4 : 2;
            uui_fill_round_rect(s, mx + in, my + in, m - 2 * in, m - 2 * in, UUI_CAPSULE,
                                l->disabled ? fg : UTHEME_ACCENT);
        }

        int tx = mx + m + pad(), ty = y + pad(), right = l->x + l->w - pad();
        int vw = c->value ? ugfx_text_width(c->value) : 0;
        int text_right = c->value ? right - vw - ugfx_char_w() : right;
        if (c->value)
            ugfx_draw_string_clipped(s, right - vw, ty, vw, c->value, muted, bg);
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        int titw = ugfx_text_width(c->title ? c->title : "");
        ugfx_draw_string_clipped(s, tx, ty, text_right - tx, c->title ? c->title : "", fg, bg);
        ugfx_set_font(was);

        // Tags: small outlined pills after the title.
        int px = tx + titw + pad();
        for (const char *p = c->tags ? c->tags : ""; *p; ) {
            while (*p == ' ') p++;
            char word[24];
            int n = 0;
            while (p[n] && p[n] != ' ' && n < (int)sizeof word - 1) n++;
            if (!n) break;
            memcpy(word, p, (size_t)n);
            word[n] = 0;
            p += n;
            int ww = ugfx_text_width(word) + ugfx_char_w();
            if (px + ww > text_right) break;
            uui_fill_round_rect(s, px, ty, ww, ugfx_char_h(), UUI_CAPSULE, muted);
            uui_fill_round_rect(s, px + 1, ty + 1, ww - 2, ugfx_char_h() - 2, UUI_CAPSULE, bg);
            ugfx_draw_string_clipped(s, px + ugfx_char_w() / 2, ty, ww - ugfx_char_w() / 2, word,
                                     muted, bg);
            px += ww + pad();
        }
        if (c->desc)
            ugfx_draw_string_clipped(s, tx, ty + ugfx_char_h() + 4, right - tx, c->desc, muted, bg);
        if (l->focused && sel && !l->disabled) uui_focus_ring(s, l->x, y, l->w, h);
    }
}

// --- ops ------------------------------------------------------------------

static void op_natural(const void *w, int *ow, int *oh) { uui_choicelist_natural_size(w, ow, oh); }
static void op_geometry(void *w, int x, int y, int ww, int hh) { uui_choicelist_set_geometry(w, x, y, ww, hh); }
static void op_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_choicelist *l = w;
    *x = l->x; *y = l->y; *ow = l->w; *oh = l->h;
}
static void op_draw(struct ugfx_surface *s, const void *w) { uui_choicelist_draw(s, w); }
static int op_hit(const void *w, int cx, int cy) { return uui_choicelist_at(w, cx, cy) >= 0; }
static int op_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_choicelist *l = w;
    if (l->disabled) return 0;
    l->armed = uui_choicelist_at(l, cx, cy);
    return l->armed >= 0;
}
static int op_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_choicelist *l = w;
    int h = l->disabled ? -1 : uui_choicelist_at(l, cx, cy);
    if (h == l->hovered) return 0;
    l->hovered = h;
    return 1;
}
// Chosen only if released on the row that was pressed.
static int op_release(void *w, int cx, int cy) {
    struct uui_choicelist *l = w;
    int at = uui_choicelist_at(l, cx, cy);
    if (l->armed >= 0 && at == l->armed) l->selected = at;
    l->armed = -1;
    return 1;
}
static int op_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_choicelist *l = w;
    if (l->disabled || l->count <= 0) return 0;
    int to = l->selected;
    switch (key) {
    case KEY_ARROW_UP:   to = to > 0 ? to - 1 : 0; break;
    case KEY_ARROW_DOWN: to = to < l->count - 1 ? to + 1 : l->count - 1; break;
    case KEY_HOME:       to = 0; break;
    case KEY_END:        to = l->count - 1; break;
    default:             return 0;
    }
    l->selected = to < 0 ? 0 : to;
    return 1;
}
static void op_set_focused(void *w, int f) { ((struct uui_choicelist *)w)->focused = f; }
static int op_accepts_focus(const void *w) {
    const struct uui_choicelist *l = w;
    return !l->disabled && l->count > 0;
}
static void op_describe(const void *w, const struct uui_describe *d) {
    const struct uui_choicelist *l = w;
    for (int i = 0; i < l->count; i++) uui_describe_rect_i(d, "row", i, l->x, row_y(l, i), l->w, row_h());
    uui_describe_int(d, "selected", l->selected);
}

const struct uui_widget_ops uui_choicelist_ops = {
    .natural_size  = op_natural,
    .set_geometry  = op_geometry,
    .bounds        = op_bounds,
    .draw          = op_draw,
    .hit           = op_hit,
    .press         = op_press,
    .motion        = op_motion,
    .release       = op_release,
    .key           = op_key,
    .set_focused   = op_set_focused,
    .accepts_focus = op_accepts_focus,
    .describe      = op_describe,
};
