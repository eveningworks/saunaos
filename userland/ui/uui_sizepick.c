// A picture's size, chosen -- see ui/uui_sizepick.h.
#include "ui/uui_sizepick.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include <stdio.h>
#include <string.h>

enum { ID_CARDS, ID_W, ID_H };

static int card_w(void) { return ugfx_char_w() * 13; }
static int card_h(void) { return ugfx_char_h() * 6; }
static int gap(void)    { return ugfx_char_w(); }

static void card_rect(const struct uui_sizepick_cards *c, int i, int *x, int *y) {
    int cols = c->owner->cols;
    *x = c->x + (i % cols) * (card_w() + gap());
    *y = c->y + (i / cols) * (card_h() + gap());
}

static int card_at(const struct uui_sizepick_cards *c, int cx, int cy) {
    for (int i = 0; i < c->owner->count; i++) {
        int x, y;
        card_rect(c, i, &x, &y);
        if (uui_hit(x, y, card_w(), card_h(), cx, cy)) return i;
    }
    return -1;
}

// The card's picture: the size's shape, scaled into the top of the card
// -- a plain frame for Custom, which has none.
static void draw_card(struct ugfx_surface *s, const struct uui_sizepick *p, int i, int x, int y) {
    const struct uui_sizepick_preset *pr = &p->presets[i];
    int sel = i == p->selected, hot = i == p->cards.hot;
    int r = utheme_radius(UTHEME_R_CONTROL);
    uint32_t bg = sel ? ugfx_blend(UTHEME_WHITE, UTHEME_ACCENT, 40) : UTHEME_WHITE;
    if (hot && !sel) bg = uui_state_bg(bg, UUI_STATE_HOVER);
    uui_fill_round_rect(s, x, y, card_w(), card_h(), r, sel ? UTHEME_ACCENT : UTHEME_OUTLINE);
    uui_fill_round_rect(s, x + (sel ? 2 : 1), y + (sel ? 2 : 1), card_w() - (sel ? 4 : 2),
                        card_h() - (sel ? 4 : 2), r > 1 ? r - 1 : 0, bg);
    int box = ugfx_char_h() * 3, bw = box, bh = box;
    if (pr->w > 0 && pr->h > 0) {
        if (pr->w >= pr->h) bh = box * pr->h / pr->w; else bw = box * pr->w / pr->h;
        if (bw < 4) bw = 4;
        if (bh < 4) bh = 4;
    } else {
        bh = box * 3 / 4;
    }
    int bx = x + (card_w() - bw) / 2, by = y + ugfx_char_h() / 2 + (box - bh) / 2;
    ugfx_fill_rect(s, bx, by, bw, bh, UTHEME_OUTLINE);
    ugfx_fill_rect(s, bx + 1, by + 1, bw - 2, bh - 2, pr->w > 0 ? 0xFFFFFF : bg);
    int ty = y + ugfx_char_h() / 2 + box + 2;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int tw = ugfx_text_width(pr->name);
    ugfx_draw_string_clipped(s, x + (card_w() - tw) / 2, ty, card_w() - 4, pr->name, UTHEME_TEXT, bg);
    ugfx_set_font(was);
    const char *sz = p->size_text[i < 16 ? i : 15];
    tw = ugfx_text_width(sz);
    ugfx_draw_string_clipped(s, x + (card_w() - tw) / 2, ty + ugfx_char_h(), card_w() - 4, sz,
                             ugfx_blend(UTHEME_TEXT, bg, 90), bg);
}

static void cards_natural(const void *w, int *ow, int *oh) {
    const struct uui_sizepick_cards *c = w;
    int rows = (c->owner->count + c->owner->cols - 1) / c->owner->cols;
    *ow = c->owner->cols * (card_w() + gap()) - gap();
    *oh = rows * (card_h() + gap()) - gap();
}

static void cards_geometry(void *w, int x, int y, int width, int height) {
    struct uui_sizepick_cards *c = w;
    c->x = x; c->y = y; c->w = width; c->h = height;
}

static void cards_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_sizepick_cards *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

static void cards_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_sizepick_cards *c = w;
    for (int i = 0; i < c->owner->count; i++) {
        int x, y;
        card_rect(c, i, &x, &y);
        draw_card(s, c->owner, i, x, y);
    }
}

static int cards_hit(const void *w, int cx, int cy) { return card_at(w, cx, cy) >= 0; }

static int cards_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_sizepick_cards *c = w;
    c->armed = card_at(c, cx, cy);
    return c->armed >= 0;
}

static int cards_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_sizepick_cards *c = w;
    int h = card_at(c, cx, cy);
    if (h == c->hot) return 0;
    c->hot = h;
    return 1;
}

static int cards_release(void *w, int cx, int cy) {
    struct uui_sizepick_cards *c = w;
    int at = card_at(c, cx, cy), was = c->armed;
    c->armed = -1;
    if (was >= 0 && at == was) c->picked = at;
    return was >= 0;
}

static void cards_describe(const void *w, const struct uui_describe *d) {
    const struct uui_sizepick_cards *c = w;
    uui_describe_int(d, "selected", c->owner->selected);
    for (int i = 0; i < c->owner->count; i++) {
        int x, y;
        card_rect(c, i, &x, &y);
        uui_describe_rect_i(d, "card", i, x, y, card_w(), card_h());
    }
}

static const struct uui_widget_ops cards_ops = {
    .natural_size = cards_natural,
    .set_geometry = cards_geometry,
    .bounds       = cards_bounds,
    .draw         = cards_draw,
    .hit          = cards_hit,
    .press        = cards_press,
    .motion       = cards_motion,
    .release      = cards_release,
    .describe     = cards_describe,
};

static void draw_text(struct ugfx_surface *s, const struct uui_custom *cu) {
    ugfx_draw_string_clipped(s, cu->x, cu->y + (cu->h - ugfx_char_h()) / 2, cu->w,
                             (const char *)cu->state, UTHEME_TEXT, UGFX_TRANSPARENT);
}

// The card a size matches: a preset of that size, else Custom.
static int match(const struct uui_sizepick *p, int w, int h) {
    int custom = -1;
    for (int i = 0; i < p->count; i++) {
        if (p->presets[i].w <= 0) { if (custom < 0) custom = i; continue; }
        if (p->presets[i].w == w && p->presets[i].h == h) return i;
    }
    return custom;
}

void uui_sizepick_init(struct uui_sizepick *p, int id_base,
                       const struct uui_sizepick_preset *presets, int count,
                       int w, int h, int max) {
    memset(p, 0, sizeof *p);
    p->id_base = id_base;
    p->presets = presets;
    p->count = count;
    p->cols = 4;
    for (int i = 0; i < count && i < 16; i++) {
        if (presets[i].w > 0) snprintf(p->size_text[i], sizeof p->size_text[i], "%d \xd7 %d", presets[i].w, presets[i].h);
        else snprintf(p->size_text[i], sizeof p->size_text[i], "your size");
    }
    p->cards = (struct uui_sizepick_cards){ .owner = p, .hot = -1, .armed = -1, .picked = -1 };
    int lw = ugfx_text_width("Height") + ugfx_char_w();
    p->w_l = (struct uui_custom){ .w = lw, .h = utheme_control_h(), .draw = draw_text, .state = (void *)"Width" };
    p->h_l = (struct uui_custom){ .w = lw, .h = utheme_control_h(), .draw = draw_text, .state = (void *)"Height" };
    uui_spinbox_init(&p->wbox, w, 1, max, 1, "px");
    uui_spinbox_init(&p->hbox, h, 1, max, 1, "px");
    p->w_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &p->w_l };
    p->w_it[1] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &p->wbox, .id = id_base + ID_W, .name = "width" };
    p->h_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &p->h_l };
    p->h_it[1] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &p->hbox, .id = id_base + ID_H, .name = "height" };
    p->w_row = (struct uui_layout){ .dir = UUI_ROW, .items = p->w_it, .count = 2 };
    p->h_row = (struct uui_layout){ .dir = UUI_ROW, .items = p->h_it, .count = 2 };
    p->side_it[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &p->w_row };
    p->side_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &p->h_row };
    p->side = (struct uui_layout){ .dir = UUI_COLUMN, .items = p->side_it, .count = 2 };
    p->top_it[0] = (struct uui_item){ .ops = &cards_ops, .widget = &p->cards, .id = id_base + ID_CARDS, .name = "sizes" };
    p->top_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &p->side };
    p->top = (struct uui_layout){ .dir = UUI_ROW, .items = p->top_it, .count = 2 };
    p->selected = match(p, w, h);
}

struct uui_item uui_sizepick_item(struct uui_sizepick *p) {
    return (struct uui_item){ .ops = &uui_layout_ops, .widget = &p->top, .flags = UUI_FILL_W };
}

int uui_sizepick_focusables(struct uui_sizepick *p, struct uui_focusable *out, int max) {
    if (max < 2) return 0;
    out[0] = (struct uui_focusable){ &p->wbox, &uui_spinbox_ops };
    out[1] = (struct uui_focusable){ &p->hbox, &uui_spinbox_ops };
    return 2;
}

void uui_sizepick_size(const struct uui_sizepick *p, int *w, int *h) {
    *w = uui_spinbox_value(&p->wbox);
    *h = uui_spinbox_value(&p->hbox);
}

// What a field SAYS while it is being typed in -- the spinbox adopts it
// only on Enter -- so the cards follow each keystroke. Anything that is
// not a number in range reads as the field's current value.
static int typed(const struct uui_spinbox *s) {
    const char *t = uui_textbox_text(&s->field);
    int v = 0, n = 0;
    for (; t[n] >= '0' && t[n] <= '9' && n < 6; n++) v = v * 10 + (t[n] - '0');
    if (!n || t[n] || v < s->min || v > s->max) return uui_spinbox_value(s);
    return v;
}

int uui_sizepick_commit(struct uui_sizepick *p) {
    int changed = uui_spinbox_commit(&p->wbox) | uui_spinbox_commit(&p->hbox);
    int w, h;
    uui_sizepick_size(p, &w, &h);
    p->selected = match(p, w, h);
    return changed;
}

int uui_sizepick_on_widget(struct uui_sizepick *p, int id) {
    int k = id - p->id_base;
    if (k < 0 || k >= UUI_SIZEPICK_IDS) return 0;
    int w0, h0, w, h;
    uui_sizepick_size(p, &w0, &h0);
    if (k == ID_CARDS) {
        int i = p->cards.picked;
        p->cards.picked = -1;
        if (i < 0) return 0;
        p->selected = i;
        // Custom keeps the numbers, ready to be typed over.
        if (p->presets[i].w > 0) {
            uui_spinbox_set_value(&p->wbox, p->presets[i].w);
            uui_spinbox_set_value(&p->hbox, p->presets[i].h);
        }
    } else {
        int m = match(p, typed(&p->wbox), typed(&p->hbox));
        // Typed or stepped: the card follows, Custom unless it is a preset.
        if (m >= 0) p->selected = m;
    }
    uui_sizepick_size(p, &w, &h);
    return w != w0 || h != h0;
}
