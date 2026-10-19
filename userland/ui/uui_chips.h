#ifndef UUI_CHIPS_H
#define UUI_CHIPS_H

#include <stdint.h>
#include "ui/ugfx.h"

// FILTER CHIPS: a row of rounded pills, each a label and a count, one
// chosen -- "All 50", "USER 26", "KERNEL 9" over a list the choice
// filters. Material's filter chips and GNOME's pill rows; uui_segmented
// is the same idea for two to four short options that fit side by side.
//
// THE APP OWNS THE DATA: `labels` and `counts` are its arrays, as many
// as it has, read at draw time -- grow them and update `count`. The
// chips WRAP onto as many rows as the width they are given needs; ask
// uui_chips_height_for() how tall that is before laying out below them.
//
// Commits on RELEASE over the chip that was pressed, like a button, and
// the arrows move the choice while it has focus; the router then names
// the widget to the app (on_widget) and `selected` holds the choice.

struct uui_chips {
    int x, y, w, h;
    const char *const *labels;      // caller-owned, `count` of them
    const int *counts;              // caller-owned; NULL, or < 0 in one: no number
    int count;
    int selected;                   // the value, or -1
    int hovered, armed;             // OWNED: chip indices, or -1
    int focused;                    // OWNED
};

void uui_chips_init(struct uui_chips *c, const char *const *labels, const int *counts,
                    int count, int selected);
// One row holding every chip, the size a layout asks for first.
void uui_chips_natural_size(const struct uui_chips *c, int *out_w, int *out_h);
// How tall the chips are when they wrap inside `width`.
int  uui_chips_height_for(const struct uui_chips *c, int width);
void uui_chips_set_geometry(struct uui_chips *c, int x, int y, int width, int height);
void uui_chips_draw(struct ugfx_surface *s, const struct uui_chips *c);
// The chip under (cx, cy), or -1.
int  uui_chips_at(const struct uui_chips *c, int cx, int cy);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_chips_ops;

#endif
