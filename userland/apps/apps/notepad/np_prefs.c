// Notepad's Options window: notepad.ui's `options #options`, every row
// bound to a key of np_conf.c's table but Clear list, a command.
#include "notepad.h"

#include <string.h>

#include "base/ulog.h"
#include "ui/uapp.h"
#include "ui/uui_builder.h"
#include "ui/uui_button.h"
#include "ui/uui_prefs.h"

enum { OPT_CLEAR = 1 };
static const struct uui_builder_name NAMES[] = { { "clear", OPT_CLEAR } };

static struct uui_builder *g_ui;
static struct uui_button *g_clear;
static struct np_conf g_edit;
static void (*g_on_commit)(const struct np_conf *next);
static void (*g_clear_fn)(void);

int np_prefs_load(void) {
    static const struct uui_builder_prefs TABLES[] = { { &np_conf_prefs, 0 } };
    g_ui = uui_builder_load("notepad", NAMES, (int)(sizeof NAMES / sizeof NAMES[0]));
    g_clear = uui_builder_widget(g_ui, "options", "clear", "button");
    uui_builder_options(g_ui, "options", TABLES, 1);
    return !uui_builder_error(g_ui);
}

int np_prefs_load_error(void) { return uui_builder_run_error(g_ui); }

static void edit_defaults(void *edit) { np_conf_defaults(edit); }

static void on_ok(void) {
    if (g_on_commit) g_on_commit(&g_edit);
}

int np_prefs_is_open(void) { return uui_prefs_is_open(); }

static void on_action(int code) {
    if (code != OPT_CLEAR) return;
    if (g_clear_fn) g_clear_fn();
    g_clear->disabled = 1;
}

void np_prefs_open(struct uapp *a, const struct np_conf *c,
                   void (*on_commit)(const struct np_conf *next),
                   void (*on_clear_recent)(void)) {
    if (uui_prefs_is_open()) return;
    g_edit = *c;
    g_on_commit = on_commit;
    g_clear_fn = on_clear_recent;
    g_clear->disabled = 0;
    struct uui_prefs_desc d = {
        .log_prefix = "options",
        .edit = &g_edit,
        .edit_defaults = edit_defaults,
        .on_ok = on_ok,
        .on_action = on_action,
    };
    if (uui_builder_options_open(g_ui, "options", &d, a, NULL, NULL)) ulog("notepad: options open\n");
}
