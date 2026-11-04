#ifndef UUI_CTXMENU_H
#define UUI_CTXMENU_H

#include "ui/uui_menubar.h"
#include "ui/uui_widget.h"

// uui_ctxmenu -- a WIDGET'S OWN right-click menu, hosted by uapp: a
// table header's column chooser is the first. The widget answers
// uui_widget_ops.context_menu with its rows and two callbacks; uapp
// opens this one menu on the secondary release, routes it through the
// router's `extra` while it is up, and runs the chosen row on the widget
// -- uui_editmenu's arrangement, for any widget rather than editable
// text. Qt's QWidget::contextMenuEvent and QHeaderView's section menu
// are the shape; an app writes none of it.
//
// ONE PER PROCESS. The rows live in the WIDGET (a table builds them from
// its columns), and stay valid while the menu is open because the widget
// does.

struct uui_ctxmenu_src {
    void *w;                               // the widget, passed back to both
    const struct uui_menu_item *items;     // the rows, widget-owned
    int count;
    unsigned (*flags)(void *w, int code);  // UUI_MI_CHECKED / _DISABLED, or 0
    // Runs the row. 1 = the widget changed: the app hears it as the
    // widget's own event (UUI_REASON_RELEASE, changed), so it can save.
    int (*run)(void *w, int code);
};

// The id the router names when the menu took an event. Never an app's.
#define UUI_CTXMENU_ID 0x7EDE0002

struct uui_item *uui_ctxmenu_item(void);

// Opens `src` at (x, y) inside the window's content (bw x bh).
// `target_id` is the router id of the widget, told after a change.
void uui_ctxmenu_open(const struct uui_ctxmenu_src *src, int target_id,
                      int x, int y, int bw, int bh);
int  uui_ctxmenu_is_open(void);
void uui_ctxmenu_close(void);
// After the router named UUI_CTXMENU_ID: runs the row a release
// committed. 1 when the widget changed, with its id in *out_target_id.
int  uui_ctxmenu_take(int *out_target_id);
// A key while it is open: 0 not taken, 1 taken, 2 taken and the widget
// changed (*out_target_id).
int  uui_ctxmenu_key(int key, int *out_target_id);

#endif
