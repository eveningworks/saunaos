// A row of latching buttons -- see ui/uui_togglebar.h.
#include "ui/uui_togglebar.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include "lib/icon_cache.h"

static int gap(void) { return 2; }

// An icon alone (B I U S) is the button's whole face, so it is drawn a
// little larger than one beside a word.
static int icon_px(const struct uui_toggle *it) {
    return ugfx_char_h() + (it->label || it->chip ? 0 : 4);
}

static int item_w(const struct uui_togglebar *t, int i) {
    const struct uui_toggle *it = &t->items[i];
    // THE CONTROL HEIGHT, NOT THE BAR'S: a row stretched taller than its
    // measure would widen every square past the room it was given, and
    // the last one ran under whatever came next.
    int h = utheme_control_h();
    (void)t;
    int w = 0;
    if (it->icon) w += icon_px(it) + (it->label || it->chip ? 6 : 0);
    if (it->label) w += ugfx_text_width(it->label);
    if (it->chip) w += ugfx_char_h() - 4 + 6;
    w += it->label || it->chip ? 20 : 0;
    return w > h ? w : h;   // at least square
}

static int item_x(const struct uui_togglebar *t, int i) {
    int x = t->x;
    for (int j = 0; j < i; j++) x += item_w(t, j) + gap();
    return x;
}

static int item_at(const struct uui_togglebar *t, int cx, int cy) {
    for (int i = 0; i < t->count; i++)
        if (uui_hit(item_x(t, i), t->y, item_w(t, i), t->h, cx, cy)) return i;
    return -1;
}

void uui_togglebar_init(struct uui_togglebar *t, const struct uui_toggle *items, int count) {
    *t = (struct uui_togglebar){ 0 };
    t->items = items;
    t->count = count < UUI_TOGGLEBAR_MAX ? count : UUI_TOGGLEBAR_MAX;
    t->hot = t->armed = t->picked = -1;
    uui_tip_init(&t->tooltip);
}

int uui_togglebar_take(struct uui_togglebar *t) {
    int p = t->picked;
    t->picked = -1;
    return p;
}

static void tb_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_togglebar *t = w;
    int r = uui_control_radius(0);
    for (int i = 0; i < t->count; i++) {
        const struct uui_toggle *it = &t->items[i];
        int x = item_x(t, i), iw = item_w(t, i), on = (t->on >> i) & 1;
        enum uui_state st = i == t->armed ? UUI_STATE_PRESSED : i == t->hot ? UUI_STATE_HOVER : UUI_STATE_REST;
        uint32_t ground = UTHEME_WHITE;
        uint32_t face = on ? uui_state_bg(UTHEME_ACCENT, st) : uui_state_bg(ground, st);
        uint32_t ink = on ? UTHEME_ACCENT_TEXT : UTHEME_TEXT;
        if (on || st != UUI_STATE_REST) uui_fill_round_rect(s, x, t->y, iw, t->h, r, face);
        // What is inside, centred: icon, label, chip.
        int cw = it->icon ? icon_px(it) + (it->label || it->chip ? 6 : 0) : 0;
        int lw = it->label ? ugfx_text_width(it->label) : 0;
        int chw = it->chip ? ugfx_char_h() - 4 + 6 : 0;
        int cx = x + (iw - cw - lw - chw) / 2;
        if (it->icon) {
            const struct uimg *ico = icon_get(it->icon, icon_px(it));
            if (ico) ugfx_blit_tinted(s, cx, t->y + (t->h - ico->h) / 2, ico->w, ico->h, ico->px, ico->w, ink);
            cx += cw;
        }
        if (it->label) {
            ugfx_draw_string(s, cx, t->y + (t->h - ugfx_char_h()) / 2, it->label, ink, UGFX_TRANSPARENT);
            cx += lw;
        }
        if (it->chip) {
            int n = ugfx_char_h() - 4;
            cx += 6;
            uui_fill_round_rect(s, cx, t->y + (t->h - n) / 2, n, n, uui_control_radius(2), UTHEME_OUTLINE);
            uui_fill_round_rect(s, cx + 1, t->y + (t->h - n) / 2 + 1, n - 2, n - 2, uui_control_radius(3),
                                *it->chip & 0xFFFFFF);
        }
    }
}

static void tb_natural(const void *w, int *ow, int *oh) {
    const struct uui_togglebar *t = w;
    struct uui_togglebar m = *t;
    m.h = utheme_control_h();
    *ow = t->count ? item_x(&m, t->count - 1) - m.x + item_w(&m, t->count - 1) : 0;
    *oh = m.h;
}

static void tb_geometry(void *w, int x, int y, int width, int height) {
    struct uui_togglebar *t = w;
    t->x = x; t->y = y; t->w = width; t->h = height;
}

static void tb_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_togglebar *t = w;
    *x = t->x; *y = t->y; *ow = t->w; *oh = t->h;
}

static int tb_hit(const void *w, int cx, int cy) { return item_at(w, cx, cy) >= 0; }

static int tb_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_togglebar *t = w;
    t->armed = item_at(t, cx, cy);
    return t->armed >= 0;
}

static int tb_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_togglebar *t = w;
    int h = item_at(t, cx, cy);
    if (h == t->hot) return 0;
    t->hot = h;
    uui_tip_hover(&t->tooltip, h);
    return 1;
}

int uui_togglebar_tick(struct uui_togglebar *t) {
    if (t->hot < 0 || t->hot >= t->count) return uui_tip_tick(&t->tooltip, 0, 0, 0, 0, 0, 0);
    return uui_tip_tick(&t->tooltip, t->items[t->hot].tip, item_x(t, t->hot), t->y, item_w(t, t->hot), t->h, 0);
}

int uui_togglebar_tip_waiting(const struct uui_togglebar *t) {
    return uui_tip_waiting(&t->tooltip) && t->hot >= 0 && t->items[t->hot].tip;
}

// The tooltip, in the router's overlay pass: above whatever is below.
static void tb_overlay(struct ugfx_surface *s, const void *w) {
    const struct uui_togglebar *t = w;
    if (t->hot < 0 || t->hot >= t->count || !t->items[t->hot].tip) return;
    const struct utheme *th = utheme_current();
    uui_tip_draw(s, &t->tooltip, t->items[t->hot].tip, item_x(t, t->hot), t->y, item_w(t, t->hot), t->h, 0,
                 th->tooltip_bg, th->tooltip_text, th->outline);
}

static int tb_release(void *w, int cx, int cy) {
    struct uui_togglebar *t = w;
    int at = item_at(t, cx, cy), was = t->armed;
    t->armed = -1;
    if (was >= 0 && at == was) t->picked = at;
    return was >= 0;
}

static void tb_describe(const void *w, const struct uui_describe *d) {
    const struct uui_togglebar *t = w;
    uui_describe_int(d, "on", (int)t->on);
    for (int i = 0; i < t->count; i++)
        uui_describe_rect_i(d, "button", i, item_x(t, i), t->y, item_w(t, i), t->h);
}

const struct uui_widget_ops uui_togglebar_ops = {
    .natural_size = tb_natural,
    .set_geometry = tb_geometry,
    .bounds       = tb_bounds,
    .draw         = tb_draw,
    .draw_overlay = tb_overlay,
    .hit          = tb_hit,
    .press        = tb_press,
    .motion       = tb_motion,
    .release      = tb_release,
    .describe     = tb_describe,
};
