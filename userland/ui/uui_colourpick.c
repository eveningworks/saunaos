// See uui_colourpick.h.
#include "ui/uui_colourpick.h"
#include "ui/uui_primitives.h"
#include "knum.h"
#include <stdio.h>
#include <string.h>

enum { ID_HEX, ID_SQUARE, ID_HUE, ID_R, ID_G, ID_B };

static void draw_swatch(struct ugfx_surface *s, const struct uui_custom *cu) {
    const struct uui_colourpick *c = cu->state;
    int r = utheme_radius(UTHEME_R_CONTROL);
    uui_fill_round_rect(s, cu->x, cu->y, cu->w, cu->h, r, UTHEME_OUTLINE);
    uui_fill_round_rect(s, cu->x + 1, cu->y + 1, cu->w - 2, cu->h - 2, r > 1 ? r - 1 : 0, c->rgb);
}

// The fields from `rgb`, all but the one being typed in (`typing`, an
// ID_ or -1): rewriting a field under its caret would fight the typist.
static void show(struct uui_colourpick *c, int typing) {
    snprintf(c->hex_text, sizeof c->hex_text, "%06X", (unsigned)(c->rgb & 0xFFFFFF));
    if (typing != ID_HEX) uui_textbox_set_text(&c->hex, c->hex_text);
    struct uui_textbox *f[3] = { &c->r, &c->g, &c->b };
    for (int i = 0; i < 3; i++) {
        snprintf(c->rgb_text[i], sizeof c->rgb_text[i], "%u", (unsigned)(c->rgb >> (16 - 8 * i)) & 0xFF);
        if (typing != ID_R + i) uui_textbox_set_text(f[i], c->rgb_text[i]);
    }
}

static void draw_text(struct ugfx_surface *s, const struct uui_custom *cu) {
    ugfx_draw_string_clipped(s, cu->x, cu->y + (cu->h - ugfx_char_h()) / 2, cu->w,
                             (const char *)cu->state, UTHEME_TEXT, UGFX_TRANSPARENT);
}

static struct uui_custom text_cell(const char *text) {
    return (struct uui_custom){ .w = ugfx_text_width(text) + ugfx_char_w() / 2, .h = utheme_control_h(),
                                .draw = draw_text, .state = (void *)text };
}

void uui_colourpick_init(struct uui_colourpick *c, int id_base, uint32_t rgb) {
    memset(c, 0, sizeof *c);
    c->id_base = id_base;
    int ch = ugfx_char_h(), pad = ugfx_char_w();
    c->swatch = (struct uui_custom){ .w = ch * 10, .h = ch * 3, .draw = draw_swatch, .state = c };
    uui_colourfield_init(&c->square, UUI_CF_SQUARE, &c->hsv);
    uui_colourfield_init(&c->hue, UUI_CF_HUE, &c->hsv);
    c->hex_l = text_cell("Hex");
    c->r_l = text_cell("R");
    c->g_l = text_cell("G");
    c->b_l = text_cell("B");
    uui_textbox_init(&c->hex, "");
    uui_textbox_init(&c->r, "");
    uui_textbox_init(&c->g, "");
    uui_textbox_init(&c->b, "");
    int num_w = ugfx_text_width("255") + pad * 2;
    struct uui_item *f = c->field_it;
    f[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &c->hex_l };
    f[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &c->hex, .id = id_base + ID_HEX, .flags = UUI_FILL_W,
                              .main_size = ugfx_text_width("FFFFFF") + pad * 2, .name = "colour_hex" };
    f[2] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &c->r_l };
    f[3] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &c->r, .id = id_base + ID_R, .main_size = num_w,
                              .name = "colour_r" };
    f[4] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &c->g_l };
    f[5] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &c->g, .id = id_base + ID_G, .main_size = num_w,
                              .name = "colour_g" };
    f[6] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &c->b_l };
    f[7] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &c->b, .id = id_base + ID_B, .main_size = num_w,
                              .name = "colour_b" };
    c->field_row = (struct uui_layout){ .dir = UUI_ROW, .items = c->field_it, .count = 8, .gap = pad / 2 };
    c->col_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &c->swatch, .flags = UUI_FILL_W,
                                      .hidden = 1, .name = "colour_swatch" };
    c->col_it[1] = (struct uui_item){ .ops = &uui_colourfield_ops, .widget = &c->square, .id = id_base + ID_SQUARE,
                                      .flags = UUI_FILL_W, .name = "colour_square" };
    c->col_it[2] = (struct uui_item){ .ops = &uui_colourfield_ops, .widget = &c->hue, .id = id_base + ID_HUE,
                                      .flags = UUI_FILL_W, .name = "colour_hue" };
    c->col_it[3] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &c->field_row, .flags = UUI_FILL_W };
    c->col = (struct uui_layout){ .dir = UUI_COLUMN, .items = c->col_it, .count = 4 };
    uui_colourpick_set(c, rgb);
}

void uui_colourpick_show_swatch(struct uui_colourpick *c, int on) { c->col_it[0].hidden = !on; }

void uui_colourpick_free(struct uui_colourpick *c) {
    uui_colourfield_free(&c->square);
    uui_colourfield_free(&c->hue);
}

void uui_colourpick_set(struct uui_colourpick *c, uint32_t rgb) {
    c->rgb = rgb & 0xFFFFFF;
    ucolour_to_hsv(c->rgb, &c->hsv);
    show(c, -1);
}

struct uui_item uui_colourpick_item(struct uui_colourpick *c) {
    return (struct uui_item){ .ops = &uui_layout_ops, .widget = &c->col, .flags = UUI_FILL_W };
}

int uui_colourpick_focusables(struct uui_colourpick *c, struct uui_focusable *out, int max) {
    if (max < 6) return 0;
    out[0] = (struct uui_focusable){ &c->square, &uui_colourfield_ops };
    out[1] = (struct uui_focusable){ &c->hue, &uui_colourfield_ops };
    out[2] = (struct uui_focusable){ &c->hex, &uui_textbox_focus_ops };
    out[3] = (struct uui_focusable){ &c->r, &uui_textbox_focus_ops };
    out[4] = (struct uui_focusable){ &c->g, &uui_textbox_focus_ops };
    out[5] = (struct uui_focusable){ &c->b, &uui_textbox_focus_ops };
    return 6;
}

// 0..255 written in full, or -1.
static int byte_field(const char *t) {
    int v = 0, n = 0;
    for (; t[n]; n++) {
        if (t[n] < '0' || t[n] > '9' || n >= 3) return -1;
        v = v * 10 + (t[n] - '0');
    }
    return n && v <= 255 ? v : -1;
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
        ucolour_to_hsv(v, &c->hsv);
    } else if (k >= ID_R) {
        struct uui_textbox *f[3] = { &c->r, &c->g, &c->b };
        int v = byte_field(uui_textbox_text(f[k - ID_R]));
        if (v < 0) return 0;
        int sh = 16 - 8 * (k - ID_R);
        c->rgb = (c->rgb & ~(0xFFu << sh)) | (uint32_t)v << sh;
        ucolour_to_hsv(c->rgb, &c->hsv);
    } else {
        // The square or the strip moved hsv itself.
        c->rgb = ucolour_from_hsv(&c->hsv);
    }
    show(c, k);
    return c->rgb != was;
}
