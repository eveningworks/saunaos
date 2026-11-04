#ifndef BROWSER_NS_SCHED_H
#define BROWSER_NS_SCHED_H

// NetSurf's one-shot timers (gui_misc_table.schedule), run from the
// app's own loop: ns_sched_run() fires what is due and says how long
// until the next, which is what the app arms its tick with.
#include "utils/errors.h"

nserror ns_schedule(int ms, void (*cb)(void *p), void *p);
// Runs every due callback; the milliseconds until the next one, or -1
// when none is pending.
int ns_sched_run(void);

#endif
