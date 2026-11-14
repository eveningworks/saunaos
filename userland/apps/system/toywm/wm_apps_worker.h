#ifndef WM_APPS_WORKER_H
#define WM_APPS_WORKER_H

// THE APP LIST IS RE-READ OFF THE WM'S THREAD, GIO's g_task_run_in_thread
// shape: a reload lists and parses the desktop-entry directory, and
// decodes its icons, on the worker thread (base/uworker.h, gui_apps_build()), and the main loop installs the result
// (gui_apps_install()) on a frame where nothing points into the registry.
// A frame used to wait out the volume lock for the whole read.
//
// One request at a time: a caller asks only while wm_apps_reload_busy()
// is 0, and asks again later for a change that came in meanwhile.

#include <stdint.h>
#include "gui_apps.h"
#include "media/icon_cache.h"

// Ask for a reload. `force`: build even when the directory's fingerprint
// is the one last built (the GPL switch moved, and no file did).
void wm_apps_reload_request(int force);

// 1 from a request until its result has been taken.
int wm_apps_reload_busy(void);

// A finished build into *out, with its entries' icons decoded at the
// sizes the icon cache held when it was asked: 1, and both are the
// caller's to install (icon_cache_install() BEFORE the set, so no frame
// draws a new entry before its icon) or free; 0 when none is ready or
// the directory had not changed. `build_ms` (may be NULL) is how long
// the worker took.
int wm_apps_reload_take(struct gui_apps_set *out, struct icon_batch *icons,
                        uint32_t *build_ms);

#endif
