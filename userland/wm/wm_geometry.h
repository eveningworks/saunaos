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
// SAVED WHEN A CHANGE ENDS, not on every move: at the end of a drag or
// a resize, on a snap or a maximize, and at close -- never per frame of
// a drag, which would be hundreds of whole-file rewrites. A crash or a
// killed client keeps the last placement. An unchanged line is not
// rewritten. A window closed maximized or snapped saves its restore
// rect plus that state, and reopens in it.
//
// `desktop.windows.remember` off stops both reading and writing; the
// file is kept, so turning it back on brings the sizes back.

// Applies this window's remembered geometry, if it has any and the app
// allows it. Call once, right after a window is created and its app_id
// is set. Silently does nothing when there is nothing saved -- the
// caller's own default placement then stands. Returns 1 when it applied
// a remembered geometry, 0 when there was none.
int wm_geometry_restore(int idx);

// Applies a restored maximized or snapped state once the window can take
// it -- at once for a kernel-space app, after its hints for a client.
void wm_geometry_apply_state(int idx);

// Fits a window that has NO remembered geometry into the work area (the
// screen minus the taskbar): moved up and left until it fits, and when it
// is larger than the work area, asked to shrink to it -- what Windows and
// KDE do with a window too big for the screen. A size sized in text lines
// outgrows a 720p screen once lines are tall enough. Popups are skipped:
// their client places them.
void wm_geometry_fit(int idx);

// Records this window's geometry. From close_window() BEFORE the slot
// is shifted out of the array, and wherever a move, resize or state
// change ends.
void wm_geometry_save(const struct window *win);
// The same, at a size the window is about to have -- a resize drag's
// last proposal, which a client has not answered yet.
void wm_geometry_save_size(const struct window *win, int w, int h);

// Whether this window's app has a remembered placement, and dropping it
// -- the window menu's "Forget saved position", KWin's rule removed.
int  wm_geometry_saved(const struct window *win);
void wm_geometry_forget(struct window *win);   // and this window saves no more

#endif
