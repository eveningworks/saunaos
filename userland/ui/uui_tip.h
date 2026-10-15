#ifndef UUI_TIP_H
#define UUI_TIP_H

// uui_tip -- A TOOLTIP for one widget's parts: shown once the pointer has
// rested on one of them for UUI_TOOLTIP_DELAY_TICKS, in a popup surface
// the compositor places beside it (below, or to the right of a vertical
// strip), and taken down when the pointer moves on. The toolbar's and the
// toggle bar's; any widget with icon-only buttons.
//
// THE APP'S TICK DRIVES IT -- nothing here has a timer: a widget hands
// on its own tick (uui_toolbar_tick(), uui_togglebar_tick()), and an app
// whose tick is slow while idle asks uui_tip_waiting() to know when to
// tick faster.

#include <stdint.h>
#include "ui/ugfx.h"

// ~500ms at the PIT's 100 Hz -- every desktop delays about this long.
#define UUI_TOOLTIP_DELAY_TICKS 50
#define UUI_TIP_PAD 4 // inset around the tip's text
#define UUI_TIP_GAP 3 // between the button and the tip, in-window only

struct uui_tip {
    int key;               // what the pointer rests on; -1 nothing
    unsigned long since;   // sys_ticks() when it arrived
    int shown;
    int popup;             // its surface, or 0 (drawn in the window instead)
};

void uui_tip_init(struct uui_tip *t);
// The pointer is over `key` now (-1: over nothing of ours): a different
// one starts the wait again and takes the old tip down.
void uui_tip_hover(struct uui_tip *t, int key);
// On a tick: up once the wait is over, for `text` beside the anchor rect
// (`beside`: to the right, else below). NULL text keeps it down. 1 when it
// came up or went down -- a redraw.
int  uui_tip_tick(struct uui_tip *t, const char *text, int bx, int by, int bw, int bh, int beside);
// Its card: in its own surface, or in `s` beside the anchor when the
// compositor gave none. A router overlay pass calls this.
void uui_tip_draw(struct ugfx_surface *s, const struct uui_tip *t, const char *text, int bx, int by, int bw, int bh,
                  int beside, uint32_t bg, uint32_t fg, uint32_t border);
void uui_tip_hide(struct uui_tip *t);
// Resting on something whose tip is not up yet: tick soon.
int  uui_tip_waiting(const struct uui_tip *t);

#endif
