// See uui_colourpick.h.
#include "ui/uui_colourpick.h"
#include "ui/uui_primitives.h"
#include "knum.h"
#include <stdio.h>
#include <string.h>

enum { ID_HEX, ID_H, ID_S, ID_L };

static void draw_swatch(struct ugfx_surface *s, const struct uui_custom *cu) {
    const struct uui_colourpick *c = cu->state;
    int r = utheme_radius(UTHEME_R_CONTROL);
    uui_fill_round_rect(s, cu->x, cu->y, cu->w, cu->h, r, UTHEME_OUTLINE);
    uui_fill_round_rect(s, cu->x + 1, cu->y + 1, cu->w - 2, cu->h - 2, r > 1 ? r - 1 : 0, c->rgb);
}

// The texts and the scales from `rgb`/`hsl`, whichever was not just moved.
static void show(struct uui_colourpick *c, int from_field) {
    snprintf(c->hex_text, sizeof c->hex_text, "%06X", (unsigned)(c->rgb & 0xFFFFFF));
    if (!from_field) uui_textbox_set_text(&c->hex, c->hex_text);
    uui_scale_set_value(&c->h, c->hsl.h);
    uui_scale_set_value(&c->s, c->hsl.s);
    uui_scale_set_value(&c->l, c->hsl.l);
    snprintf(c->h_text, sizeof c->h_text, "%d", c->hsl.h);
    snprintf(c->s_text, sizeof c->s_text, "%d%%", c->hsl.s);
    snprintf(c->l_text, sizeof c->l_text, "%d%%", c->hsl.l);
}

static void draw_text(struct ugfx_surface *s, const struct uui_custom *cu) {
    ugfx_draw_string_clipped(s, cu->x, cu->y + (cu->h - ugfx_char_h()) / 2, cu->w,
                             (const char *)cu->state, UTHEME_TEXT, UGFX_TRANSPARENT);
}

static struct uui_custom text_cell(const char *text, int w) {
    return (struct uui_custom){ .w = w, .h = utheme_control_h(), .draw = draw_text, .state = (void *)text };
}

static void row(struct uui_layout *l, struct uui_item *it, struct uui_custom *lab, const char *text,
                struct uui_scale *sc, long max, int id, struct uui_custom *val, char *val_text) {
    int ch = ugfx_char_h();
    *lab = text_cell(text, ugfx_text_width("Saturation") + ch / 2);
    uui_scale_init(sc, 0, max, 0);
    *val = text_cell(val_text, ugfx_text_width("359%") + ch / 2);
    it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = lab };
    it[1] = (struct uui_item){ .ops = &uui_scale_ops, .widget = sc, .id = id, .flags = UUI_FILL_W };
    it[2] = (struct uui_item){ .ops = &uui_custom_ops, .widget = val };
    *l = (struct uui_layout){ .dir = UUI_ROW, .items = it, .count = 3 };
}

void uui_colourpick_init(struct uui_colourpick *c, int id_base, uint32_t rgb) {
    memset(c, 0, sizeof *c);
    c->id_base = id_base;
    int ch = ugfx_char_h();
    c->swatch = (struct uui_custom){ .w = ch * 10, .h = ch * 3, .draw = draw_swatch, .state = c };
    c->hex_l = text_cell("Colour", ugfx_text_width("Saturation") + ch / 2);
    uui_textbox_init(&c->hex, "");
    c->hex_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &c->hex_l };
    c->hex_it[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &c->hex, .id = id_base + ID_HEX,
                                      .flags = UUI_FILL_W, .name = "colour_hex" };
    c->hex_row = (struct uui_layout){ .dir = UUI_ROW, .items = c->hex_it, .count = 2 };
    row(&c->h_row, c->h_it, &c->h_l, "Hue", &c->h, 359, id_base + ID_H, &c->h_v, c->h_text);
    row(&c->s_row, c->s_it, &c->s_l, "Saturation", &c->s, 100, id_base + ID_S, &c->s_v, c->s_text);
    row(&c->l_row, c->l_it, &c->l_l, "Lightness", &c->l, 100, id_base + ID_L, &c->l_v, c->l_text);
    c->col_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &c->swatch, .flags = UUI_FILL_W,
                                      .name = "colour_swatch" };
    c->col_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &c->hex_row, .flags = UUI_FILL_W };
    c->col_it[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &c->h_row, .flags = UUI_FILL_W };
    c->col_it[3] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &c->s_row, .flags = UUI_FILL_W };
    c->col_it[4] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &c->l_row, .flags = UUI_FILL_W };
    c->col = (struct uui_layout){ .dir = UUI_COLUMN, .items = c->col_it, .count = 5 };
    uui_colourpick_set(c, rgb);
}

void uui_colourpick_set(struct uui_colourpick *c, uint32_t rgb) {
    c->rgb = rgb & 0xFFFFFF;
    ucolour_to_hsl(c->rgb, &c->hsl);
    show(c, 0);
}

struct uui_item uui_colourpick_item(struct uui_colourpick *c) {
    return (struct uui_item){ .ops = &uui_layout_ops, .widget = &c->col, .flags = UUI_FILL_W };
}

int uui_colourpick_focusables(struct uui_colourpick *c, struct uui_focusable *out, int max) {
    if (max < 4) return 0;
    out[0] = (struct uui_focusable){ &c->hex, &uui_textbox_focus_ops };
    out[1] = (struct uui_focusable){ &c->h, &uui_scale_focus_ops };
    out[2] = (struct uui_focusable){ &c->s, &uui_scale_focus_ops };
    out[3] = (struct uui_focusable){ &c->l, &uui_scale_focus_ops };
    return 4;
}

int uui_colourpick_on_widget(struct uui_colourpick *c, int id) {
    int k = id - c->id_base;
    if (k < 0 || k >= UUI_COLOURPICK_IDS) return 0;
    uint32_t was = c->rgb;
    if (k == ID_HEX) {
        // Six hex digits and nothing else; a half-typed value waits.
        const char *t = uui_textbox_text(&c->hex);
        uint32_t v = 0;
        int i = 0;
        for (; t[i] && i < 6; i++) {
            int d = k_hex_digit(t[i]);
            if (d < 0) return 0;
            v = (v << 4) | (uint32_t)d;
        }
        if (i != 6 || t[6]) return 0;
        c->rgb = v;
        ucolour_to_hsl(v, &c->hsl);
        show(c, 1);
    } else {
        c->hsl.h = (int)c->h.value;
        c->hsl.s = (int)c->s.value;
        c->hsl.l = (int)c->l.value;
        c->rgb = ucolour_from_hsl(&c->hsl);
        show(c, 0);
    }
    return c->rgb != was;
}
