#ifndef UITEST_APP_H
#define UITEST_APP_H

// uitest_app -- a WHOLE APP on the host, headless (docs/gui-unittest-
// design.md, stage 2). The app's real main(), uapp_desc and handlers run
// in-process against a stand-in for the compositor's half: this file
// serves the app's real channel (base/uchan.h over the stubs' shm) the
// way toywm does -- CREATE, PRESENT and its buffer release, the widget
// map, the title -- and the test drives it with events by widget NAME.
// Qt's offscreen platform plugin, at the protocol instead of in-process
// calls, so uapp itself is unchanged.
//
// ONE THREAD: the app runs on its own stack until it PARKS (a futex wait
// in uapp's loop), and only then does the test run. A frame read after
// uitest_app_settle() is therefore the settled frame, every time.
//
// What it does not stand in for, yet: popups as surfaces (refused, so a
// dropdown draws in the window, the toolkit's own fallback), dialog
// windows, drag and drop, timers (a tick is uitest_app_tick()), a
// resize the compositor proposes.
//
// A case names the app it builds on its first line, and the runner
// compiles that directory's sources with main renamed:
//
//     // uitest-app: userland/apps/apps/calculator
#include "uitest.h"

int uitest_app_main(int argc, char **argv);   // the app's main, renamed by the runner

// Starts the app and runs it until its first frame is out and it parks.
// 0 when it never showed a window (its main returned first).
int  uitest_app_start(int argc, char **argv);
// Runs the app until it parks with nothing left to do: animations run
// out at uapp's frame rate (the clock moves for them), a frame owed goes
// out. 0 when the app has exited.
int  uitest_app_settle(void);

// The window as the app last presented it.
int      uitest_app_w(void);
int      uitest_app_h(void);
uint32_t uitest_app_pixel(int x, int y);
int      uitest_app_frames(void);        // presents so far -- a frame went out
const char *uitest_app_title(void);
// A widget's rect from the window's widget map (the app's uui_item names,
// as `gui widgets` reads them). 0 and a FAIL when there is none.
int  uitest_app_widget(const char *name, int *x, int *y, int *w, int *h);

// Input, window-relative, each followed by a settle.
void uitest_app_click_at(int x, int y);
void uitest_app_press_at(int x, int y);
void uitest_app_move_to(int x, int y, unsigned buttons);
void uitest_app_release_at(int x, int y);
void uitest_app_rclick_at(int x, int y);           // the secondary button
void uitest_app_click(const char *name);           // its centre
void uitest_app_key(int key, unsigned mods);       // a press and its release
void uitest_app_type(const char *text);            // ASCII, one key each
void uitest_app_tick(int n);                       // n WIN_EV_TIMER ticks

// Asks the window to close (WIN_EV_CLOSE, as Alt+F4) and runs the app to
// the end. The app's exit status, or -1 when it did not exit.
int  uitest_app_close(void);

// Writes the presented frame to $UITEST_KEEP/<case>-<tag>.ppm.
void uitest_app_keep(const char *tag);

#endif
