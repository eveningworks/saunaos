// Pushed configuration for the compositor -- see wm_watch.h.
#include "apps/system/toywm/wm_watch.h"
#include "apps/system/toywm/wm_log.h"
#include "apps/system/toywm/wm_conf.h"   // wm_setting_generation(), defined here
#include "wm_internal.h"   // wm_render_reset()
#include <string.h>
#include "rt/sys.h"
#include "base/udclick.h"
#include "ui/utheme.h"
#include "base/udirs.h"
#include "base/uconf.h"
#include "base/uworker.h"
#include "setting_abi.h"   // SETTING_OP_COUNT

static const char *PATH[WM_TOPIC_COUNT] = {
    [WM_TOPIC_SETTINGS] = 0,                    // an event, not a path
    [WM_TOPIC_ETC]      = "/etc",
    [WM_TOPIC_APPS]     = "/usr/wm/applications",
    [WM_TOPIC_DESKTOP]  = 0,                    // the known folder, set below
    [WM_TOPIC_EFFECTS]  = "/etc/effects",
    [WM_TOPIC_TRASH]    = "/home/.Trash/files",
    [WM_TOPIC_THUMBS]   = "/var/cache/thumbnails",
};

static int g_id[WM_TOPIC_COUNT];         // the kernel's watch id, > 0
static uint64_t g_gen[WM_TOPIC_COUNT];   // moved by the events, at once

// WHAT THE POLLERS SEE LAGS THE EVENTS BY ONE REFRESH. A change moves
// g_gen at once; the worker then re-reads every config file the cache
// holds (uconf_cache_refresh()), and only then are the sampled counters
// published -- so the pollers' re-reads, all on this thread, are answered
// from memory instead of each waiting out the disk and the volume lock.
// dconf's shape: a change costs the reader nothing but a memory read.
static uint64_t g_pub[WM_TOPIC_COUNT], g_want[WM_TOPIC_COUNT];
static uint32_t g_kgen_pub, g_kgen_want;   // the settings registry's own generation
static struct uworker_job g_refresh;
static int g_refreshed;                    // files the last refresh read
static unsigned long g_refreshes;
static int g_primed;

static uint64_t raw_gen(enum wm_topic t) {
    if (PATH[t] && g_id[t] <= 0) return sys_fs_generation();   // the fallback
    return g_gen[t];
}

static uint32_t raw_kgen(void) {
    struct setting_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.op = SETTING_OP_COUNT;   // any op fills `generation`; COUNT has no arguments
    return sys_setting(&msg) == 0 ? msg.generation : 0;
}

static void refresh_run(struct uworker_job *j) {
    (void)j;
    // uconf only: usetting_schema's own state is unlocked, this thread's.
    g_refreshed = uconf_cache_refresh();
}

void wm_watch_init(void) {
    uconf_count_thread();   // `gui frameio` counts this thread's reads: the frame's
    // The personal folders before anything lists them (base/udirs.h):
    // the old lower-case layout moved across once, then any missing made.
    int moved = udir_migrate();
    if (moved) wm_logf("wm: moved %d item(s) to the personal folders\n", moved);
    udir_create_all();
    PATH[WM_TOPIC_DESKTOP] = udir_path(UDIR_DESKTOP);
    // The bin and the thumbnail cache are made on first use, and a watch
    // needs a folder to watch: one refused falls back to EVERY write.
    sys_mkdir("/home/.Trash");
    sys_mkdir("/home/.Trash/files");
    sys_mkdir("/home/.Trash/info");
    sys_mkdir("/var/cache");
    sys_mkdir("/var/cache/thumbnails");
    for (int t = 0; t < WM_TOPIC_COUNT; t++) {
        g_gen[t] = 1;
        if (!PATH[t]) continue;
        g_id[t] = sys_fs_watch(PATH[t]);
        if (g_id[t] <= 0)
            wm_logf("wm: no watch on %s (%d) -- polling it instead\n", PATH[t], g_id[t]);
    }
}

void wm_watch_fired(int id) {
    // SEVERAL TOPICS CAN SHARE AN ID: the kernel hands back the same id
    // for the same path, and matches by hash, so bump every topic on it.
    // WIN_FSWATCH_OVERFLOW: more fired than the kernel's queue named --
    // every watched topic re-reads.
    for (int t = 0; t < WM_TOPIC_COUNT; t++)
        if (g_id[t] > 0 && (g_id[t] == id || id == WIN_FSWATCH_OVERFLOW)) g_gen[t]++;
}

void wm_watch_setting(void) { g_gen[WM_TOPIC_SETTINGS]++; }

// On the publish of a settings change, with the cache refreshed.
static void settings_published(void) {
    udclick_reload();   // the compositor's own clicks: title bars, desktop icons
    // Its corners and scrollbars, and the theme (ui/utheme.h). A theme
    // that changed repaints everything: every frame and the taskbar are
    // drawn from it, and a damage-limited frame would leave the rest in
    // the old colours -- the font change's reason for wm_render_reset().
    struct utheme before = *utheme_current();
    utheme_settings_reload();
    if (memcmp(&before, utheme_current(), sizeof before)) wm_render_reset();
}

uint64_t wm_watch_gen(enum wm_topic t) {
    if (t < 0 || t >= WM_TOPIC_COUNT) return 0;
    if (!g_primed) return raw_gen(t);
    return g_pub[t];
}

uint32_t wm_setting_generation(void) { return g_primed ? g_kgen_pub : raw_kgen(); }

void wm_watch_poll(void) {
    if (!g_primed) {
        g_primed = 1;
        for (int t = 0; t < WM_TOPIC_COUNT; t++) g_pub[t] = raw_gen(t);
        g_kgen_pub = raw_kgen();
        return;
    }
    if (uworker_take(&g_refresh)) {
        int settings = g_want[WM_TOPIC_SETTINGS] != g_pub[WM_TOPIC_SETTINGS];
        for (int t = 0; t < WM_TOPIC_COUNT; t++) g_pub[t] = g_want[t];
        g_kgen_pub = g_kgen_want;
        g_refreshes++;
        if (settings) settings_published();
    }
    if (uworker_busy(&g_refresh)) return;
    int moved = 0;
    for (int t = 0; t < WM_TOPIC_COUNT; t++) moved |= (g_want[t] = raw_gen(t)) != g_pub[t];
    g_kgen_want = raw_kgen();
    moved |= g_kgen_want != g_kgen_pub;
    if (!moved) return;
    g_refresh.run = refresh_run;
    uworker_submit(&g_refresh);
}

void wm_watch_refresh_stats(unsigned long *refreshes, int *last_read) {
    *refreshes = g_refreshes;
    *last_read = g_refreshed;
}
