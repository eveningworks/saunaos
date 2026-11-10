// driver-none: the boot splash, drawn over whatever display.c chose
//
// The boot splash -- see kernel/splash.h.
#include "splash.h"
#include "splash_layout.h"
#include "gfx.h"
#include "klog.h"
#include "timer.h"
#include "string.h"
#include "multiboot.h"
#include "input_keys.h"
#include "setting.h"
#include "etc_config.h"

// The longest the logo may stand with nothing claiming the screen: a
// desktop that crashes before drawing gets the log back after this.
#define SPLASH_MAX_TICKS (30 * COARSE_HZ)

#define MESSAGES_FILE "/etc/saunaos.conf"
#define MESSAGES_KEY  "boot_messages"

static volatile int g_active;
static volatile int g_log_wanted;
static uint64_t g_until;
static struct splash_layout g_l;
static int g_text_w;   // the line on screen now, so the next one can clear it
static int g_detail_w; // ...and the shutdown screen's second line

// THE SHUTDOWN SCREEN: init's lines, kept from the moment it starts
// stopping things, drawn only once the desktop has let the screen go.
static int g_shutdown;          // SHUTDOWN_F_* while one is under way
static char g_line[SHUTDOWN_LINE_MAX], g_detail[SHUTDOWN_LINE_MAX];

static uint32_t pack(uint32_t rgb) {
    return gfx_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

static void round_rect(const struct splash_rect *r, uint32_t color) {
    for (int row = 0; row < r->h; row++) {
        int in = splash_round_inset(r, row);
        gfx_front_fill_rect(r->x + in, r->y + row, r->w - 2 * in, 1, color);
    }
}

static void stone(const struct splash_stone *st, uint32_t color) {
    for (int y = st->cy - st->a - st->b; y <= st->cy + st->a + st->b; y++) {
        int x0, x1;
        if (splash_stone_span(st, y, &x0, &x1)) gfx_front_fill_rect(x0, y, x1 - x0 + 1, 1, color);
    }
}

// One centred line at `y`, clearing the one it replaces (`*prev_w`).
static void say_at(int y, const char *text, uint32_t rgb, int *prev_w) {
    int w = gfx_text_width(text), sw = gfx_width();
    if (*prev_w) gfx_front_fill_rect((sw - *prev_w) / 2, y, *prev_w, gfx_char_h(), pack(SPLASH_BG));
    gfx_front_draw_string((sw - w) / 2, y, text, pack(rgb), pack(SPLASH_BG));
    *prev_w = w;
}

static void say(const char *text) { say_at(g_l.label_y, text, SPLASH_TEXT, &g_text_w); }

// The words that turn the splash off, read for the boot and the shutdown alike.
static int splash_allowed(void) {
    char v[16];
    if (multiboot_cmdline_value("splash=", v, sizeof v) && !k_strcmp(v, "off")) return 0;
    if (multiboot_cmdline_value("target=", v, sizeof v) && k_strcmp(v, "graphical")) return 0;
    return gfx_double_buffered() && gfx_width() > 0;   // no page behind it to keep
}

static void draw_logo(void) {
    splash_layout(gfx_width(), gfx_height(), &g_l);
    gfx_front_fill_rect(0, 0, gfx_width(), gfx_height(), pack(SPLASH_BG));
    round_rect(&g_l.plate, pack(SPLASH_PLATE));
    for (int i = 0; i < SPLASH_STONES; i++) stone(&g_l.stone[i], pack(SPLASH_STONE));
    g_text_w = g_detail_w = 0;
}

void splash_begin(void) {
    if (!splash_allowed()) return;
    draw_logo();
    say("Starting SaunaOS...");
    g_until = coarse_ticks() + SPLASH_MAX_TICKS;
    g_active = 1;
    klog_write("splash: on -- Esc shows the boot log\n");
}

int splash_active(void) { return g_active; }

// `detailed` names each stage; `simple`, the default, says "Starting
// SaunaOS..." until the desktop. Read when init starts, the first moment
// /etc is there to read.
void splash_stage_services(void) {
    char v[16];
    if (!g_active) return;
    if (etc_config_get(MESSAGES_FILE, MESSAGES_KEY, v, sizeof v) && !k_strcmp(v, "detailed"))
        say("Starting services...");
}

// --- the shutdown screen (SYS_SHUTDOWN_STATUS) ------------------------
//
// No spinner: the kernel draws once per change, never from a timer (the
// top of splash.h), so a turning spinner is the desktop's alone and a
// frozen one would read as a hang. The lines changing are the progress.

static void shutdown_draw_lines(void) {
    say_at(g_l.label_y, g_line, SPLASH_TEXT, &g_text_w);
    say_at(g_l.detail_y, g_detail, SPLASH_DIM, &g_detail_w);
}

// The screen is the kernel's again: the desktop let it go, or there was
// none. 1 when the shutdown screen took it, 0 to restore the console.
int splash_shutdown_takeover(void) {
    if (!(g_shutdown & SHUTDOWN_F_ACTIVE) || g_active || !splash_allowed()) return 0;
    draw_logo();
    shutdown_draw_lines();
    g_until = coarse_ticks() + SPLASH_MAX_TICKS;   // a shutdown that hangs gets its log back
    g_log_wanted = 0;
    g_active = 1;
    klog_write("splash: shutdown screen on -- Esc shows the log\n");
    return 1;
}

void splash_shutdown_set(uint32_t flags, const char *line, const char *detail) {
    if (flags & SHUTDOWN_F_ACTIVE) g_shutdown = (int)(flags & (SHUTDOWN_F_ACTIVE | SHUTDOWN_F_REBOOT));
    if (!(g_shutdown & SHUTDOWN_F_ACTIVE)) return;
    if (line) k_strlcpy(g_line, line, sizeof g_line);
    if (detail) k_strlcpy(g_detail, detail, sizeof g_detail);
    if (g_active) shutdown_draw_lines();
}

void splash_shutdown_get(struct shutdown_status *out) {
    k_memset(out, 0, sizeof *out);
    out->flags = (uint32_t)g_shutdown;
    k_strlcpy(out->line, g_line, sizeof out->line);
    k_strlcpy(out->detail, g_detail, sizeof out->detail);
}

void splash_end(int show_log) {
    if (!g_active) return;
    g_active = 0;
    g_log_wanted = 0;
    if (show_log) gfx_present_all();
    klog_write(show_log ? "splash: off -- the boot log is on screen\n"
                        : "splash: off -- handed to the desktop\n");
}

void splash_key(uint16_t keycode, int down) {
    if (g_active && down && keycode == INPUT_KEY_ESC) g_log_wanted = 1;
}

void splash_poll(void) {
    if (!g_active) return;
    int esc = g_log_wanted;
    if (!esc && coarse_ticks() < g_until) return;
    // ENDED BEFORE ANYTHING IS LOGGED: a log line reaches the console,
    // whose present calls back in here -- with the splash still up, that
    // recursed until the serial port was all the CPU did.
    splash_end(1);
    klog_write(esc ? "splash: Esc pressed\n" : "splash: nothing claimed the screen in time\n");
}

// --- the registry descriptor (setting.h) ------------------------------

static const char *const MESSAGES[] = { "simple", "detailed" };

static int messages_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= 2) return 0;
    k_strlcpy(out, MESSAGES[index], out_size);
    return 1;
}

static void messages_get(char *out, uint32_t out_size) {
    if (!etc_config_get(MESSAGES_FILE, MESSAGES_KEY, out, out_size) ||
        (k_strcmp(out, "simple") && k_strcmp(out, "detailed")))
        k_strlcpy(out, "simple", out_size);
}

// Read at the next boot, by the kernel here and by the compositor
// (wm_setup_hold.c): there is nothing to apply now but the file.
static int messages_apply(const char *value) {
    if (k_strcmp(value, "simple") && k_strcmp(value, "detailed")) return SETTING_INVALID;
    return etc_config_set(MESSAGES_FILE, MESSAGES_KEY, value) ? SETTING_SAVED : SETTING_UNSAVED;
}

static const struct setting g_messages_setting = {
    .name     = MESSAGES_KEY,
    .label    = "Startup messages",
    .type     = SETTING_TYPE_ENUM,
    .file     = MESSAGES_FILE,
    .category = "System",
    .group    = "Startup",
    .choice   = messages_choice,
    .get      = messages_get,
    .apply    = messages_apply,
};

void splash_setting_register(void) { setting_register(&g_messages_setting); }
