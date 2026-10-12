#ifndef UUI_NAMEASK_H
#define UUI_NAMEASK_H

// ASK FOR A NAME: a small modal window -- a caption, a text field with a
// suggestion selected, a line for a complaint, Cancel and an OK button
// -- for "Save as new theme", "Rename" and their kind. The caller checks
// the name (NULL = fine, else the sentence to show) and is told the
// answer once; Cancel and the window's X tell it nothing.

#include "ui/uapp.h"
#include "ui/uui_label.h"
#include "ui/uui_textbox.h"
#include "ui/uui_button.h"
#include "ui/uui_layout.h"
#include "ui/uui_focus.h"

struct uui_nameask {
    struct uapp_window *win;
    struct uui_label caption, err, gap;
    struct uui_textbox field;
    struct uui_button ok, cancel;
    struct uui_item items[4], btns[3];
    struct uui_layout layout, btn_row;
    struct uui_focusable focus_items[3];
    struct uui_focus focus;
    const char *(*check)(void *ctx, const char *name);
    void (*done)(void *ctx, const char *name);
    void *ctx;
};

// `q` is the caller's and must outlive the window (file scope, as every
// widget here is).
void uui_nameask_open(struct uui_nameask *q, struct uapp *a, const char *title,
                      const char *caption, const char *initial, const char *ok_label,
                      const char *(*check)(void *ctx, const char *name),
                      void (*done)(void *ctx, const char *name), void *ctx);
int  uui_nameask_is_open(const struct uui_nameask *q);

#endif
