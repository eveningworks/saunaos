#ifndef UUI_CHOICELIST_H
#define UUI_CHOICELIST_H

#include "ui/ugfx.h"
#include "ui/uui_widget.h"

// ONE CHOICE OUT OF A FEW, EACH A ROW THAT SAYS WHAT IT IS: a radio, a
// bold title with small tags beside it ("default", "internal"), a line of
// description under it and a value at the right -- System Update's
// channels, Windows' Insider channel picker. uui_radio_list is the same
// choice with one line per option; this is for options a word does not
// explain.
//
// Arrowing moves the selection itself, as a radio group does, so the
// focus ring is on the selected row and there is no separate cursor.

struct uui_choice {
    const char *title;
    const char *tags;    // space-separated words, each drawn as a pill; NULL for none
    const char *desc;    // NULL for none
    const char *value;   // right-aligned, muted; NULL for none
};

struct uui_choicelist {
    int x, y, w, h;
    const struct uui_choice *items;   // caller-owned
    int count;
    int selected;                     // the value, or -1
    int hovered, armed;               // OWNED: rows, or -1
    int focused;                      // OWNED
    int disabled;
};

void uui_choicelist_init(struct uui_choicelist *l, const struct uui_choice *items, int count,
                         int selected);
void uui_choicelist_natural_size(const struct uui_choicelist *l, int *out_w, int *out_h);
void uui_choicelist_set_geometry(struct uui_choicelist *l, int x, int y, int w, int h);
void uui_choicelist_draw(struct ugfx_surface *s, const struct uui_choicelist *l);
// The row at a point, or -1.
int  uui_choicelist_at(const struct uui_choicelist *l, int cx, int cy);

extern const struct uui_widget_ops uui_choicelist_ops;

#endif
