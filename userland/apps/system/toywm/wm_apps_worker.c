// The desktop-entry reload, as a job on the process's worker thread
// (base/uworker.h; wm_apps_worker.h). The worker owns g_job's set and
// icons from submit to take, the main loop from the take on.
#include <string.h>
#include "wm_apps_worker.h"
#include "rt/sys.h"
#include "base/uworker.h"
#include "media/icon_cache.h"

#define SIZES_MAX 8

static struct {
    struct uworker_job job;    // first: the worker hands this pointer back
    int force;
    int sizes[SIZES_MAX];      // the icon cache's sizes when asked
    int nsizes;
    struct gui_apps_set set;
    struct icon_batch icons;   // every entry's icon at every size, decoded
    int built;                 // set holds a build (0: the directory was unchanged)
    uint32_t ms;
} g_job;

// Worker-only: the fingerprint of the directory as last built.
static uint64_t g_fp;
static int g_fp_known;

// On the worker: the build sleeps on the disk and the volume lock, which
// is the whole point of doing it here -- and so does every icon decode.
static void run_one(struct uworker_job *j) {
    (void)j;
    uint64_t t0 = sys_monotonic_ns();
    // Ask the cheap question first -- one directory listing, no file
    // reads -- before re-reading every entry.
    uint64_t fp = gui_apps_dir_fingerprint();
    g_job.built = 0;
    if (g_job.force || !g_fp_known || fp != g_fp) {
        g_fp = fp;
        g_fp_known = 1;
        g_job.built = gui_apps_build(&g_job.set) == 0;
    }
    for (int i = 0; g_job.built && i < g_job.set.count; i++) {
        const char *icon = g_job.set.entries[i].icon;
        for (int k = 0; icon && icon[0] && k < g_job.nsizes; k++)
            icon_batch_add(&g_job.icons, icon, g_job.sizes[k]);
    }
    icon_batch_decode(&g_job.icons);
    g_job.ms = (uint32_t)((sys_monotonic_ns() - t0) / 1000000u);
}

void wm_apps_reload_request(int force) {
    if (uworker_busy(&g_job.job)) return;
    g_job.force = force;
    g_job.nsizes = icon_cache_sizes(g_job.sizes, SIZES_MAX);
    g_job.job.run = run_one;
    uworker_submit(&g_job.job);
}

int wm_apps_reload_busy(void) { return uworker_busy(&g_job.job); }

int wm_apps_reload_take(struct gui_apps_set *out, struct icon_batch *icons,
                        uint32_t *build_ms) {
    if (!uworker_take(&g_job.job)) return 0;
    int built = g_job.built;
    if (built) {
        *out = g_job.set;
        *icons = g_job.icons;
    } else {
        icon_batch_free(&g_job.icons);
    }
    memset(&g_job.set, 0, sizeof g_job.set);
    memset(&g_job.icons, 0, sizeof g_job.icons);
    if (build_ms) *build_ms = g_job.ms;
    return built;
}
