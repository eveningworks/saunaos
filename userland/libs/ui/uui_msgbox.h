#ifndef UUI_MSGBOX_H
#define UUI_MSGBOX_H

#include "ui/uapp.h"

// uui_msgbox -- A MESSAGE BOX IN A WINDOW OF ITS OWN: a severity badge,
// the message, and a row of buttons on a footer strip, right-aligned.
// Win32's MessageBox, KDE's KMessageBox, GTK's GtkMessageDialog -- and
// what SDL_ShowMessageBox is drawn as. A window rather than ui/uui_dialog.h
// because it must work with no window to cover: a program's start-up
// error, an SDL program before its first window.
//
// MODAL OVER `owner` (NULL: the app's main window; an app with none
// gets an UNOWNED box with a taskbar button of its own). The message is
// a ui/uui_para.h paragraph -- selectable, Ctrl+C copies it (all of it
// with nothing selected, as MessageBox does).
//
// ONE AT A TIME per process: a second open while one is up is refused.

enum {
    UUI_MSGBOX_PLAIN = 0,   // no badge
    UUI_MSGBOX_INFO,
    UUI_MSGBOX_WARNING,
    UUI_MSGBOX_ERROR,
};

// The default: drawn so, and FOCUSED when the box opens, so Enter answers
// with it -- or, after Tab, with whichever button has the focus.
#define UUI_MSGBOX_RETURN 0x1u
// Esc and the window's close answer with this one. A box with none
// answers a close with UUI_MSGBOX_CLOSED.
#define UUI_MSGBOX_ESCAPE 0x2u
#define UUI_MSGBOX_CLOSED (-1)

struct uui_msgbox_button {
    const char *label;
    int code;            // what the box answers when it is chosen
    unsigned flags;      // UUI_MSGBOX_RETURN | UUI_MSGBOX_ESCAPE
};

struct uui_msgbox {
    const char *title;
    const char *message;
    int kind;                                // UUI_MSGBOX_*
    const struct uui_msgbox_button *buttons; // left to right as drawn
    int count;                               // at least 1
    struct uapp_window *owner;               // NULL: the app's main window
};

// The answer: a button's code, or UUI_MSGBOX_CLOSED.
typedef void (*uui_msgbox_fn)(void *ctx, int code);

// Opens it and returns at once; `done` hears the answer once. Everything
// in `m` is copied. 0 when one is already up, a field is out of range,
// or the compositor refused.
int uui_msgbox_open(struct uapp *a, const struct uui_msgbox *m, uui_msgbox_fn done, void *ctx);

// Opens it and WAITS, pumping `a` (uapp_pump()) until it is answered --
// for a program on uapp's step API with its own loop (SDL's driver).
// NEVER FROM INSIDE A uapp CALLBACK: the pump would run within the
// dispatch that called it. 1 with `*code` set, 0 if it could not open or
// the app ended first (the desktop went away).
int uui_msgbox_run(struct uapp *a, const struct uui_msgbox *m, int *code);

int uui_msgbox_is_open(void);

#endif
