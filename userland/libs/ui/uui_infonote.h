#ifndef UUI_INFONOTE_H
#define UUI_INFONOTE_H

// uui_infonote -- A LINE THAT EXPLAINS: the info icon in the accent, then
// muted text -- under a choice that needs a sentence ("Colour 2 is the
// panel's second colour"), so it reads as a note and not as a label or
// a control. Windows 11's InfoBar in its quietest form, without the box.
//
// One line, clipped to the width it is given; the text is the caller's
// and may change between frames (a colour's hex).

#include "ui/uui_widget.h"

struct uui_infonote {
    int x, y, w, h;
    const char *text;   // not owned
};

void uui_infonote_init(struct uui_infonote *n, const char *text);

extern const struct uui_widget_ops uui_infonote_ops;

#endif
