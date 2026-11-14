// The desktop-entry reload's worker thread (wm_apps_worker.h).
//
// State under g_lock, a three-step handshake: IDLE -> ASKED (the main
// loop) -> DONE (the worker) -> IDLE (the main loop's take). The worker
// owns g_set from ASKED to DONE and the main loop from DONE on, so the set
// is never touched by both.
#include <pthread.h>
#include <string.h>
#include "wm_apps_worker.h"
#include "rt/sys.h"
#include "apps/system/toywm/wm_log.h"

enum { IDLE, ASKED, DONE };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static int g_state = IDLE;
static int g_force;
static int g_started;     // 1 running, -1 could not start (reloads run inline)
static struct gui_apps_set g_set;
static int g_built;       // g_set holds a build (0: the directory was unchanged)
static uint32_t g_ms;

// Worker-only: the fingerprint of the directory as last built.
static uint64_t g_fp;
static int g_fp_known;

// Called with no lock held: the build sleeps on the disk and the volume
// lock, which is the whole point of doing it here.
static void run_one(int force) {
    uint64_t t0 = sys_monotonic_ns();
    // Ask the cheap question first -- one directory listing, no file
    // reads -- before re-reading every entry.
    uint64_t fp = gui_apps_dir_fingerprint();
    int built = 0;
    if (force || !g_fp_known || fp != g_fp) {
        g_fp = fp;
        g_fp_known = 1;
        built = gui_apps_build(&g_set) == 0;
    }
    pthread_mutex_lock(&g_lock);
    g_built = built;
    g_ms = (uint32_t)((sys_monotonic_ns() - t0) / 1000000u);
    g_state = DONE;
    pthread_mutex_unlock(&g_lock);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (g_state != ASKED) pthread_cond_wait(&g_cond, &g_lock);
        int force = g_force;
        pthread_mutex_unlock(&g_lock);
        run_one(force);
    }
    return 0;
}

void wm_apps_reload_request(int force) {
    if (!g_started) {
        pthread_t t;
        g_started = pthread_create(&t, 0, worker, 0) == 0 ? 1 : -1;
        if (g_started < 0) wm_logf("wm: no reload worker -- the app list reloads on the WM's thread\n");
    }
    pthread_mutex_lock(&g_lock);
    if (g_state != IDLE) { pthread_mutex_unlock(&g_lock); return; }
    g_state = ASKED;
    g_force = force;
    if (g_started > 0) pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    if (g_started < 0) run_one(force);
}

int wm_apps_reload_busy(void) {
    pthread_mutex_lock(&g_lock);
    int busy = g_state != IDLE;
    pthread_mutex_unlock(&g_lock);
    return busy;
}

int wm_apps_reload_take(struct gui_apps_set *out, uint32_t *build_ms) {
    pthread_mutex_lock(&g_lock);
    if (g_state != DONE) { pthread_mutex_unlock(&g_lock); return 0; }
    int built = g_built;
    if (built) {
        *out = g_set;
        memset(&g_set, 0, sizeof g_set);
    }
    if (build_ms) *build_ms = g_ms;
    g_state = IDLE;
    pthread_mutex_unlock(&g_lock);
    return built;
}
