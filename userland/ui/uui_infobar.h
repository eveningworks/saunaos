#ifndef UUI_INFOBAR_H
#define UUI_INFOBAR_H

// AN INFO BAR across the top of a view: an icon, one line saying what
// is going on, and at most one action -- "Opened for this session" with
// Save to Network, a read-only medium with Open as writable. KDE's
// KMessageWidget and Firefox's notification bar; Windows puts the same
// thing in the yellow strip of an Explorer window.
//
// Drawn in the accent's tint, so it reads as information rather than a
// warning. The click is PARKED (uui_infobar_take()) as a dialog's is:
// the router can only say the bar was released.
#include "ui/ugfx.h"
#include "ui/uui_button.h"

struct uui_infobar {
    int x, y, w, h;
    const char *text;          // caller-owned
    const char *icon;          // icon_get() name, or NULL
    struct uui_button action;  // label NULL: no action
    int pressed;               // OWNED: a press landed on the action
    int clicked;               // parked, taken by uui_infobar_take()
};

void uui_infobar_init(struct uui_infobar *b, const char *icon, const char *text,
                      const char *action_label);
// 1 if the action was clicked since the last call.
int uui_infobar_take(struct uui_infobar *b);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_infobar_ops;

#endif
