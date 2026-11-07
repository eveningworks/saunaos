// Screenshot's Options window: screenshot.ui's `options #options`, its
// rows bound to shot_conf.c's table, and the ones a table cannot say.
// The Print Screen keys are read from and written to /etc/shortcuts.conf
// (shot_conf.c), never kept here.
#include "screenshot.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ui/uapp.h"
#include "base/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_label.h"
#include "ui/uui_nametpl.h"
#include "ui/uui_builder.h"
#include "ui/uui_prefs.h"
#include "ui/uui_segmented.h"

enum { OPT_KEY0 = 1, OPT_KEY1, OPT_KEY2, OPT_DELAYS, OPT_FOLDER, OPT_BROWSE, OPT_NAME, OPT_FORMAT };
static const struct uui_builder_name NAMES[] = {
    { "key0", OPT_KEY0 }, { "key1", OPT_KEY1 }, { "key2", OPT_KEY2 }, { "delays", OPT_DELAYS },
    { "folder", OPT_FOLDER }, { "browse", OPT_BROWSE }, { "name", OPT_NAME }, { "format", OPT_FORMAT },
};
static const char *const PAGE_NAMES[SHOT_PAGES] = { "capture", "after", "saving" };   // by SHOT_PAGE_*
// THE TIMER'S CHOICES AS SETS, not three free numbers: a row holds two
// controls, and nobody wants a 7 s timer. A hand-edited file that names
// some other set keeps it until one of these is chosen.
static const int DELAY_SETS[][SHOT_DELAYS] = { { 3, 5, 10 }, { 2, 5, 10 }, { 5, 10, 30 }, { 1, 3, 5 } };
static const char *const DELAY_OPTS[] = { "3, 5 or 10 seconds", "2, 5 or 10 seconds",
                                          "5, 10 or 30 seconds", "1, 3 or 5 seconds" };
#define DELAY_SET_COUNT 4

static struct uui_builder *g_ui;
static struct shot_conf g_edit;
static void (*g_on_commit)(const struct shot_conf *next);
static struct uapp *g_app;

static struct uui_segmented *g_format;
static int g_png_at;   // the format row's PNG segment; the other is QOI
static struct uui_dropdown *g_delays, *g_key[SHOT_KEYS];
static struct uui_label *g_folder;
static struct uui_nametpl g_name;
static struct shot_vars g_sample;
static char g_folder_text[SHOT_DIR_MAX + 48];
static struct uui_filedialog g_chooser;

int shot_prefs_load(void) {
    static const struct uui_builder_prefs TABLES[] = { { &shot_conf_prefs, 0 } };
    static const char *const KEY_NAMES[SHOT_KEYS] = { "key0", "key1", "key2" };
    g_ui = uui_builder_load("screenshot", NAMES, (int)(sizeof NAMES / sizeof NAMES[0]));
    for (int i = 0; i < SHOT_KEYS; i++) g_key[i] = uui_builder_widget(g_ui, "options", KEY_NAMES[i], "dropdown");
    g_delays = uui_builder_widget(g_ui, "options", "delays", "dropdown");
    g_folder = uui_builder_widget(g_ui, "options", "folder", "label");
    g_format = uui_builder_widget(g_ui, "options", "format", "segmented");
    g_png_at = uui_builder_choice(g_ui, "options", "format", "png");
    uui_builder_choice(g_ui, "options", "format", "qoi");
    struct uui_item *name = uui_builder_item(g_ui, "options", "name");
    if (name) {
        name->ops = &uui_nametpl_ops;
        name->widget = &g_name;
    }
    uui_builder_options(g_ui, "options", TABLES, 1);
    if (uui_builder_error(g_ui)) return 0;
    for (int i = 0; i < SHOT_KEYS; i++) uui_dropdown_set_items(g_key[i], SHOT_KEY_LABEL, SHOT_KEY_CHOICES);
    uui_dropdown_set_items(g_delays, DELAY_OPTS, DELAY_SET_COUNT);
    return 1;
}

int shot_prefs_load_error(void) { return uui_builder_run_error(g_ui); }

static int delay_set_of(const int d[SHOT_DELAYS]) {
    for (int i = 0; i < DELAY_SET_COUNT; i++)
        if (!memcmp(DELAY_SETS[i], d, sizeof DELAY_SETS[i])) return i;
    return -1;
}

static void show_folder(const char *note) {
    if (note) snprintf(g_folder_text, sizeof g_folder_text, "%s", note);
    else snprintf(g_folder_text, sizeof g_folder_text, "%s", g_edit.folder);
    uui_label_set_text(g_folder, g_folder_text);
}

// The rows the window cannot bind: the timer's SETS, the keys (which live
// in /etc/shortcuts.conf), the folder, the template and the format.
static void to_controls(void) {
    int ds = delay_set_of(g_edit.delays);
    uui_dropdown_set_selected(g_delays, ds >= 0 ? ds : 0);
    int keys[SHOT_KEYS];
    shot_keys_get(keys);
    for (int i = 0; i < SHOT_KEYS; i++) uui_dropdown_set_selected(g_key[i], keys[i]);
    show_folder(0);
    uui_textbox_set_text(&g_name.field, g_edit.name);
    g_format->selected = g_edit.png ? g_png_at : !g_png_at;
}

static void from_controls(void) {
    int ds = uui_dropdown_selected(g_delays);
    if (ds >= 0 && ds < DELAY_SET_COUNT && ds != delay_set_of(g_edit.delays))
        memcpy(g_edit.delays, DELAY_SETS[ds], sizeof g_edit.delays);
    // AN UNUSABLE TEMPLATE IS NOT SAVED: the field said so under itself,
    // and the name that works stays.
    if (uui_nametpl_valid(&g_name) && strlen(uui_nametpl_text(&g_name)) < sizeof g_edit.name)
        snprintf(g_edit.name, sizeof g_edit.name, "%s", uui_nametpl_text(&g_name));
    g_edit.png = g_format->selected == g_png_at;
}

static void edit_defaults(void *edit) { shot_conf_defaults(edit); }

static void on_defaults(void) {
    to_controls();
    // The keys' factory bindings: Print Screen opens the bar, Shift saves
    // the screen, Alt the window (base/ushortcut_actions.c).
    for (int i = 0; i < SHOT_KEYS; i++) uui_dropdown_set_selected(g_key[i], i);
}

static void on_ok(void) {
    from_controls();
    int keys[SHOT_KEYS];
    for (int i = 0; i < SHOT_KEYS; i++) {
        int k = uui_dropdown_selected(g_key[i]);
        keys[i] = k >= 0 && k < SHOT_KEY_CHOICES ? k : SHOT_KEY_NONE;
    }
    if (!shot_keys_set(keys)) ulog("screenshot: could not write the Print Screen keys\n");
    ulogf("screenshot: options saved (folder %s, name %s, %s)\n", g_edit.folder, g_edit.name,
          g_edit.png ? "png" : "qoi");
    if (g_on_commit) g_on_commit(&g_edit);
}

static void folder_chosen(void *ctx, const char *path) {
    (void)ctx;
    if (!path) return;
    if (!shot_folder_ok(path)) {
        // REFUSED, with the reason where the folder is shown: the card
        // names the file, and hands the path to Files on one line.
        show_folder("Choose a folder whose name has no spaces or symbols");
    } else {
        snprintf(g_edit.folder, sizeof g_edit.folder, "%s", path);
        show_folder(0);
    }
    uui_prefs_redraw();
}

static void on_action(int code) {
    if (code != OPT_BROWSE || uui_filedialog_is_open(&g_chooser)) return;
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_FOLDER,
        .title = "Save Screenshots In",
        .start_dir = g_edit.folder,
    };
    uui_filedialog_open(g_app, &g_chooser, &o, folder_chosen, 0);
}

// The preview names the file in the format chosen beside it.
static void on_widget(int id, int reason) {
    (void)reason;
    if (id == OPT_FORMAT) g_name.suffix = g_format->selected == g_png_at ? ".png" : ".qoi";
}

int shot_prefs_is_open(void) { return uui_prefs_is_open(); }

void shot_prefs_open(struct uapp *a, const struct shot_conf *c, int page,
                     void (*on_commit)(const struct shot_conf *next)) {
    if (uui_prefs_is_open()) return;
    g_app = a;
    g_edit = *c;
    g_on_commit = on_commit;

    shot_vars_now(&g_sample, "notepad", SHOT_MODE_REGION);
    uui_nametpl_init(&g_name, g_edit.name, SHOT_CHIPS, SHOT_CHIP_COUNT, g_sample.v, SHOT_CHIP_COUNT);
    g_name.suffix = g_edit.png ? ".png" : ".qoi";
    to_controls();

    struct uui_prefs_desc d = {
        .log_prefix = "options",
        .edit = &g_edit,
        .edit_defaults = edit_defaults,
        .on_defaults = on_defaults,
        .on_ok = on_ok,
        .on_widget = on_widget,
        .on_action = on_action,
    };
    const char *first = page >= 0 && page < SHOT_PAGES ? PAGE_NAMES[page] : 0;
    if (!uui_builder_options_open(g_ui, "options", &d, a, first, NULL)) ulog("screenshot: could not open Options\n");
}
