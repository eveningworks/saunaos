// One region of a run, its end dragged (ui/uui_resizebar.h).
#include "ui/uui_resizebar.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "keyboard.h"
#include "win_proto.h"   // WIN_CURSOR_RESIZE_H

#include <string.h>

#define GAP 3

void uui_resizebar_init(struct uui_resizebar *b) {
    memset(b, 0, sizeof *b);
    b->span = b->max_end = b->min_end = b->end = 1;
}

static uint64_t step_of(const struct uui_resizebar *b) { return b->step ? b->step : 1; }

// Clamped into [min_end, max_end] and onto a step from `start` -- down,
// except at max_end, which a drag to the far right must reach exactly.
static uint64_t snap(const struct uui_resizebar *b, uint64_t v) {
    if (v >= b->max_end) return b->max_end;
    if (v <= b->min_end) return b->min_end;
    uint64_t st = step_of(b), off = (v - b->start) / st * st;
    v = b->start + off;
    return v < b->min_end ? b->min_end : v;
}

void uui_resizebar_set(struct uui_resizebar *b, uint64_t span, uint64_t start, uint64_t end,
                       uint64_t min_end, uint64_t max_end, uint64_t used) {
    b->span = span ? span : 1;
    b->start = start;
    b->max_end = max_end > b->span ? b->span : max_end;
    b->min_end = min_end > b->max_end ? b->max_end : min_end;
    if (b->min_end <= start) b->min_end = start + 1 > b->max_end ? b->max_end : start + 1;
    b->used = used;
    b->end = snap(b, end);
}

int uui_resizebar_set_end(struct uui_resizebar *b, uint64_t end) {
    uint64_t v = snap(b, end);
    if (v == b->end) return 0;
    b->end = v;
    return 1;
}

// Where `v` of the run falls across the bar.
static int x_of(const struct uui_resizebar *b, uint64_t v) {
    if (v > b->span) v = b->span;
    return b->x + (int)((v * (uint64_t)(b->w > 0 ? b->w : 0)) / b->span);
}

static uint64_t v_of(const struct uui_resizebar *b, int px) {
    if (px <= b->x || b->w <= 0) return 0;
    if (px >= b->x + b->w) return b->span;
    return (uint64_t)(px - b->x) * b->span / (uint64_t)b->w;
}

// Two thirds of a line: grabbable, and thin enough to read as an edge.
static int handle_w(void) { int w = ugfx_char_h() * 2 / 3; return w < 8 ? 8 : w; }

void uui_resizebar_handle(const struct uui_resizebar *b, int *x, int *y, int *w, int *h) {
    int hw = handle_w(), cx = x_of(b, b->end);
    *x = cx - hw / 2;
    if (*x + hw > b->x + b->w) *x = b->x + b->w - hw;
    if (*x < b->x) *x = b->x;
    *y = b->y;
    *w = hw;
    *h = b->h;
}

static int on_handle(const struct uui_resizebar *b, int cx, int cy) {
    int hx, hy, hw, hh;
    uui_resizebar_handle(b, &hx, &hy, &hw, &hh);
    return uui_hit(hx - 2, hy, hw + 4, hh, cx, cy);
}

// --- drawing ----------------------------------------------------------------

static void dashed_box(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c) {
    for (int i = 0; i < w; i += 6) {
        int l = w - i < 3 ? w - i : 3;
        ugfx_fill_rect(s, x + i, y, l, 1, c);
        ugfx_fill_rect(s, x + i, y + h - 1, l, 1, c);
    }
    for (int i = 0; i < h; i += 6) {
        int l = h - i < 3 ? h - i : 3;
        ugfx_fill_rect(s, x, y + i, 1, l, c);
        ugfx_fill_rect(s, x + w - 1, y + i, 1, l, c);
    }
}

// What lies outside the region: a neighbour card, dimmed.
static void dim_card(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (w < 2) return;
    uint32_t edge = ugfx_blend(UTHEME_WHITE, UTHEME_OUTLINE, 150);
    uui_fill_round_rect(s, x, y, w, h, 4, edge);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, 3, ugfx_blend(UTHEME_WHITE, UTHEME_OUTLINE, 40));
}

// Diagonal stripes, 4 px of each tint: what is in use.
static void hatch(struct ugfx_surface *s, int x0, int y0, int x1, int y1, uint32_t a, uint32_t c) {
    for (int yy = y0; yy < y1; yy++) {
        int phase = yy - y0;
        for (int xx = x0; xx < x1;) {
            int k = ((xx - x0) + phase) % 8, run = k < 4 ? 4 - k : 8 - k;
            if (run > x1 - xx) run = x1 - xx;
            ugfx_fill_rect(s, xx, yy, run, 1, k < 4 ? a : c);
            xx += run;
        }
    }
}

static void rb_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_resizebar *b = w;
    if (b->w <= 0 || b->h <= 0) return;
    int y = b->y, h = b->h;
    int xs = x_of(b, b->start) + (b->start ? GAP / 2 + 1 : 0);
    int xe = x_of(b, b->end);
    int xm = x_of(b, b->max_end) - (b->max_end < b->span ? GAP / 2 + 1 : 0);
    uint32_t accent = b->disabled ? uui_state_bg(UTHEME_ACCENT, UUI_STATE_DISABLED) : UTHEME_ACCENT;
    uint32_t tint = ugfx_blend(UTHEME_WHITE, accent, 34);

    if (b->start) dim_card(s, b->x, y, xs - GAP - b->x, h);
    if (b->max_end < b->span) dim_card(s, xm + GAP, y, b->x + b->w - xm - GAP, h);
    // The free space it may grow into.
    if (xm > xe + 1) {
        ugfx_fill_rect(s, xe, y, xm - xe, h, UTHEME_WHITE);
        dashed_box(s, xe, y, xm - xe, h, UTHEME_OUTLINE);
    }
    // The region: a 2 px accent edge, the used part hatched from its start.
    int rw = xe - xs;
    if (rw >= 4) {
        uui_fill_round_rect(s, xs, y, rw, h, 4, accent);
        uui_fill_round_rect(s, xs + 2, y + 2, rw - 4, h - 4, 2, tint);
        if (b->used) {
            uint64_t uv = b->start + b->used;
            int ux = x_of(b, uv > b->end ? b->end : uv);
            if (ux > xs + 3)
                hatch(s, xs + 3, y + 3, ux > xe - 3 ? xe - 3 : ux, y + h - 3,
                      ugfx_blend(UTHEME_WHITE, accent, 140), ugfx_blend(UTHEME_WHITE, accent, 95));
        }
        if (b->label) {
            int pad = ugfx_char_w() / 2 + 2;
            ugfx_draw_string_clipped(s, xs + pad, y + (h - ugfx_char_h()) / 2, rw - pad * 2 - handle_w() / 2,
                                     b->label, UTHEME_TEXT, UGFX_TRANSPARENT);
        }
    }
    // The handle, over the junction, with a light edge so it reads on both.
    if (!b->disabled) {
        int hx, hy, hw, hh;
        uui_resizebar_handle(b, &hx, &hy, &hw, &hh);
        uint32_t face = b->dragging ? uui_state_bg(accent, UUI_STATE_PRESSED)
                      : b->hot ? uui_state_bg(accent, UUI_STATE_HOVER) : accent;
        uui_fill_round_rect(s, hx, hy, hw, hh, 3, UTHEME_WHITE);
        uui_fill_round_rect(s, hx + 1, hy + 1, hw - 2, hh - 2, 3, face);
        // Keyboard focus: a grip line down its middle.
        if (b->focused) ugfx_fill_rect(s, hx + hw / 2, hy + hh / 4, 1, hh / 2, UTHEME_WHITE);
    }
}

// --- ops ---------------------------------------------------------------------

static void rb_natural(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = ugfx_char_w() * 40;
    *oh = ugfx_char_h() * 2 + ugfx_char_w();
}

static void rb_geometry(void *w, int x, int y, int width, int height) {
    struct uui_resizebar *b = w;
    b->x = x; b->y = y; b->w = width; b->h = height;
}

static void rb_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_resizebar *b = w;
    *x = b->x; *y = b->y; *ow = b->w; *oh = b->h;
}

static int rb_hit(const void *w, int cx, int cy) {
    const struct uui_resizebar *b = w;
    return uui_hit(b->x, b->y, b->w, b->h, cx, cy);
}

static int movable(const struct uui_resizebar *b) { return !b->disabled && b->max_end > b->min_end; }

static int rb_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_resizebar *b = w;
    if (!uui_hit(b->x, b->y, b->w, b->h, cx, cy)) return 0;
    if (!movable(b)) return 1;
    b->dragging = 1;
    // On the handle, the end stays put until the pointer moves; anywhere
    // else on the bar, it jumps to the pointer.
    if (!on_handle(b, cx, cy)) uui_resizebar_set_end(b, v_of(b, cx));
    return 1;
}

static int rb_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_resizebar *b = w;
    if (b->dragging && (buttons & 1)) return uui_resizebar_set_end(b, v_of(b, cx));
    int hot = movable(b) && on_handle(b, cx, cy);
    int changed = hot != b->hot;
    b->hot = hot;
    return changed;
}

static int rb_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    struct uui_resizebar *b = w;
    int was = b->dragging;
    b->dragging = 0;
    return was;
}

static int rb_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_resizebar *b = w;
    if (!movable(b)) return 0;
    uint64_t st = step_of(b), v = b->end;
    switch (key) {
    case KEY_ARROW_LEFT:  v = v > st ? v - st : 0; break;
    case KEY_ARROW_RIGHT: v += st; break;
    case KEY_PAGE_UP:     v = v > st * 10 ? v - st * 10 : 0; break;
    case KEY_PAGE_DOWN:   v += st * 10; break;
    case KEY_HOME:        v = b->min_end; break;
    case KEY_END:         v = b->max_end; break;
    default: return 0;
    }
    uui_resizebar_set_end(b, v);
    return 1;
}

static int rb_accepts(const void *w) { return movable(w); }
static void rb_focused(void *w, int f) { ((struct uui_resizebar *)w)->focused = f; }

static int rb_cursor(const void *w, int cx, int cy) {
    const struct uui_resizebar *b = w;
    return movable(b) && (b->dragging || on_handle(b, cx, cy)) ? WIN_CURSOR_RESIZE_H : WIN_CURSOR_DEFAULT;
}

static void rb_describe(const void *w, const struct uui_describe *d) {
    const struct uui_resizebar *b = w;
    int hx, hy, hw, hh;
    uui_resizebar_handle(b, &hx, &hy, &hw, &hh);
    uui_describe_rect(d, "handle", hx, hy, hw, hh);
    // Per mille of the run, so a test reads the end without the unit.
    uui_describe_int(d, "end_pm", (int)(b->end * 1000 / b->span));
    uui_describe_int(d, "min_pm", (int)(b->min_end * 1000 / b->span));
    uui_describe_int(d, "max_pm", (int)(b->max_end * 1000 / b->span));
    uui_describe_int(d, "disabled", b->disabled);
}

const struct uui_widget_ops uui_resizebar_ops = {
    .natural_size = rb_natural,
    .set_geometry = rb_geometry,
    .bounds = rb_bounds,
    .draw = rb_draw,
    .hit = rb_hit,
    .key = rb_key,
    .set_focused = rb_focused,
    .accepts_focus = rb_accepts,
    .press = rb_press,
    .motion = rb_motion,
    .release = rb_release,
    .cursor = rb_cursor,
    .describe = rb_describe,
};
