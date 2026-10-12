// The desktop held back while First-boot setup starts (wm_setup_hold.h).
#include "wm_setup_hold.h"
#include "wm_internal.h"
#include "wm_log.h"
#include "rt/sys.h"
#include "lib/ufbs.h"
#include "kapi.h"      // k_strcmp

#define HOLD_MAX_NS 20000000000ull   // the longest a setup that never draws can keep the screen grey

static int g_hold;
static uint64_t g_until;

void wm_setup_hold_begin(void) {
    if (!ufbs_needed()) return;
    g_hold = 1;
    g_until = sys_monotonic_ns() + HOLD_MAX_NS;
    wm_logf("wm: holding the desktop back until First-boot setup is up\n");
}

// Setup's window, drawn at the screen's size: its first full-screen frame.
static int setup_on_screen(void) {
    for (int i = 0; i < window_count; i++) {
        const struct window *w = &windows[i];
        if (k_strcmp(w->app_id, "firstboot") == 0 && w->fullscreen &&
            w->client_w >= screen_w && w->client_h >= screen_h)
            return 1;
    }
    return 0;
}

void wm_setup_hold_poll(void) {
    if (!g_hold) return;
    int up = setup_on_screen();
    if (!up && sys_monotonic_ns() < g_until) return;
    g_hold = 0;
    wm_logf("wm: the desktop is let go -- %s\n", up ? "First-boot setup is up" : "setup never drew, timed out");
    wm_damage_rect(0, 0, screen_w, screen_h);
    redraw_pending = 1;
}

int wm_setup_hold_active(void) { return g_hold; }
