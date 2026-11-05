#ifndef UUI_STACK_H
#define UUI_STACK_H

// uui_stack -- PAGES IN ONE PLACE, one shown at a time, the whole sized to
// the LARGEST page: Qt's QStackedWidget, GtkStack's homogeneous default.
// An inspector whose page follows a selection, a settings pane, a wizard's
// steps.
//
// WHY NOT `hidden` ON SIBLINGS IN A COLUMN: layout runs at open and on a
// resize only, so hiding one item and showing another while the window is
// open leaves them where the LAST layout put them (Theme Editor's frame
// page drew its rows on top of each other this way); and laid out again,
// the column narrows to each page, so everything beside it jumps. A stack
// places EVERY page in its own rect whenever it is placed, so a switch is
// a flag and a redraw -- nothing moves.
//
// The pages' `hidden` flags are the stack's, set by uui_stack_set(): the
// router and the layout log skip a hidden page as usual, and a focus ring
// skips its controls when told their page (uui_focus.page; ui/uui_builder.h
// sets it).

#include "ui/uui_widget.h"

struct uui_stack {
    int x, y, w, h;
    struct uui_item *pages;   // caller-owned
    int count;
    int current;
};

// Shows page 0.
void uui_stack_init(struct uui_stack *s, struct uui_item *pages, int count);
void uui_stack_set(struct uui_stack *s, int index);
// By the page's item (what ui/uui_builder.h's uui_builder_item() returns):
// 1 when it is one of this stack's pages.
int uui_stack_show(struct uui_stack *s, const struct uui_item *page);

extern const struct uui_widget_ops uui_stack_ops;

#endif
