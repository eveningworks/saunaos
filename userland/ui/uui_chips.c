// See ui/uui_chips.h.
#include "ui/uui_chips.h"
#include "ui/uui_widget.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY
#include <stdio.h>

static int chip_h(void) { return ugfx_char_h() + 8; }
static int gap(void) { return ugfx_char_w() / 2 + 2; }

static const char *label_of(const struct uui_chips *c, int i) {
    return c->labels && c->labels[i] ? c->labels[i] : "";
}

static int count_of(const struct uui_chips *c, int i, char *out, size_t cap) {
    if (!c->counts || c->counts[i] < 0) { out[0] = 0; return 0; }
    snprintf(out, cap, "%d", c->counts[i]);
    return 1;
}

static int chip_w(const struct uui_chips *c, int i) {
    char n[16];
    int w = ugfx_text_width(label_of(c, i)) + 2 * UUI_PAD_X;
    if (count_of(c, i, n, sizeof n)) w += ugfx_char_w() / 2 + ugfx_text_width(n);
    return w;
}

// Where chips go, one at a time: left to right, a new row when the next
// one would cross `width` (a lone chip wider than that still gets a row).
struct walk { int i, x, y, w; };

static void walk_start(struct walk *k) { *k = (struct walk){ -1, 0, 0, 0 }; }

static int walk_next(const struct uui_chips *c, int width, struct walk *k) {
    if (++k->i >= c->count) return 0;
    int x = k->i ? k->x + k->w + gap() : 0;
    k->w = chip_w(c, k->i);
    if (k->i && x + k->w > width) { x = 0; k->y += chip_h() + gap(); }
    k->x = x;
    return 1;
}

void uui_chips_init(struct uui_chips *c, const char *const *labels, const int *counts,
                    int count, int selected) {
    *c = (struct uui_chips){ .labels = labels, .counts = counts, .count = count,
                             .selected = selected, .hovered = -1, .armed = -1 };
}

void uui_chips_natural_size(const struct uui_chips *c, int *out_w, int *out_h) {
    int w = 0;
    for (int i = 0; i < c->count; i++) w += (i ? gap() : 0) + chip_w(c, i);
    *out_w = w;
    *out_h = chip_h();
}

int uui_chips_height_for(const struct uui_chips *c, int width) {
    struct walk k;
    walk_start(&k);
    int h = 0;
    while (walk_next(c, width, &k)) h = k.y + chip_h();
    return h ? h : chip_h();
}

void uui_chips_set_geometry(struct uui_chips *c, int x, int y, int width, int height) {
    c->x = x;
    c->y = y;
    c->w = width;
    c->h = height;
}

int uui_chips_at(const struct uui_chips *c, int cx, int cy) {
    struct walk k;
    walk_start(&k);
    while (walk_next(c, c->w, &k))
        if (uui_hit(c->x + k.x, c->y + k.y, k.w, chip_h(), cx, cy)) return k.i;
    return -1;
}

void uui_chips_draw(struct ugfx_surface *s, const struct uui_chips *c) {
    uint32_t fg = UTHEME_TEXT, face = UTHEME_WHITE;
    uint32_t edge = uui_state_bg(fg, UUI_STATE_DISABLED);
    int r = chip_h() / 2;
    struct walk k;
    walk_start(&k);
    while (walk_next(c, c->w, &k)) {
        int x = c->x + k.x, y = c->y + k.y, h = chip_h();
        int chosen = k.i == c->selected;
        uint32_t bg = chosen ? UTHEME_ACCENT : face;
        if (!chosen && k.i == c->armed) bg = uui_state_bg(face, UUI_STATE_PRESSED);
        else if (!chosen && k.i == c->hovered) bg = uui_state_bg(face, UUI_STATE_HOVER);
        uint32_t text = chosen ? UTHEME_ACCENT_TEXT : fg;
        if (!chosen) uui_fill_round_rect(s, x, y, k.w, h, r, edge);
        uui_fill_round_rect(s, x + !chosen, y + !chosen, k.w - 2 * !chosen, h - 2 * !chosen,
                            chosen ? r : r - 1, bg);
        int tx = x + UUI_PAD_X, ty = y + (h - ugfx_char_h()) / 2;
        const char *l = label_of(c, k.i);
        ugfx_draw_string_clipped(s, tx, ty, k.w - UUI_PAD_X, l, text, bg);
        char n[16];
        if (count_of(c, k.i, n, sizeof n)) {
            int nx = tx + ugfx_text_width(l) + ugfx_char_w() / 2;
            ugfx_draw_string_clipped(s, nx, ty, x + k.w - nx, n, ugfx_blend(bg, text, 170), bg);
        }
        if (chosen && c->focused)
            uui_focus_ring(s, x - 2, y - 2, k.w + 4, h + 4);
    }
}

// --- ops ----------------------------------------------------------------

static void op_natural(const void *w, int *ow, int *oh) { uui_chips_natural_size(w, ow, oh); }
static void op_geometry(void *w, int x, int y, int width, int height) {
    uui_chips_set_geometry(w, x, y, width, height);
}
static void op_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_chips *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}
static void op_draw(struct ugfx_surface *s, const void *w) { uui_chips_draw(s, w); }
static int op_hit(const void *w, int cx, int cy) { return uui_chips_at(w, cx, cy) >= 0; }

static int op_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_chips *c = w;
    c->armed = uui_chips_at(c, cx, cy);
    return c->armed >= 0;
}

static int op_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_chips *c = w;
    int h = uui_chips_at(c, cx, cy);
    if (h == c->hovered) return 0;
    c->hovered = h;
    return 1;
}

// Chosen only if released over the chip that was pressed -- dragging off
// cancels, as it does a button.
static int op_release(void *w, int cx, int cy) {
    struct uui_chips *c = w;
    int at = uui_chips_at(c, cx, cy);
    if (c->armed >= 0 && at == c->armed) c->selected = at;
    c->armed = -1;
    return 1;
}

static int op_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_chips *c = w;
    int to = c->selected;
    if (c->count <= 0) return 0;
    switch (key) {
    case KEY_ARROW_LEFT:  to = to > 0 ? to - 1 : 0; break;
    case KEY_ARROW_RIGHT: to = to < c->count - 1 ? to + 1 : c->count - 1; break;
    case KEY_HOME:        to = 0; break;
    case KEY_END:         to = c->count - 1; break;
    default:              return 0;
    }
    c->selected = to < 0 ? 0 : to;
    return 1;
}

static void op_set_focused(void *w, int f) { ((struct uui_chips *)w)->focused = f; }
static int op_accepts_focus(const void *w) { return ((const struct uui_chips *)w)->count > 0; }

static void op_describe(const void *w, const struct uui_describe *d) {
    const struct uui_chips *c = w;
    struct walk k;
    walk_start(&k);
    while (walk_next(c, c->w, &k))
        uui_describe_rect_i(d, "slot", k.i, c->x + k.x, c->y + k.y, k.w, chip_h());
    uui_describe_int(d, "selected", c->selected);
}

const struct uui_widget_ops uui_chips_ops = {
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
