// A window of steps -- see ui/uui_wizard.h.
#include "ui/uui_wizard.h"

#include <stdio.h>
#include <string.h>

#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"

// Font-derived, so the frame reflows with the session font.
static int rail_w(void)  { return ugfx_char_advance('n') * 32; }
static int row_h(void)   { return ugfx_char_h() * 2 + ugfx_char_h() / 3; }
static int dot_d(void)   { return ugfx_char_h() + ugfx_char_h() / 3; }
static int page_pad(void) { return ugfx_char_w() * 4; }

// --- the rail -----------------------------------------------------------

static int rail_rows_y(const struct uui_wizard_rail *r) {
    return r->y + ugfx_char_h() * 3;   // under the heading
}

static int rail_row_at(const struct uui_wizard_rail *r, int cx, int cy) {
    if (!uui_hit(r->x, r->y, r->w, r->h, cx, cy)) return -1;
    int i = (cy - rail_rows_y(r)) / row_h();
    return cy >= rail_rows_y(r) && i >= 0 && i < r->wz->d.count ? i : -1;
}

static void rail_natural(const void *w, int *out_w, int *out_h) {
    const struct uui_wizard_rail *r = w;
    *out_w = rail_w();
    *out_h = ugfx_char_h() * 3 + row_h() * r->wz->d.count;
}

static void rail_geometry(void *w, int x, int y, int width, int height) {
    struct uui_wizard_rail *r = w;
    r->x = x; r->y = y; r->w = width; r->h = height;
}

static void rail_bounds(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_wizard_rail *r = w;
    *x = r->x; *y = r->y; *out_w = r->w; *out_h = r->h;
}

static void draw_tick(struct ugfx_surface *s, int x, int y, int d, uint32_t c) {
    int x0 = x + d * 2 / 7, y0 = y + d / 2, x1 = x + d * 3 / 7, y1 = y + d * 9 / 14;
    int x2 = x + d * 5 / 7, y2 = y + d * 5 / 14;
    for (int k = 0; k < 2; k++) {
        ugfx_draw_line(s, x0, y0 + k, x1, y1 + k, c, GEOM_AA);
        ugfx_draw_line(s, x1, y1 + k, x2, y2 + k, c, GEOM_AA);
    }
}

static void rail_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_wizard_rail *r = w;
    const struct uui_wizard *wz = r->wz;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    if (wz->d.heading)
        ugfx_draw_string_clipped(s, r->x + ugfx_char_w(), r->y + ugfx_char_h(), r->w - ugfx_char_w(),
                                 wz->d.heading, UTHEME_TEXT, UTHEME_CHROME);
    ugfx_set_font(was);

    int rh = row_h(), d = dot_d(), rad = ugfx_char_h() / 2;
    for (int i = 0; i < wz->d.count; i++) {
        int y = rail_rows_y(r) + i * rh;
        int now = i == wz->step, done = i < wz->step || i <= wz->reached;
        uint32_t bg = UTHEME_CHROME;
        if (now) {
            // The soft accent selection with its 1px edge
            // (docs/gui-guidelines.md, the app design language).
            uui_fill_round_rect(s, r->x, y + 2, r->w, rh - 4, rad, UTHEME_ACCENT);
            uui_fill_round_rect(s, r->x + 1, y + 3, r->w - 2, rh - 6, rad - 1, UTHEME_SELECTION);
            bg = UTHEME_SELECTION;
        }
        int dx = r->x + ugfx_char_w(), dy = y + (rh - d) / 2;
        int lit = now || i < wz->step;
        if (lit) {
            ugfx_fill_circle(s, dx + d / 2, dy + d / 2, d / 2, UTHEME_ACCENT);
        } else {
            ugfx_fill_circle(s, dx + d / 2, dy + d / 2, d / 2, UTHEME_OUTLINE);
            ugfx_fill_circle(s, dx + d / 2, dy + d / 2, d / 2 - 1, UTHEME_WHITE);
        }
        if (i < wz->step) {
            draw_tick(s, dx, dy, d, UTHEME_ACCENT_TEXT);
        } else {
            char n[4];
            snprintf(n, sizeof n, "%d", i + 1);
            ugfx_draw_string(s, dx + (d - ugfx_text_width(n)) / 2, dy + (d - ugfx_char_h()) / 2, n,
                             lit ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, lit ? UTHEME_ACCENT : UTHEME_WHITE);
        }
        int tx = dx + d + ugfx_char_w();
        const struct ugfx_font *f = ugfx_set_font(now ? ugfx_font_session(UGFX_FONT_BOLD) : 0);
        uint32_t fg = done || now ? UTHEME_TEXT : uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
        ugfx_draw_string_clipped(s, tx, y + (rh - ugfx_char_h()) / 2, r->x + r->w - tx,
                                 wz->d.steps[i].title, fg, bg);
        ugfx_set_font(f);
    }
}

static int rail_hit(const void *w, int cx, int cy) {
    const struct uui_wizard_rail *r = w;
    return uui_hit(r->x, r->y, r->w, r->h, cx, cy);
}

// A reached step arms on the press and is gone to on the release over the
// same row -- press, then commit on release (docs/gui-guidelines.md).
static int rail_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_wizard_rail *r = w;
    int i = rail_row_at(r, cx, cy);
    r->armed = i >= 0 && i <= r->wz->reached && i != r->wz->step ? i : -1;
    return r->armed >= 0;
}

static int rail_release(void *w, int cx, int cy) {
    struct uui_wizard_rail *r = w;
    int i = r->armed;
    r->armed = -1;
    if (i < 0 || rail_row_at(r, cx, cy) != i) return 0;
    uui_wizard_go(r->wz, i);
    return 1;
}

static void rail_describe(const void *w, const struct uui_describe *d) {
    const struct uui_wizard_rail *r = w;
    for (int i = 0; i < r->wz->d.count; i++)
        uui_describe_rect_i(d, "step", i, r->x, rail_rows_y(r) + i * row_h(), r->w, row_h());
}

static const struct uui_widget_ops RAIL_OPS = {
    .natural_size = rail_natural,
    .set_geometry = rail_geometry,
    .bounds       = rail_bounds,
    .draw         = rail_draw,
    .hit          = rail_hit,
    .press        = rail_press,
    .release      = rail_release,
    .describe     = rail_describe,
};

// --- showing a step -----------------------------------------------------

static void show(struct uui_wizard *w, int step) {
    w->step = step;
    if (step > w->reached) w->reached = step;
    snprintf(w->count_text, sizeof w->count_text, "STEP %d OF %d", step + 1, w->d.count);
    w->title_l.text = w->d.steps[step].title;
    w->note_l.text = w->d.steps[step].note;
    w->body_items[2].hidden = !w->note_l.text || !w->note_l.text[0];
    for (int i = 0; i < w->d.count; i++) w->body_items[3 + i].hidden = i != step;
    w->back.disabled = step == 0;
    int last = step == w->d.count - 1;
    w->next.label = last && w->d.finish ? w->d.finish : "Next";
    // A new label is a new width: lay out again where the wizard already is.
    if (w->w > 0) uui_layout_run(&w->root, w->x, w->y, w->w, w->h);
    if (w->d.on_enter) w->d.on_enter(step);
}

void uui_wizard_go(struct uui_wizard *w, int step) {
    if (step < 0 || step >= w->d.count || step == w->step) return;
    if (step > w->step) {
        // Forward only ONE step at a time and only through can_leave():
        // a later step's check is never jumped over.
        if (step != w->step + 1) return;
        if (w->d.can_leave && !w->d.can_leave(w->step)) return;
    }
    show(w, step);
}

int uui_wizard_step(const struct uui_wizard *w) { return w->step; }

int uui_wizard_action(struct uui_wizard *w, int code) {
    switch (code) {
    case UUI_WIZARD_BACK:
        uui_wizard_go(w, w->step - 1);
        return 1;
    case UUI_WIZARD_NEXT:
        if (w->step < w->d.count - 1) {
            uui_wizard_go(w, w->step + 1);
        } else if (!w->d.can_leave || w->d.can_leave(w->step)) {
            if (w->d.on_finish) w->d.on_finish();
        }
        return 1;
    case UUI_WIZARD_SKIP:
        if (w->d.on_skip) w->d.on_skip();
        return 1;
    }
    return 0;
}

int uui_wizard_focusables(struct uui_wizard *w, const struct uui_widget_ops **ops,
                          void **widgets, int max) {
    struct uui_button *b[3] = { &w->back, &w->next, &w->skip };
    int n = w->d.skip ? 3 : 2, k = 0;
    for (int i = 0; i < n && k < max; i++, k++) {
        ops[k] = &uui_button_ops;
        widgets[k] = b[i];
    }
    return k;
}

// --- the frame ----------------------------------------------------------

void uui_wizard_init(struct uui_wizard *w, const struct uui_wizard_desc *d) {
    memset(w, 0, sizeof *w);
    w->d = *d;
    if (w->d.count > UUI_WIZARD_STEPS_MAX) w->d.count = UUI_WIZARD_STEPS_MAX;
    w->rail.wz = w;
    w->rail.armed = -1;

    uui_label_init(&w->count_l, w->count_text);
    w->count_l.fg = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    uui_label_init(&w->title_l, "");
    w->title_l.font = ugfx_font_session(UGFX_FONT_BOLD);
    uui_label_init(&w->note_l, "");
    uui_label_set_wrap(&w->note_l, 2);
    uui_label_init(&w->spacer, "");
    uui_button_init(&w->back, 0, 0, 0, 0, "Back", UTHEME_BUTTON_BG, UTHEME_TEXT, UUI_WIZARD_BACK);
    uui_button_init(&w->next, 0, 0, 0, 0, "Next", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, UUI_WIZARD_NEXT);
    uui_button_init(&w->skip, 0, 0, 0, 0, d->skip ? d->skip : "", UTHEME_CHROME,
                    utheme_action(UTHEME_ACT_NAV), UUI_WIZARD_SKIP);
    w->back.outlined = w->next.outlined = 1;

    w->left_items[0] = (struct uui_item){ .ops = &RAIL_OPS, .widget = &w->rail,
                                          .flags = UUI_FILL_W | UUI_FILL_H, .name = "rail" };
    w->left_items[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &w->skip,
                                          .id = UUI_WIZARD_SKIP, .name = "skip",
                                          .hidden = !d->skip };
    w->left = (struct uui_layout){ .dir = UUI_COLUMN, .items = w->left_items, .count = 2,
                                   .margin = ugfx_char_w() * 2, .gap = ugfx_char_h() };

    w->body_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &w->count_l,
                                          .flags = UUI_FILL_W, .name = "count" };
    w->body_items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &w->title_l,
                                          .flags = UUI_FILL_W, .name = "title" };
    w->body_items[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &w->note_l,
                                          .flags = UUI_FILL_W, .name = "note" };
    for (int i = 0; i < w->d.count; i++) {
        w->body_items[3 + i] = w->d.steps[i].page;
        w->body_items[3 + i].flags = UUI_FILL_W | UUI_FILL_H;
    }
    w->body = (struct uui_layout){ .dir = UUI_COLUMN, .items = w->body_items,
                                   .count = 3 + w->d.count, .margin = page_pad(),
                                   .gap = ugfx_char_h() / 2 };

    w->bar_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &w->spacer,
                                         .flags = UUI_FILL_W };
    w->bar_items[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &w->back,
                                         .id = UUI_WIZARD_BACK, .name = "back" };
    w->bar_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &w->next,
                                         .id = UUI_WIZARD_NEXT, .name = "next" };
    w->bar = (struct uui_layout){ .dir = UUI_ROW, .items = w->bar_items, .count = 3,
                                  .margin = ugfx_char_w() * 2 };

    w->main_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &w->body,
                                          .flags = UUI_FILL_W | UUI_FILL_H };
    w->main_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &w->bar,
                                          .flags = UUI_FILL_W };
    w->main = (struct uui_layout){ .dir = UUI_COLUMN, .items = w->main_items, .count = 2,
                                   .margin = 1, .gap = 1 };

    w->root_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &w->left,
                                          .flags = UUI_FILL_H, .main_size = rail_w() };
    w->root_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &w->main,
                                          .flags = UUI_FILL_W | UUI_FILL_H };
    w->root = (struct uui_layout){ .dir = UUI_ROW, .items = w->root_items, .count = 2,
                                   .margin = 1, .gap = 1 };
    w->inner = (struct uui_item){ .ops = &uui_layout_ops, .widget = &w->root,
                                  .flags = UUI_FILL_W | UUI_FILL_H };
    w->self_item = (struct uui_item){ .ops = &uui_wizard_ops, .widget = w,
                                      .flags = UUI_FILL_W | UUI_FILL_H, .name = "wizard" };
    show(w, 0);
}

struct uui_item *uui_wizard_item(struct uui_wizard *w) { return &w->self_item; }

// EVERY FONT-DERIVED SIZE IS SET HERE, at layout time, not in init: an app
// may build its wizard before the session font has arrived, and a margin
// measured then is 0 for good.
static void metrics(struct uui_wizard *w) {
    w->title_l.font = ugfx_font_display();
    w->left.margin = ugfx_char_w() * 2;
    w->left.gap = ugfx_char_h();
    w->body.margin = page_pad();
    w->body.gap = ugfx_char_h() / 2;
    w->bar.margin = ugfx_char_w() * 2;
    w->root_items[0].main_size = rail_w();
}

static void wz_natural(const void *p, int *out_w, int *out_h) {
    struct uui_wizard *w = (struct uui_wizard *)p;
    metrics(w);
    w->root.nested = 1;
    uui_layout_natural_size(&w->root, out_w, out_h);
}

static void wz_geometry(void *p, int x, int y, int width, int height) {
    struct uui_wizard *w = p;
    w->x = x; w->y = y; w->w = width; w->h = height;
    metrics(w);
    w->root.nested = 1;
    uui_layout_run(&w->root, x, y, width, height);
}

static void wz_bounds(const void *p, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_wizard *w = p;
    *x = w->x; *y = w->y; *out_w = w->w; *out_h = w->h;
}

// The grounds, under the children: the rail's chrome and the bar's, each
// with a separator hairline -- the app design language's grey chrome. In
// children_begin, because the router paints a container's children and
// never its `draw` (ui/uui_route.c).
static void wz_paint(struct ugfx_surface *s, void *p) {
    const struct uui_wizard *w = p;
    ugfx_fill_rect(s, w->x, w->y, w->w, w->h, UTHEME_WINDOW_BG);
    const struct uui_layout *l = &w->left;
    ugfx_fill_rect(s, w->x, w->y, l->x + l->w - w->x, w->h, UTHEME_CHROME);
    ugfx_fill_rect(s, l->x + l->w, w->y, 1, w->h, UTHEME_SEPARATOR);
    const struct uui_layout *b = &w->bar;
    ugfx_fill_rect(s, b->x, b->y, w->x + w->w - b->x, w->y + w->h - b->y, UTHEME_BAR_BG);
    ugfx_fill_rect(s, b->x, b->y - 1, w->x + w->w - b->x, 1, UTHEME_SEPARATOR);
}

static struct uui_item *wz_children(void *p, int *out_count) {
    struct uui_wizard *w = p;
    *out_count = 1;
    return &w->inner;
}

const struct uui_widget_ops uui_wizard_ops = {
    .natural_size = wz_natural,
    .set_geometry = wz_geometry,
    .bounds       = wz_bounds,
    .children       = wz_children,
    .children_begin = wz_paint,
};
