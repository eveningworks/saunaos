// See ui/uui_caret.h.
#include "ui/uui_caret.h"
#include "base/usetting.h"
#include "rt/sys.h"

#define LAST_PHASE (UUI_CARET_TIMEOUT_MS / UUI_CARET_HALF_MS) // even: ends ON

static unsigned long long g_t0_ns;  // the last input; 0 = none yet
static int g_blink = -1;            // desktop.caret_blink; -1 = not yet asked
static int g_drawn = -1;            // the phase a caret was last drawn in
static int g_asked = LAST_PHASE;    // the phase a repaint was last asked for
                                    // (the input's own, from a reset)

static int read_setting(void) {
    char v[8];
    // ON when the registry has nothing to say -- the declared default.
    if (!usetting_get("desktop.caret_blink", v, sizeof v)) return 1;
    return !(v[0] == 'o' && v[1] == 'f' && v[2] == 'f');
}

// Which half-period we are in: even = on. LAST_PHASE once stopped.
static int phase(void) {
    if (g_blink < 0) g_blink = read_setting();
    if (!g_blink || !g_t0_ns) return LAST_PHASE;
    unsigned long long ms = (sys_monotonic_ns() - g_t0_ns) / 1000000ULL;
    unsigned long long p = ms / UUI_CARET_HALF_MS;
    return p >= LAST_PHASE ? LAST_PHASE : (int)p;
}

int uui_caret_visible(void) {
    int p = phase();
    g_drawn = p;
    return (p & 1) == 0;
}

static unsigned long long g_read_ns;   // when the setting was last asked

void uui_caret_reset(void) {
    g_t0_ns = sys_monotonic_ns();
    g_drawn = -1;
    // The registry, at most every two seconds: this runs on EVERY key and
    // press in every app, caret or not, and a settings round trip per
    // key-repeat is input latency for nothing. A change in System
    // Settings still applies within one typed word.
    if (g_blink < 0 || g_t0_ns - g_read_ns > 2000000000ULL) {
        g_blink = read_setting();
        g_read_ns = g_t0_ns;
    }
    // The input's own repaint draws this phase; the first ask is the next.
    g_asked = phase();
}

int uui_caret_wait_ms(void) {
    int p = phase();
    if (g_drawn < 0) return -1;
    if (p != g_asked) {
        // A NEW PHASE: ask for its repaint ONCE, whoever has drawn since.
        // A frame painted for other damage (the taskbar clock's, every
        // second) runs the caret's draw with the clip elsewhere, so a
        // visible() call is not the caret's pixels changing -- taking it
        // for one held the Start menu's caret on, locked to the clock,
        // for a whole blink. If the LAST ask drew no caret (its field
        // lost focus) nothing is blinking: asking again would spin.
        if (g_drawn < g_asked) return -1;
        g_asked = p;
        return 0;
    }
    if (p >= LAST_PHASE) return -1;
    unsigned long long ms = (sys_monotonic_ns() - g_t0_ns) / 1000000ULL;
    long long left = (long long)(p + 1) * UUI_CARET_HALF_MS - (long long)ms;
    return left > 0 ? (int)left : 0;
}

static const struct ugfx_surface *g_at_s;   // the surface the last caret was placed on
static int g_at[4];

void uui_caret_at(const struct ugfx_surface *s, int x, int y, int w, int h) {
    g_at_s = s;
    g_at[0] = x; g_at[1] = y; g_at[2] = w; g_at[3] = h;
}

void uui_caret_frame_begin(void) { g_at_s = 0; }

int uui_caret_frame_end(const struct ugfx_surface *s, int *x, int *y, int *w, int *h) {
    if (!g_at_s || g_at_s != s) return 0;
    *x = g_at[0]; *y = g_at[1]; *w = g_at[2]; *h = g_at[3];
    return 1;
}
