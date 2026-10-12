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

#define MESSAGES_FILE "/etc/toyos.conf"
#define MESSAGES_KEY  "boot_messages"

static volatile int g_active;
static volatile int g_log_wanted;
static uint64_t g_until;
static struct splash_layout g_l;
static int g_text_w;   // the line on screen now, so the next one can clear it

static uint32_t pack(uint32_t rgb) {
    return gfx_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

static void round_rect(const struct splash_rect *r, uint32_t color) {
    for (int row = 0; row < r->h; row++) {
        int in = splash_round_inset(r, row);
        gfx_front_fill_rect(r->x + in, r->y + row, r->w - 2 * in, 1, color);
    }
}

static void say(const char *text) {
    int w = gfx_text_width(text), sw = gfx_width();
    if (g_text_w) gfx_front_fill_rect((sw - g_text_w) / 2, g_l.label_y, g_text_w, gfx_char_h(),
                                      pack(SPLASH_BG));
    gfx_front_draw_string((sw - w) / 2, g_l.label_y, text, pack(SPLASH_TEXT), pack(SPLASH_BG));
    g_text_w = w;
}

void splash_begin(void) {
    char v[16];
    if (multiboot_cmdline_value("splash=", v, sizeof v) && !k_strcmp(v, "off")) return;
    if (multiboot_cmdline_value("target=", v, sizeof v) && k_strcmp(v, "graphical")) return;
    if (!gfx_double_buffered() || gfx_width() <= 0) return;   // no page behind it to keep

    splash_layout(gfx_width(), gfx_height(), &g_l);
    gfx_front_fill_rect(0, 0, gfx_width(), gfx_height(), pack(SPLASH_BG));
    round_rect(&g_l.plate, pack(SPLASH_PLATE));
    for (int i = 0; i < 3; i++) round_rect(&g_l.brick[i], pack(SPLASH_BRICK));
    say("Starting toy-os...");
    g_until = coarse_ticks() + SPLASH_MAX_TICKS;
    g_active = 1;
    klog_write("splash: on -- Esc shows the boot log\n");
}

int splash_active(void) { return g_active; }

// `detailed` names each stage; `simple`, the default, says "Starting
// toy-os..." until the desktop. Read when init starts, the first moment
// /etc is there to read.
void splash_stage_services(void) {
    char v[16];
    if (!g_active) return;
    if (etc_config_get(MESSAGES_FILE, MESSAGES_KEY, v, sizeof v) && !k_strcmp(v, "detailed"))
        say("Starting services...");
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
