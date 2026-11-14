#ifndef WM_APPS_WORKER_H
#define WM_APPS_WORKER_H

// THE APP LIST IS RE-READ OFF THE WM'S THREAD, GIO's g_task_run_in_thread
// shape: a reload lists and parses the desktop-entry directory on a worker
// thread (gui_apps_build()), and the main loop installs the result
// (gui_apps_install()) on a frame where nothing points into the registry.
// A frame used to wait out the volume lock for the whole read.
//
// One request at a time: a caller asks only while wm_apps_reload_busy()
// is 0, and asks again later for a change that came in meanwhile.

#include <stdint.h>
#include "gui_apps.h"

// Ask for a reload. `force`: build even when the directory's fingerprint
// is the one last built (the GPL switch moved, and no file did).
void wm_apps_reload_request(int force);

// 1 from a request until its result has been taken.
int wm_apps_reload_busy(void);

// A finished build into *out: 1, and *out is the caller's to install or
// free; 0 when none is ready or the directory had not changed. `build_ms`
// (may be NULL) is how long the worker took.
int wm_apps_reload_take(struct gui_apps_set *out, uint32_t *build_ms);

#endif
