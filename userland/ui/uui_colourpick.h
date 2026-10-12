#ifndef UUI_COLOURPICK_H
#define UUI_COLOURPICK_H

// uui_colourpick -- ONE COLOUR, picked: a swatch, its hex value in a
// field, and hue, saturation and lightness on three scales that follow
// each other. The Theme Editor's inspector; any app choosing a colour.
//
// A PANEL OF STANDARD CONTROLS, as uui_sndformat is: the app embeds
// uui_colourpick_item() in its layout, adds the focusables to its ring,
// and offers each on_widget id to uui_colourpick_on_widget() first --
// which answers 1 when the colour changed. The hex field takes six hex
// digits and nothing else; anything else leaves the colour as it was.

#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_scale.h"
#include "ui/uui_textbox.h"
#include "ui/uui_focus.h"
#include "lib/ucolour.h"

// The ids it reports under: id_base .. id_base + UUI_COLOURPICK_IDS - 1.
#define UUI_COLOURPICK_IDS 4

struct uui_colourpick {
    int id_base;
    uint32_t rgb;                 // 0xRRGGBB
    struct ucolour_hsl hsl;
    struct uui_custom swatch;
    // Captions and readouts are fixed-width text cells, so the three
    // scales line up whatever their words are.
    struct uui_custom hex_l, h_l, s_l, l_l, h_v, s_v, l_v;
    struct uui_textbox hex;
    struct uui_scale h, s, l;
    char hex_text[8], h_text[8], s_text[8], l_text[8];
    struct uui_item hex_it[2], h_it[3], s_it[3], l_it[3], col_it[5];
    struct uui_layout hex_row, h_row, s_row, l_row, col;
};

void uui_colourpick_init(struct uui_colourpick *c, int id_base, uint32_t rgb);
void uui_colourpick_set(struct uui_colourpick *c, uint32_t rgb);
struct uui_item uui_colourpick_item(struct uui_colourpick *c);
// Its controls, in tab order, appended to `out`; returns how many.
int uui_colourpick_focusables(struct uui_colourpick *c, struct uui_focusable *out, int max);
// 1 when `id` was one of its controls AND the colour changed.
int uui_colourpick_on_widget(struct uui_colourpick *c, int id);

#endif
