#ifndef UUI_PARA_H
#define UUI_PARA_H

#include <stdint.h>
#include "ui/uui_edit.h"
#include "ui/uui_widget.h"

// uui_para -- A PARAGRAPH A PERSON CAN SELECT AND COPY: read-only text,
// word-wrapped to its width, with drag, double and triple click, Ctrl+A,
// Ctrl+C and the shared right-click menu (Copy, Select All) -- the
// selectable static text of a GTK message dialog (GtkLabel:selectable).
// A label (ui/uui_label.h) is for a word or two in a control; this is
// for a sentence a person might want to quote: a message box's message,
// an error's details.
//
// THE TEXT IS COPIED, so a caller may pass a buffer it reuses. Its
// natural size is the text wrapped at `wrap_chars` average characters
// (its widest line, so a short message makes a narrow box); given more
// width it wraps there instead, given less it overflows, as a layout's
// children do. A '\n' always breaks.
//
// IT TAKES NO FOCUS: the window's buttons keep the keyboard, and the
// window hands its Ctrl+A and Ctrl+C here with uui_para_key() -- Win32's
// MessageBox copies on Ctrl+C with the focus on a button.

struct uui_para {
    char *text;            // owned; never NULL after a successful init
    int len;
    int wrap_chars;        // the natural wrap width in 'n' advances; 0 = 60
    uint32_t bg;           // what it is drawn on: a colour or UUI_ROLE(); UNSET = panel_bg
    int x, y, w, h;
    struct uui_edit ed;    // UUI_EDIT_READONLY: the selection, never a change
    int dragging;
};

// 1, or 0 when the copy could not be allocated (the paragraph is then empty).
int  uui_para_init(struct uui_para *p, const char *text);
void uui_para_free(struct uui_para *p);

// A key the window did not use: Ctrl+A selects all, Ctrl+C copies the
// selection -- or, with none, the whole text. 1 when it took the key.
int uui_para_key(struct uui_para *p, int key, unsigned mods);

extern const struct uui_widget_ops uui_para_ops;

#endif
