#ifndef WM_GEOMETRY_H
#define WM_GEOMETRY_H

#include "wm.h"

// Per-application window geometry, remembered across launches.
//
// KEYED BY THE PROGRAM'S PATH, `app_key` (gui_apps.h's wm_app_key()):
// the spawn path, plus ":<app_id>" for one app of a host running several.
// Not the identity int -- interned per boot in launch order, so
// "identity 3" is a different app tomorrow -- and no longer the declared
// app_id alone, which two programs could both choose. A line under the
// old app_id key is still read, once, so nobody's sizes reset.
//
// SAVED ON CLOSE, not on every move. Geometry changes from five call
// sites -- two drags, maximize, a client's resize ack, and the
// pull-back-on-screen -- and a drag writes new coordinates every frame.
// Saving there would be hundreds of whole-file rewrites per drag;
// close_window() is one chokepoint holding the final answer.

// Applies this window's remembered geometry, if it has any and the app
// allows it. Call once, right after a window is created and its app_id
// is set. Silently does nothing when there is nothing saved -- the
// caller's own default placement then stands. Returns 1 when it applied
// a remembered geometry, 0 when there was none.
int wm_geometry_restore(int idx);

// Fits a window that has NO remembered geometry into the work area (the
// screen minus the taskbar): moved up and left until it fits, and when it
// is larger than the work area, asked to shrink to it -- what Windows and
// KDE do with a window too big for the screen. A size sized in text lines
// outgrows a 720p screen once lines are tall enough. Popups are skipped:
// their client places them.
void wm_geometry_fit(int idx);

// Records this window's geometry. Call from close_window() BEFORE the
// slot is shifted out of the array.
void wm_geometry_save(const struct window *win);

#endif
