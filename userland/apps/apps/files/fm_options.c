// Options: the File Manager's preferences and the window that edits them
// (See more > Options) -- the shared Options window (ui/uui_prefs.h) over
// one base/uprefs.h table, Dolphin's Configure dialog's shape. Stored in
// FILES_CONF beside the rest of the app's state; OK writes and applies,
// Cancel forgets.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "ui/uui_prefs.h"
#include "ui/uui_segmented.h"
#include "ui/uui_builder.h"
#include "base/uconf.h"
#include "base/ulaunch.h"
#include "base/uprefs.h"
#include "base/ufolderopts.h"
#include "base/ulog.h"
#include <stddef.h>
#include <string.h>

struct fm_options g_opt;

// --- the file -------------------------------------------------------------

static const char *const START_WORDS[]  = { "last", "home", "root" };          // FM_START_*
static const char *const VIEW_WORDS[]   = { "icons", "large", "details" };     // FM_VIEW_*
static const char *const RENAME_WORDS[] = { "inplace", "dialog" };

// The defaults: what Explorer and Dolphin ship. The options the desktop
// follows too are base/ufolderopts.h's table, in the same file.
#define C struct fm_options
static const struct upref KEYS[] = {
    UPREF_WORD_KEY("start",          C, start,          START_WORDS, 3, FM_START_LAST),
    UPREF_WORD_KEY("view",           C, view,           VIEW_WORDS, 3, FM_VIEW_ICONS),
    UPREF_WORD_KEY("rename",         C, rename_dialog,  RENAME_WORDS, 2, 0),
};
#undef C
static const struct uprefs PREFS = { FILES_CONF, KEYS, (int)(sizeof KEYS / sizeof KEYS[0]) };

void options_load(void) {
    memset(&g_opt, 0, sizeof g_opt);
    uprefs_load(&PREFS, &g_opt);
    ufolderopts_load(&g_opt.folder);
}

// "New windows use" is what the panes open in, so a CHANGE to it is
// their view too -- an OK that left it alone keeps each pane's own.
static void save(int view_changed) {
    uprefs_save(&PREFS, &g_opt, 0);
    uprefs_save(&ufolderopts_prefs, &g_opt.folder, 0);
    if (!view_changed) return;
    uconf_set(FILES_CONF, "left_view", VIEW_WORDS[g_opt.view]);
    uconf_set(FILES_CONF, "right_view", VIEW_WORDS[g_opt.view]);
}

void options_apply(void) {
    for (int i = 0; i < 2; i++) {
        struct uui_fileview *fv = &g_pane[i];
        fv->single_click = g_opt.folder.single_click;
        fv->hide_ext = !g_opt.folder.extensions;
        uui_fileview_set_thumb(fv, g_opt.folder.thumbs ? pane_thumb : 0, 0);
        uui_fileview_set_badge(fv, g_opt.folder.thumbs ? pane_badge : 0);
    }
    reload_panes();   // hidden files come and go with the filter
}

// --- the window -----------------------------------------------------------
// files.ui's `options #options`: every row bound to a key of the two
// tables but Scripts and Programs, which are /etc/mimeapps.conf's.

// A SEGMENT IS KNOWN BY ITS id= (the file may reorder them): which act each is.
static const struct { const char *id; int act; } ACTS[] = {
    { "ask", ULAUNCH_ASK }, { "terminal", ULAUNCH_TERMINAL }, { "run", ULAUNCH_RUN }, { "edit", ULAUNCH_EDIT },
};
#define NACTS (int)(sizeof ACTS / sizeof ACTS[0])

static struct uui_builder *g_ui;
static struct fm_options g_edit;   // what the window shows, until OK
static struct uui_segmented *g_scripts, *g_programs;
static int g_script_at[NACTS], g_program_at[NACTS];   // an act's segment; -1 when the row has none

static void acts_of(const char *row, int *at, int needed) {
    for (int i = 0; i < NACTS; i++) at[i] = i < needed ? uui_builder_choice(g_ui, "options", row, ACTS[i].id) : -1;
}

int options_ui_load(void) {
    static const struct uui_builder_prefs TABLES[] = {
        { &PREFS, 0 }, { &ufolderopts_prefs, offsetof(struct fm_options, folder) },
    };
    g_ui = uui_builder_load("files", 0, 0);
    g_scripts = uui_builder_widget(g_ui, "options", "scripts", "segmented");
    g_programs = uui_builder_widget(g_ui, "options", "programs", "segmented");
    acts_of("scripts", g_script_at, 4);     // a script may be opened in Notepad,
    acts_of("programs", g_program_at, 3);   // a program not
    uui_builder_options(g_ui, "options", TABLES, 2);
    return !uui_builder_error(g_ui);
}

int options_ui_error(void) { return uui_builder_run_error(g_ui); }

static void act_to(struct uui_segmented *s, const int *at, int act) {
    for (int i = 0; i < NACTS; i++)
        if (ACTS[i].act == act && at[i] >= 0) { s->selected = at[i]; return; }
    s->selected = at[0];   // a policy the row does not offer shows Ask
}

static int act_of(const struct uui_segmented *s, const int *at) {
    for (int i = 0; i < NACTS; i++)
        if (at[i] == s->selected) return ACTS[i].act;
    return ULAUNCH_ASK;
}

static void edit_defaults(void *edit) {
    memset(edit, 0, sizeof g_edit);
    uprefs_defaults(&PREFS, edit);
    uprefs_defaults(&ufolderopts_prefs, &((struct fm_options *)edit)->folder);
}

static void on_defaults(void) {
    act_to(g_scripts, g_script_at, ULAUNCH_ASK);
    act_to(g_programs, g_program_at, ULAUNCH_ASK);
}

static void on_ok(void) {
    int view_changed = g_edit.view != g_opt.view;
    g_opt = g_edit;
    save(view_changed);
    if (!ulaunch_set_policy(ULAUNCH_SCRIPT, act_of(g_scripts, g_script_at)) ||
        !ulaunch_set_policy(ULAUNCH_PROGRAM, act_of(g_programs, g_program_at)))
        ulog("files: could not save what a double-click runs\n");
    if (view_changed)
        for (int i = 0; i < 2; i++) {
            g_pane[i].icon_px = g_opt.view == FM_VIEW_LARGE ? ugfx_char_h() * 6 : 0;
            uui_fileview_set_mode(&g_pane[i], g_opt.view == FM_VIEW_DETAILS
                                                  ? UUI_FILEVIEW_DETAILS : UUI_FILEVIEW_ICONS);
        }
    options_apply();
    refresh_status();
    uapp_redraw(g_app);
}

void options_open(struct uapp *a) {
    if (uui_prefs_is_open()) return;   // one at a time
    g_edit = g_opt;
    act_to(g_scripts, g_script_at, ulaunch_policy(ULAUNCH_SCRIPT));
    act_to(g_programs, g_program_at, ulaunch_policy(ULAUNCH_PROGRAM));
    struct uui_prefs_desc d = {
        .log_prefix = "options",
        .edit = &g_edit,
        .edit_defaults = edit_defaults,
        .on_defaults = on_defaults,
        .on_ok = on_ok,
    };
    if (!uui_builder_options_open(g_ui, "options", &d, a, NULL, NULL)) ulog("files: could not open Options\n");
}
