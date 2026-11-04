#ifndef UUI_SIZEPICK_H
#define UUI_SIZEPICK_H

// uui_sizepick -- A PICTURE'S SIZE, CHOSEN: preset cards that each show
// their size's SHAPE, and a width and a height beside them --
// Photoshop's New Document and Paint.NET's presets. Picking a card fills
// the two fields; typing a size picks the Custom card (or the preset it
// happens to equal).
//
// A PANEL OF CONTROLS, uui_colourpick's arrangement: embed
// uui_sizepick_item() in a layout (a dialog's body), add the focusables
// to the ring, and offer every on_widget id to uui_sizepick_on_widget()
// first. Call uui_sizepick_commit() before reading the size on OK -- a
// spinbox adopts what was typed only on Enter or focus loss.

#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_layout.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_focus.h"

// A card. `w` <= 0 is CUSTOM: no size of its own, chosen by typing.
struct uui_sizepick_preset {
    const char *name;
    int w, h;
};

// The ids it reports under: id_base .. id_base + UUI_SIZEPICK_IDS - 1.
#define UUI_SIZEPICK_IDS 3

struct uui_sizepick_cards {
    int x, y, w, h;
    struct uui_sizepick *owner;
    int hot, armed, picked;   // OWNED: card indices, -1 for none
};

struct uui_sizepick {
    int id_base;
    const struct uui_sizepick_preset *presets;   // caller-owned
    int count;
    int selected;                                // the card shown chosen
    int cols;                                    // cards per row
    char size_text[16][24];                      // "1280 x 720", per card
    struct uui_sizepick_cards cards;
    struct uui_custom w_l, h_l;
    struct uui_spinbox wbox, hbox;
    struct uui_item w_it[2], h_it[2], side_it[2], top_it[2];
    struct uui_layout w_row, h_row, side, top;
};

// `w` x `h` to start at; `max` bounds both fields.
void uui_sizepick_init(struct uui_sizepick *p, int id_base,
                       const struct uui_sizepick_preset *presets, int count,
                       int w, int h, int max);
struct uui_item uui_sizepick_item(struct uui_sizepick *p);
int  uui_sizepick_focusables(struct uui_sizepick *p, struct uui_focusable *out, int max);
// 1 when `id` was one of its controls AND the size changed.
int  uui_sizepick_on_widget(struct uui_sizepick *p, int id);
// Adopts what is typed in either field; 1 if the size changed.
int  uui_sizepick_commit(struct uui_sizepick *p);
void uui_sizepick_size(const struct uui_sizepick *p, int *w, int *h);

#endif
