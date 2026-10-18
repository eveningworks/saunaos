// Per-stage frame cost; see wm_prof.h.
#include "wm_prof.h"
#include <stdint.h>

static const char *const NAME[WMS_COUNT] = {
    "other", "desktop", "shadow", "glass", "chrome", "client",
    "taskbar", "overlay", "cursor", "present",
};

static int g_cur = WMS_OTHER;
static unsigned long long g_mark, g_frame[WMS_COUNT];
static unsigned long long g_sum[2][WMS_COUNT];
static unsigned g_n[2];

static unsigned long long now(void) { return __builtin_ia32_rdtsc(); }

int wm_stage(int s) {
    unsigned long long t = now();
    g_frame[g_cur] += t - g_mark;
    g_mark = t;
    int was = g_cur;
    g_cur = s >= 0 && s < WMS_COUNT ? s : WMS_OTHER;
    return was;
}

void wm_stage_frame_begin(void) {
    for (int i = 0; i < WMS_COUNT; i++) g_frame[i] = 0;
    g_cur = WMS_OTHER;
    g_mark = now();
}

void wm_stage_frame_end(int full) {
    wm_stage(WMS_OTHER);   // charge the tail
    int b = full ? 1 : 0;
    for (int i = 0; i < WMS_COUNT; i++) g_sum[b][i] += g_frame[i];
    g_n[b]++;
}

unsigned wm_stage_stats(int full, unsigned long long cyc[WMS_COUNT]) {
    int b = full ? 1 : 0;
    for (int i = 0; i < WMS_COUNT; i++) cyc[i] = g_n[b] ? g_sum[b][i] / g_n[b] : 0;
    return g_n[b];
}

const char *wm_stage_name(int s) { return s >= 0 && s < WMS_COUNT ? NAME[s] : "?"; }

void wm_stage_reset(void) {
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < WMS_COUNT; i++) g_sum[b][i] = 0;
        g_n[b] = 0;
    }
}
