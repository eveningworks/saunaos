#ifndef UUI_TOGGLEBAR_H
#define UUI_TOGGLEBAR_H

// uui_togglebar -- A ROW OF LATCHING BUTTONS: B I U S, Windows 11 Paint's
// and every word processor's text bar. A button is an icon, a word, a
// colour chip, or a mix (a "Fill" with Colour 2's chip). An icon alone is
// a control-height square; a latched button fills in the accent, a click
// commits on release.
//
// The latches are the caller's bits (`on`), so the bar draws state it
// does not own; a click parks the button's index for
// uui_togglebar_take(), the menu bar's arrangement.

#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_tip.h"

struct uui_toggle {
    const char *label;        // a word ("Fill"), or NULL for an icon alone
    const char *tip;          // what it does: its tooltip, the layout log's, a test's
    const char *icon;         // icon_get() name before the label, or NULL
    const uint32_t *chip;     // 0xRRGGBB to show after the label, or NULL
};

#define UUI_TOGGLEBAR_MAX 12

struct uui_togglebar {
    int x, y, w, h;
    const struct uui_toggle *items;   // caller-owned
    int count;
    unsigned on;          // bit i: item i latched -- the caller's state
    int hot, armed, picked;           // OWNED
    struct uui_tip tooltip;           // OWNED
};

void uui_togglebar_init(struct uui_togglebar *t, const struct uui_toggle *items, int count);
// A committed click, once: the item's index, or -1.
int  uui_togglebar_take(struct uui_togglebar *t);
// The hovered button's tooltip, on the app's tick (ui/uui_tip.h): 1 when
// it came up or went down.
int  uui_togglebar_tick(struct uui_togglebar *t);
int  uui_togglebar_tip_waiting(const struct uui_togglebar *t);

extern const struct uui_widget_ops uui_togglebar_ops;

#endif
