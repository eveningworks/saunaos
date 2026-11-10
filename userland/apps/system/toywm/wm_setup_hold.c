// The desktop held back while First-boot setup starts (wm_setup_hold.h).
#include "wm_setup_hold.h"
#include "wm_internal.h"
#include "wm_log.h"
#include "rt/sys.h"
#include "base/ufbs.h"
#include "kapi.h"      // k_strcmp
#include "splash_layout.h"
#include "base/usetting.h"
#include "ui/ugfx.h"
#include <stdio.h>
#include <string.h>

#define HOLD_MAX_NS 20000000000ull   // the longest a setup that never draws can keep the screen held
#define SPIN_DOTS 8
#define SPIN_STEP_NS 100000000ull    // one dot of the spinner's turn

static int g_hold;      // HOLD_*
static uint64_t g_until;
static int g_phase = -1;
static int g_drawn;     // a held frame has been drawn

enum { HOLD_NONE, HOLD_SETUP, HOLD_DESKTOP, HOLD_SHUTDOWN };

// THE SHUTDOWN SCREEN, while init stops what started after the desktop:
// init's lines, read again as the spinner turns, until init stops this
// desktop too and the kernel draws the same screen without the spinner
// (kernel/drivers/splash.c).
static struct shutdown_status g_down;

void wm_setup_hold_shutdown(int reboot) {
    g_hold = HOLD_SHUTDOWN;
    if (sys_shutdown_status(SHUTDOWN_OP_GET, &g_down) < 0 || !g_down.line[0]) {
        memset(&g_down, 0, sizeof g_down);
        snprintf(g_down.line, sizeof g_down.line, "%s", reboot ? "Restarting..." : "Shutting down...");
    }
    wm_logf("wm: shutting down -- the shutdown screen until init stops the desktop\n");
    wm_damage_rect(0, 0, screen_w, screen_h);
    WM_REDRAW();
}

void wm_setup_hold_begin(void) {
    if (ufbs_needed()) {
        g_hold = HOLD_SETUP;
        g_until = sys_monotonic_ns() + HOLD_MAX_NS;
        wm_logf("wm: holding the desktop back until First-boot setup is up\n");
        return;
    }
    // A MORE VERBOSE BOOT says the last stage too (system.boot_messages,
    // kernel/drivers/splash.c): the splash stays for one frame of
    // "Loading desktop...", and the desktop's own first frame -- the
    // slow one, decoding the wallpaper -- replaces it.
    char v[16] = "";
    if (usetting_get("system.boot_messages", v, sizeof v) && !k_strcmp(v, "detailed")) {
        g_hold = HOLD_DESKTOP;
        g_until = sys_monotonic_ns() + HOLD_MAX_NS;
    }
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

static int phase_now(void) { return (int)((sys_monotonic_ns() / SPIN_STEP_NS) % SPIN_DOTS); }

void wm_setup_hold_poll(void) {
    if (!g_hold) return;
    if (g_hold == HOLD_SHUTDOWN) {
        if (phase_now() == g_phase) return;
        struct shutdown_status st;
        if (sys_shutdown_status(SHUTDOWN_OP_GET, &st) == 0 && st.line[0]) g_down = st;
        wm_damage_rect(0, 0, screen_w, screen_h);   // the spinner and both lines
        WM_REDRAW();
        return;
    }
    if (g_hold == HOLD_DESKTOP) {
        if (!g_drawn) return;
        g_hold = HOLD_NONE;
        wm_damage_rect(0, 0, screen_w, screen_h);
        WM_REDRAW();
        return;
    }
    int up = setup_on_screen();
    if (!up && sys_monotonic_ns() < g_until) {
        // The spinner turns: repaint its box when a dot moves on.
        if (phase_now() != g_phase) {
            struct splash_layout l;
            splash_layout(screen_w, screen_h, &l);
            int e = l.spin_r + l.dot_r + 2;
            wm_damage_rect(l.spin_cx - e, l.spin_cy - e, 2 * e, 2 * e);
            WM_REDRAW();
        }
        return;
    }
    g_hold = 0;
    wm_logf("wm: the desktop is let go -- %s\n", up ? "First-boot setup is up" : "setup never drew, timed out");
    wm_damage_rect(0, 0, screen_w, screen_h);
    WM_REDRAW();
}

int wm_setup_hold_active(void) { return g_hold; }

static uint32_t rgb_of(uint32_t v) {
    return ugfx_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
}

static void span_rect(struct ugfx_surface *s, const struct splash_rect *r, uint32_t c) {
    for (int row = 0; row < r->h; row++) {
        int in = splash_round_inset(r, row);
        ugfx_fill_rect(s, r->x + in, r->y + row, r->w - 2 * in, 1, c);
    }
}

static void span_stone(struct ugfx_surface *s, const struct splash_stone *st, uint32_t c) {
    for (int y = st->cy - st->a - st->b; y <= st->cy + st->a + st->b; y++) {
        int x0, x1;
        if (splash_stone_span(st, y, &x0, &x1)) ugfx_fill_rect(s, x0, y, x1 - x0 + 1, 1, c);
    }
}

void wm_setup_hold_draw(struct ugfx_surface *s) {
    struct splash_layout l;
    splash_layout(screen_w, screen_h, &l);
    uint32_t bg = rgb_of(SPLASH_BG);
    ugfx_fill_rect(s, 0, 0, screen_w, screen_h, bg);
    // The kernel's logo, to the pixel (api/splash_layout.h): the handover
    // from its splash to this one must not move anything.
    span_rect(s, &l.plate, rgb_of(SPLASH_PLATE));
    for (int i = 0; i < SPLASH_STONES; i++) span_stone(s, &l.stone[i], rgb_of(SPLASH_STONE));

    // Eight dots round a ring, the lead one brightest and the tail
    // fading, a dot further on every step -- Windows' boot spinner.
    // The eighths without a sine table: cos 45 is about 181/256.
    static const int DX[SPIN_DOTS] = { 0, 181, 256, 181, 0, -181, -256, -181 };
    static const int DY[SPIN_DOTS] = { -256, -181, 0, 181, 256, 181, 0, -181 };
    g_phase = phase_now();
    g_drawn = 1;
    for (int i = 0; i < SPIN_DOTS; i++) {
        int k = (g_phase - i + SPIN_DOTS) % SPIN_DOTS;   // 0 = the lead dot
        uint8_t a = (uint8_t)(255 - k * 26);
        int x = l.spin_cx + DX[i] * l.spin_r / 256, y = l.spin_cy + DY[i] * l.spin_r / 256;
        ugfx_fill_circle(s, x, y, l.dot_r, ugfx_blend(bg, ugfx_rgb(255, 255, 255), a));
    }
    // The line under it, in the fixed-cell face: the kernel's own
    // splash said "Starting SaunaOS..." in its baked monospace font, and
    // the line changes here rather than its typeface.
    const char *label = g_hold == HOLD_SHUTDOWN ? g_down.line
                      : g_hold == HOLD_SETUP ? "Loading setup..." : "Loading desktop...";
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    ugfx_draw_string(s, (screen_w - ugfx_text_width(label)) / 2, l.label_y, label,
                     rgb_of(SPLASH_TEXT), bg);
    if (g_hold == HOLD_SHUTDOWN && g_down.detail[0])
        ugfx_draw_string(s, (screen_w - ugfx_text_width(g_down.detail)) / 2, l.detail_y,
                         g_down.detail, rgb_of(SPLASH_DIM), bg);
    ugfx_set_font(was);
}
