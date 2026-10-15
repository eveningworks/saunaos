#ifndef UUI_COLOURPICK_H
#define UUI_COLOURPICK_H

// uui_colourpick -- ONE COLOUR, picked: by eye on a saturation and
// brightness square over a hue strip (ui/uui_colourfield.h), or exactly
// in a hex field and R, G and B fields, all following each other. An
// optional swatch above shows the colour itself. The Theme Editor's
// inspector and Paint's panel; any app choosing a colour.
//
// A PANEL OF STANDARD CONTROLS, as uui_sndformat is: the app embeds
// uui_colourpick_item() in its layout, adds the focusables to its ring,
// and offers each on_widget id to uui_colourpick_on_widget() first --
// which answers 1 when the colour changed. A field takes a whole value
// or nothing: six hex digits, or 0 to 255; anything half-typed leaves
// the colour as it was until it is whole.

#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_textbox.h"
#include "ui/uui_focus.h"
#include "lib/ucolour.h"
#include "ui/uui_colourfield.h"

// The ids it reports under: id_base .. id_base + UUI_COLOURPICK_IDS - 1.
#define UUI_COLOURPICK_IDS 6

struct uui_colourpick {
    int id_base;
    uint32_t rgb;                 // 0xRRGGBB
    struct ucolour_hsv hsv;       // what the square and the strip show
    struct uui_custom swatch;
    struct uui_colourfield square, hue;
    // Captions are text cells; the fields' widths are pinned (main_size)
    // to what they hold, and the hex field takes what is left.
    struct uui_custom hex_l, r_l, g_l, b_l;
    struct uui_textbox hex, r, g, b;
    char hex_text[8], rgb_text[3][4];
    struct uui_item field_it[8], col_it[4];
    struct uui_layout field_row, col;
};

void uui_colourpick_init(struct uui_colourpick *c, int id_base, uint32_t rgb);
void uui_colourpick_set(struct uui_colourpick *c, uint32_t rgb);
// The swatch above the square: off unless asked for (Paint's panel has
// its own colour wells).
void uui_colourpick_show_swatch(struct uui_colourpick *c, int on);
void uui_colourpick_free(struct uui_colourpick *c);
struct uui_item uui_colourpick_item(struct uui_colourpick *c);
// Its controls, in tab order, appended to `out`; returns how many.
int uui_colourpick_focusables(struct uui_colourpick *c, struct uui_focusable *out, int max);
// 1 when `id` was one of its controls AND the colour changed.
int uui_colourpick_on_widget(struct uui_colourpick *c, int id);

#endif
