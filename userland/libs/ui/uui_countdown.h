#ifndef UUI_COUNTDOWN_H
#define UUI_COUNTDOWN_H

#include "ui/uapp.h"

// uui_countdown -- A QUESTION THAT ANSWERS ITSELF: a bold line, a
// sentence, a bar running down beside "Restarting in 9 seconds", and two
// buttons; when the time runs out the timeout's answer is taken.
// Windows' "Keep these display settings?" and GNOME's "Keep Changes?",
// where the safe answer must happen even when nobody can see the screen.
//
// `seconds` 0 is a plain message: no bar, no clock, and only the
// primary button when `other_label` is NULL.
//
// Hosted as an app's MAIN WINDOW (uui_runask's arrangement, for a program
// that is only the question); `done` runs once, after which the app
// should quit.

struct uui_countdown_spec {
    const char *window_title;
    const char *title;           // the bold first line
    const char *text;            // what happens, wrapped
    const char *remaining;       // a printf format with one %d: "Restarting in %d seconds"
    int seconds;
    const char *primary_label;   // the accent button, Enter's answer
    int primary_code;
    const char *other_label;     // NULL for none
    int other_code;
    int timeout_code;            // the answer when time runs out
    int close_code;              // Esc, Alt+F4, the window's X
};

typedef void (*uui_countdown_fn)(void *ctx, int code);

// Fills `d` (title, size, widgets, layout, focus and input hooks); the
// spec's strings must outlive the app.
void uui_countdown_app_desc(struct uapp_desc *d, const struct uui_countdown_spec *s,
                            uui_countdown_fn done, void *ctx);

#endif
