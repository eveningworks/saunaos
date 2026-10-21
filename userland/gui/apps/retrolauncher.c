// retrolauncher.c -- Retro Launcher: the Windows 3.x programs in a few folders,
// each with its own icon and how far w16 runs it, run with a double click
// and set up per program (docs/w16-design.md). The design is the
// "Win16 Programs launcher" canvas (its old name)'s L1 and S2: folders on the left,
// program tiles, a details panel with the program's own settings, and an
// Options window for the defaults (w16/w16prefs.h, which w16 reads too).
//
// THIS APP NEVER PARSES A PROGRAM. A Windows executable is hostile input;
// `w16 --check` reads it inside w16's sandbox and answers in plain lines
// -- its name, what it imports that w16 lacks, its icon as pixels -- and
// that answer is all this reads. A folder is checked a few programs a
// tick, so a big one fills in rather than holding the window shut.
//
// `retrolauncher FILE` runs that program and exits without a window: what the
// File Manager's Open does for an .exe (Handles= in its desktop entry).
// `retrolauncher --calls FILE` is the window listing what it imports
// (retrolauncher/calls.c), which the details panel's button opens.
#include <stdarg.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "keyboard.h"
#include "rt/sys.h"
#include "lib/icon_cache.h"
#include "lib/ulistdir.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui.h"
#include "ui/uui_button.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_grid.h"
#include "ui/uui_listedit.h"
#include "ui/uui_menubar.h"
#include "ui/uui_nameask.h"
#include "ui/uui_prefs.h"
#include "ui/uui_runask.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_stackbar.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_table.h"
#include "ui/uui_toolbar.h"
#include "w16/w16prefs.h"
#include "retrolauncher/retrolauncher.h"

#define MISSING_SHOWN 3                      // the panel names this many; the window lists all

// A stand-in's note and the calls w16 answers that way: "CreateDC, Escape"
// share "no printer to draw on" when w16 says the same of both.
struct partly {
    char *names;
    char *note;
    char *key;            // the first call's "USER.121", for its Why?
    char *first;          // ...and its name
};

// Every string is allocated (prog_free()); `icon` is w16_icon()'s largest.
struct prog {
    char *path;
    char *name;                      // its description, else its file name
    char *title;                     // what its tile says: w16_short_title()
    char *module;
    char *foreign;                   // not a Windows 3.x program: what it is ("a DOS program")
    int folder;                      // which folder it was found in
    int checked;                     // `w16 --check` has answered
    int ok;                          // ...and it is a Win16 program
    int imports, has;                // `has` counts the stand-ins too
    int partly;                      // ...this many of it
    struct partly *part;             // by note, `npart` of them
    int npart;
    char *missing[MISSING_SHOWN];
    int nmissing;
    uint32_t icon[32 * 32];
    int iw, ih;
    int last;                        // 0 never run, 1 ended, 2 stopped
    long last_when;
    char *last_text;
};

static struct prog *g_prog;
static int g_nprog, g_next_check;
static int *g_view, g_nview;                 // the programs the sidebar's choice shows
static struct w16_prefs g_def;               // the defaults
static struct w16_folder *g_folders;         // the folders programs are listed from
static int g_nfolders;
static struct uapp *g_app;

enum { CMD_RUN = 1, CMD_PROPS, CMD_ADD, CMD_REMOVE, CMD_REFRESH, CMD_OPTIONS, CMD_EXIT, CMD_CALLS,
       CMD_CHANGE, CMD_RENAME, CMD_UP, CMD_DOWN };
enum { ID_MENU = 1, ID_TOOLBAR, ID_SIDEBAR, ID_GRID, ID_STATUS, ID_SCALE, ID_VERSION, ID_FRAME,
       ID_TRACE, ID_RUN, ID_CALLS, ID_METHOD, ID_FONTS, ID_OPT_FONTCARD, ID_DRIVEC, ID_CTX,
       ID_DRV_ADD, ID_DRV_CHANGE, ID_DRV_REMOVE,
       ID_FOL_ADD, ID_FOL_CHANGE, ID_FOL_RENAME, ID_FOL_REMOVE, ID_FOL_UP, ID_FOL_DOWN, ID_SOUNDS, ID_SOUND, ID_HELP };

static struct uui_menubar g_menu;
static struct uui_menubar g_ctx;             // a folder's right-click menu
static struct uui_toolbar g_tb;
static struct uui_sidebar g_side;
static struct uui_grid g_grid;
static struct uui_statusbar g_sb;
#define NSET 8                               // the per-program settings: SETTINGS below
static struct uui_dropdown g_dd[NSET];
static struct uui_dropdown g_ddc;            // its drive C:, a folder rather than a choice
static struct uui_button g_run;
static struct uui_button g_calls;            // "Missing calls...": the calls window
static struct uui_filedialog g_chooser;      // ~20 KB: file scope
static int g_panel = 1;                      // the details panel is open
static int g_px, g_py, g_pw, g_ph;           // where it is
static char g_st_count[40], g_st_runs[40], g_st_need[48];

// --- reading what w16 says -----------------------------------------------------

static const char *file_name(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

// `*to` = a copy of `s`, the old one freed; an allocation that fails
// leaves "" rather than NULL, so a draw never checks.
static void set_str(char **to, const char *s) {
    free(*to);
    *to = strdup(s ? s : "");
}

static const char *str(const char *s) { return s ? s : ""; }

static void prog_free(struct prog *p) {
    free(p->path); free(p->name); free(p->title); free(p->module); free(p->last_text); free(p->foreign);
    for (int i = 0; i < p->nmissing; i++) free(p->missing[i]);
    for (int i = 0; i < p->npart; i++) {
        free(p->part[i].names); free(p->part[i].note); free(p->part[i].key); free(p->part[i].first);
    }
    free(p->part);
    memset(p, 0, sizeof *p);
}

static void read_last(struct prog *p) {
    char path[W16_PATH_MAX + 32];
    w16_prog_last(p->path, path, sizeof path);
    p->last = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = 0;
    size_t cap = 0;
    if (getline(&line, &cap, f) > 0) {
        char *rest;
        p->last_when = strtol(line, &rest, 10);
        while (*rest == ' ') rest++;
        rest[strcspn(rest, "\n")] = 0;
        p->last = !strncmp(rest, "stopped", 7) ? 2 : 1;
        set_str(&p->last_text, p->last == 2 ? rest + (rest[7] ? 8 : 7) : "");
    }
    free(line);
    fclose(f);
}

// "call partly GDI.38 Escape Printing<tab>a printer's call; refused": the
// name joins the row for its note, or starts one.
static void add_partly(struct prog *p, char *line) {
    char *note = strchr(line, '\t'), *name = strchr(line + 12, ' ');
    if (!note || !name || name > note) return;
    *note++ = 0;
    *name = 0;                                   // line + 12: "USER.121"
    char *end = strchr(++name, ' ');
    if (end) *end = 0;
    int i = 0;
    while (i < p->npart && strcmp(p->part[i].note, note)) i++;
    if (i == p->npart) {
        struct partly *g = realloc(p->part, sizeof *p->part * (size_t)(p->npart + 1));
        if (!g) return;
        p->part = g;
        p->part[i].names = strdup(name);
        p->part[i].note = strdup(note);
        p->part[i].key = strdup(line + 12);
        p->part[i].first = strdup(name);
        if (!p->part[i].names || !p->part[i].note || !p->part[i].key || !p->part[i].first) {
            free(p->part[i].names); free(p->part[i].note); free(p->part[i].key); free(p->part[i].first);
            return;
        }
        p->npart++;
        return;
    }
    size_t n = strlen(p->part[i].names) + strlen(name) + 3;
    char *joined = malloc(n);
    if (!joined) return;
    snprintf(joined, n, "%s, %s", p->part[i].names, name);
    free(p->part[i].names);
    p->part[i].names = joined;
}

// `w16 --check PATH`'s lines into `p`.
static void check(struct prog *p) {
    p->checked = 1;
    char *out = retro_check(p->path);
    if (!out) return;
    int icon_row = -1;
    for (char *line = out; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (icon_row >= 0 && icon_row < p->ih) {
            for (int x = 0; x < p->iw && line[x * 8]; x++) {
                char hex[9];
                memcpy(hex, line + x * 8, 8);
                hex[8] = 0;
                p->icon[icon_row * p->iw + x] = (uint32_t)strtoul(hex, 0, 16);
            }
            icon_row++;
        } else if (!strncmp(line, "module ", 7)) {
            set_str(&p->module, line + 7);
            p->ok = 1;
        } else if (!strncmp(line, "foreign ", 8)) {
            set_str(&p->foreign, line + 8);
        } else if (!strncmp(line, "description ", 12)) {
            set_str(&p->name, line + 12);
        } else if (!strncmp(line, "imports ", 8)) {
            p->imports = atoi(line + 8);
        } else if (!strncmp(line, "has ", 4)) {
            p->has = atoi(line + 4);
        } else if (!strncmp(line, "partly ", 7)) {
            p->partly = atoi(line + 7);
        } else if (!strncmp(line, "call partly ", 12)) {
            add_partly(p, line);
        } else if (!strncmp(line, "call missing ", 13)) {
            if (p->nmissing < MISSING_SHOWN) {
                // "call missing USER.150 LoadMenu Menus": the name, else USER.150
                char *imp = line + 13, *name = strchr(imp, ' ');
                char *end = name ? strchr(name + 1, ' ') : 0;
                if (end) *end = 0;
                if (name && name[1] != '?') imp = name + 1;
                else if (name) *name = 0;
                p->missing[p->nmissing] = 0;
                set_str(&p->missing[p->nmissing++], imp);
            }
        } else if (!strncmp(line, "icon ", 5)) {
            int w = 0, h = 0;
            sscanf(line + 5, "%d %d", &w, &h);
            if (w > 0 && h > 0 && w <= 32 && h <= 32) { p->iw = w; p->ih = h; icon_row = 0; }
        }
        line = nl ? nl + 1 : 0;
    }
    free(out);
    if (!p->name || !p->name[0]) set_str(&p->name, file_name(p->path));
    // A tile is narrow: "Microsoft Windows Notepad Application (c)1990"
    // says "Notepad Application" there, and the panel keeps the whole name.
    free(p->title);
    p->title = w16_short_title(p->name);
    read_last(p);
}

static int is_exe(const char *name) {
    size_t n = strlen(name);
    return n > 4 && name[n - 4] == '.' && (name[n - 3] | 0x20) == 'e' && (name[n - 2] | 0x20) == 'x' &&
           (name[n - 1] | 0x20) == 'e';
}

static void find_programs(void) {
    for (int i = 0; i < g_nprog; i++) prog_free(&g_prog[i]);
    g_nprog = g_next_check = 0;
    static int cap;
    for (int f = 0; f < g_nfolders; f++) {
        const char *dir = g_folders[f].path;
        struct sys_dirent *ents;
        int n = ulistdir(dir, &ents);
        for (int i = 0; i < n; i++) {
            if (!is_exe(ents[i].name)) continue;
            if (g_nprog == cap) {
                int c = cap ? cap * 2 : 64;
                struct prog *g = realloc(g_prog, sizeof *g_prog * (size_t)c);
                if (g) g_prog = g;
                int *v = g ? realloc(g_view, sizeof *g_view * (size_t)c) : 0;
                if (v) g_view = v;
                if (!g || !v) break;
                cap = c;
            }
            struct prog *p = &g_prog[g_nprog++];
            memset(p, 0, sizeof *p);
            size_t len = strlen(dir) + strlen(ents[i].name) + 2;
            if ((p->path = malloc(len))) snprintf(p->path, len, "%s/%s", dir, ents[i].name);
            set_str(&p->name, ents[i].name);
            set_str(&p->title, ents[i].name);
            p->folder = f;
            if (!p->path) g_nprog--;
        }
        free(n >= 0 ? ents : 0);
    }
}

// --- the sidebar: All programs, then each folder ---------------------------------

static struct uui_sidebar_row *g_rows;       // "All programs", then a row per folder

// What a folder is called: its own name, else the folder's.
static const char *folder_label(const struct w16_folder *f) {
    if (f->name[0]) return f->name;
    return !strcmp(f->path, "/usr/share/w16") ? "Samples" : file_name(f->path);
}

// Rebuilt whenever g_folders changes: the labels point into it.
static void build_sidebar(void) {
    struct uui_sidebar_row *g = realloc(g_rows, sizeof *g_rows * (size_t)(g_nfolders + 1));
    if (!g) return;
    g_rows = g;
    int n = 0;
    g_rows[n++] = (struct uui_sidebar_row){ "All programs", UUI_SIDEBAR_TOP, 0, -1 };
    for (int f = 0; f < g_nfolders; f++)
        g_rows[n++] = (struct uui_sidebar_row){ folder_label(&g_folders[f]), UUI_SIDEBAR_TOP, 0, f };
    uui_sidebar_set_rows(&g_side, g_rows, n);
    if (g_side.selected < 0) uui_sidebar_select_id(&g_side, -1);
}

static void build_view(void) {
    int want = uui_sidebar_selected_id(&g_side);
    g_nview = 0;
    for (int i = 0; i < g_nprog; i++)
        if ((want < 0 || g_prog[i].folder == want) && (!g_prog[i].checked || g_prog[i].ok))
            g_view[g_nview++] = i;
    uui_grid_set(&g_grid, g_nview, g_grid.cell, 0);
    if (g_grid.selected >= g_nview) g_grid.selected = g_nview - 1;
}

static struct prog *cur(void) {
    return g_grid.selected >= 0 && g_grid.selected < g_nview ? &g_prog[g_view[g_grid.selected]] : 0;
}

// --- the program's own settings, in the panel ------------------------------------

static const char *const SCALE_NAMES[W16_SCALE_COUNT] = { "Auto", "1x", "1.25x", "1.5x", "1.75x", "2x",
                                                          "2.5x", "3x" };
// Named for what each looks like; the canvas's R3, R1 and R2.
static const char *const METHOD_NAMES[] = { "Sharp", "Pixels", "Smooth" };
static const char *const VERSION_NAMES[] = { "3.10", "3.11", "3.00" };
static const char *const FRAME_NAMES[] = { "Windows 3.1", "The desktop's" };
static const char *const TRACE_NAMES[] = { "Off", "On" };
static const char *const FONTS_NAMES[] = { "Windows 3.11's own", "toy-os's stand-ins" };
static const char *const SOUNDS_NAMES[] = { "Windows 3.11's own", "toy-os's own", "Off" };
static const char *const HELP_NAMES[] = { "toy-os Help Viewer", "WINHELP.EXE" };
static char g_dflt_label[NSET][48];
static const char *g_dd_items[NSET][W16_SCALE_COUNT + 1];   // the longest list, plus "Default"

// Each dropdown: "Default (what that is)", then the values. Its key's
// bit in w16_prefs_overridden() -- its place in w16_prefs_keys -- and
// the field it sets.
static const struct { const char *caption; const char *const *names; int n; int bit; size_t field; } SETTINGS[NSET] = {
    { "Scale",           SCALE_NAMES,   W16_SCALE_COUNT, 0, offsetof(struct w16_prefs, scale) },
    { "Scaled by",       METHOD_NAMES,  3, 6, offsetof(struct w16_prefs, method) },
    { "Version told",    VERSION_NAMES, 3, 2, offsetof(struct w16_prefs, version) },
    { "Window frame",    FRAME_NAMES,   2, 1, offsetof(struct w16_prefs, frame) },
    { "Fonts",           FONTS_NAMES,   2, 7, offsetof(struct w16_prefs, fonts) },
    { "Sound",           SOUNDS_NAMES,  3, 8, offsetof(struct w16_prefs, sounds) },
    { "Help",            HELP_NAMES,    2, 9, offsetof(struct w16_prefs, help) },
    { "Trace its calls", TRACE_NAMES,   2, 4, offsetof(struct w16_prefs, trace) },
};

static int *field_of(struct w16_prefs *p, int i) { return (int *)((char *)p + SETTINGS[i].field); }

// Drive C: per program: the default, the folder the program is in, a
// folder of its own once chosen, and Choose a folder... -- a path, so a
// list of its own rather than a SETTINGS row.
#define DRIVEC_BIT 5                         // drive_c's place in w16_prefs_keys
enum { DC_DEFAULT, DC_ITS_FOLDER, DC_OWN, DC_CHOOSE };
static char g_dc_label[3][W16_PATH_MAX + 32];
static const char *g_dc_items[4];
static int g_dc_kind[4], g_dc_n;

static void prog_dir(const struct prog *p, char *out, size_t cap) {
    snprintf(out, cap, "%s", p->path);
    char *slash = strrchr(out, '/');
    if (slash && slash != out) *slash = 0;
}

static void fill_drive_c(const struct prog *p, unsigned mask, const struct w16_prefs *eff) {
    char dir[W16_PATH_MAX] = "";
    if (p) prog_dir(p, dir, sizeof dir);
    int own = p && (mask & (1u << DRIVEC_BIT)), sel = 0;
    g_dc_n = 0;
    snprintf(g_dc_label[0], sizeof g_dc_label[0], "Default (%s)", g_def.drive_c);
    g_dc_items[g_dc_n] = g_dc_label[0]; g_dc_kind[g_dc_n++] = DC_DEFAULT;
    snprintf(g_dc_label[1], sizeof g_dc_label[1], "The folder it is in");
    if (own && !strcmp(eff->drive_c, dir)) sel = g_dc_n;
    g_dc_items[g_dc_n] = g_dc_label[1]; g_dc_kind[g_dc_n++] = DC_ITS_FOLDER;
    if (own && strcmp(eff->drive_c, dir)) {
        snprintf(g_dc_label[2], sizeof g_dc_label[2], "%s", eff->drive_c);
        sel = g_dc_n;
        g_dc_items[g_dc_n] = g_dc_label[2]; g_dc_kind[g_dc_n++] = DC_OWN;
    }
    g_dc_items[g_dc_n] = "Choose a folder..."; g_dc_kind[g_dc_n++] = DC_CHOOSE;
    uui_dropdown_set_items(&g_ddc, g_dc_items, g_dc_n);
    uui_dropdown_set_selected(&g_ddc, sel);
    g_ddc.disabled = !p || !p->ok;
}

// The program's own drive C: set to `path`, or back to the default (NULL).
static void set_prog_drive_c(const struct prog *p, const char *path) {
    static struct w16_prefs eff;                 // ~3 KB with the drives: not on the stack
    w16_prefs_load(&eff, p->path);
    unsigned mask = w16_prefs_overridden(p->path) & ~(1u << DRIVEC_BIT);
    if (path) {
        snprintf(eff.drive_c, sizeof eff.drive_c, "%s", path);
        mask |= 1u << DRIVEC_BIT;
    }
    w16_prefs_save_program(p->path, &eff, mask);
}

static void fill_panel(void) {
    struct prog *p = cur();
    unsigned mask = p ? w16_prefs_overridden(p->path) : 0;
    static struct w16_prefs eff;
    w16_prefs_load(&eff, p ? p->path : 0);
    for (int i = 0; i < NSET; i++) {
        snprintf(g_dflt_label[i], sizeof g_dflt_label[i], "Default (%s)",
                 SETTINGS[i].names[*field_of(&g_def, i)]);
        // Auto says what it comes to on this screen.
        if (i == 0 && g_def.scale == W16_SCALE_AUTO) {
            int q = w16_prefs_auto_q();
            snprintf(g_dflt_label[i], sizeof g_dflt_label[i], "Default (Auto: %gx)", q / 4.0);
        }
        g_dd_items[i][0] = g_dflt_label[i];
        for (int k = 0; k < SETTINGS[i].n; k++) g_dd_items[i][k + 1] = SETTINGS[i].names[k];
        uui_dropdown_set_items(&g_dd[i], g_dd_items[i], SETTINGS[i].n + 1);
        uui_dropdown_set_selected(&g_dd[i], mask & (1u << SETTINGS[i].bit) ? *field_of(&eff, i) + 1 : 0);
        g_dd[i].disabled = !p || !p->ok;
    }
    fill_drive_c(p, mask, &eff);
}

static void save_panel(void) {
    struct prog *p = cur();
    if (!p) return;
    static struct w16_prefs eff;
    w16_prefs_load(&eff, p->path);
    unsigned mask = w16_prefs_overridden(p->path);
    for (int i = 0; i < NSET; i++) {
        int sel = uui_dropdown_selected(&g_dd[i]);
        unsigned bit = 1u << SETTINGS[i].bit;
        if (sel <= 0) mask &= ~bit;
        else { mask |= bit; *field_of(&eff, i) = sel - 1; }
    }
    w16_prefs_save_program(p->path, &eff, mask);
}

static const struct prog *g_pick_prog;       // whose drive C: the chooser is for
static void prog_drive_c(const char *path) {
    for (int i = 0; i < g_nprog; i++)
        if (&g_prog[i] == g_pick_prog) set_prog_drive_c(g_pick_prog, path);
    fill_panel();
}

static void pick_folder(int why, int row, const char *title, const char *start);

// The panel's Drive C: list changed.
static void drive_c_chosen(void) {
    const struct prog *p = cur();
    int sel = uui_dropdown_selected(&g_ddc);
    if (!p || sel < 0 || sel >= g_dc_n) return;
    char dir[W16_PATH_MAX];
    switch (g_dc_kind[sel]) {
    case DC_DEFAULT:    set_prog_drive_c(p, 0); break;
    case DC_ITS_FOLDER: prog_dir(p, dir, sizeof dir); set_prog_drive_c(p, dir); break;
    case DC_CHOOSE:
        g_pick_prog = p;
        prog_dir(p, dir, sizeof dir);
        pick_folder(6, -1, "Choose the program's drive C:", dir);   // PICK_PROG_C
        break;
    default: break;
    }
    fill_panel();
}

// --- actions --------------------------------------------------------------------

static void run(const char *path) {
    if (!path) return;
    uapp_spawn(g_app, W16_BIN, path);
}

// The calls window, its own process: `retrolauncher --calls PATH`.
static void show_calls(const char *path) {
    char *args[] = { "retrolauncher", "--calls", (char *)path, 0 };
    sys_spawn_argv("/bin/wm/apps/retrolauncher", args);
}

// --- the folders: a list kept in order, changed in place or in Options ------------

// `path` added at the end of `*f`; its index, or -1. Already there: that one.
static int folders_add(struct w16_folder **f, int *n, const char *path) {
    for (int i = 0; i < *n; i++)
        if (!strcmp((*f)[i].path, path)) return i;
    struct w16_folder *g = realloc(*f, sizeof **f * (size_t)(*n + 1));
    if (!g) return -1;
    *f = g;
    g[*n].path = strdup(path);
    g[*n].name = strdup("");
    if (!g[*n].path || !g[*n].name) { free(g[*n].path); free(g[*n].name); return -1; }
    return (*n)++;
}

static void folders_remove(struct w16_folder *f, int *n, int i) {
    if (i < 0 || i >= *n) return;
    free(f[i].path);
    free(f[i].name);
    memmove(&f[i], &f[i + 1], sizeof *f * (size_t)(*n - i - 1));
    (*n)--;
}

// Row `i` one place up (by -1) or down (+1); where it went, or `i`.
static int folders_move(struct w16_folder *f, int n, int i, int by) {
    int j = i + by;
    if (i < 0 || j < 0 || i >= n || j >= n) return i;
    struct w16_folder t = f[i];
    f[i] = f[j];
    f[j] = t;
    return j;
}

static void set_owned(char **to, const char *s) {
    char *c = strdup(s);
    if (!c) return;
    free(*to);
    *to = c;
}

// A folder's .exe files, for a list's Programs column.
static int count_programs(const char *dir) {
    struct sys_dirent *ents;
    int n = ulistdir(dir, &ents), k = 0;
    for (int i = 0; i < n; i++) k += is_exe(ents[i].name);
    free(n >= 0 ? ents : 0);
    return k;
}

// The launcher's folders changed: saved, and everything shown from them redone.
static void folders_changed(int select) {
    w16_folders_save(g_folders, g_nfolders);
    find_programs();
    build_sidebar();
    uui_sidebar_select_id(&g_side, select);
    build_view();
    fill_panel();
}

// Which folder a Folder command is for: the one right-clicked, else the
// sidebar's choice (-1 for All programs).
static int g_ctx_folder = -1;
static int target_folder(void) { return g_ctx_folder >= 0 ? g_ctx_folder : uui_sidebar_selected_id(&g_side); }

// --- choosing a folder, for whichever asked ----------------------------------------

enum pick { PICK_ADD, PICK_CHANGE, PICK_OPT_ADD, PICK_OPT_CHANGE, PICK_DRIVE_ADD, PICK_DRIVE_CHANGE, PICK_PROG_C };
_Static_assert(PICK_PROG_C == 6, "drive_c_chosen() names it by number, being above the enum");
static int g_pick;
static int g_pick_row;                       // the row a Change is for
static struct uui_nameask g_ask;             // Rename...

static void opt_folders_changed(int select);
static void opt_drives_changed(int select);
static struct w16_folder *g_ofold;           // Options' copy of the folders, until OK
static int g_nofold;
static struct w16_prefs g_edit;
static char g_drive_rows[W16_DRIVES + 2];    // Options' Drives list: its letters, C: to Z:
static int g_ndrive_rows;

static char *drive_slot(struct w16_prefs *p, char d) {
    return d == 'C' ? p->drive_c : d >= 'D' && d < 'D' + W16_DRIVES ? p->drive[d - 'D'] : 0;
}

static void chosen(void *ctx, const char *path) {
    (void)ctx;
    if (!path) return;
    switch (g_pick) {
    case PICK_ADD: {
        int i = folders_add(&g_folders, &g_nfolders, path);
        if (i >= 0) folders_changed(i);
        break;
    }
    case PICK_CHANGE:
        if (g_pick_row >= 0 && g_pick_row < g_nfolders) {
            set_owned(&g_folders[g_pick_row].path, path);
            folders_changed(g_pick_row);
        }
        break;
    case PICK_OPT_ADD: opt_folders_changed(folders_add(&g_ofold, &g_nofold, path)); break;
    case PICK_OPT_CHANGE:
        if (g_pick_row >= 0 && g_pick_row < g_nofold) set_owned(&g_ofold[g_pick_row].path, path);
        opt_folders_changed(g_pick_row);
        break;
    case PICK_DRIVE_ADD:
        for (char d = 'D'; d < 'D' + W16_DRIVES; d++) {
            char *slot = drive_slot(&g_edit, d);
            if (slot[0]) continue;
            snprintf(slot, W16_PATH_MAX, "%s", path);
            opt_drives_changed(d);
            break;
        }
        break;
    case PICK_DRIVE_CHANGE:
        if (g_pick_row >= 0 && g_pick_row < g_ndrive_rows) {
            char d = g_drive_rows[g_pick_row], *slot = drive_slot(&g_edit, d);
            if (slot) snprintf(slot, W16_PATH_MAX, "%s", path);
            opt_drives_changed(d);
        }
        break;
    case PICK_PROG_C: prog_drive_c(path); break;
    }
    uapp_redraw(g_app);
}

static void pick_folder(int why, int row, const char *title, const char *start) {
    if (uui_filedialog_is_open(&g_chooser)) return;
    g_pick = why;
    g_pick_row = row;
    struct uui_filedialog_opts o = { .mode = UUI_FILEDIALOG_FOLDER, .title = title,
                                     .start_dir = start && start[0] ? start : "/home" };
    uui_filedialog_open(g_app, &g_chooser, &o, chosen, g_app);
}

// Rename...: the launcher's folder, or Options' copy of it.
static int g_rename_opt;
static void renamed(void *ctx, const char *name) {
    (void)ctx;
    if (g_rename_opt) {
        if (g_pick_row >= 0 && g_pick_row < g_nofold) set_owned(&g_ofold[g_pick_row].name, name);
        opt_folders_changed(g_pick_row);
    } else if (g_pick_row >= 0 && g_pick_row < g_nfolders) {
        set_owned(&g_folders[g_pick_row].name, name);
        folders_changed(g_pick_row);
    }
    uapp_redraw(g_app);
}

static void rename_folder(int opt, int row) {
    const struct w16_folder *f = opt ? &g_ofold[row] : &g_folders[row];
    if (uui_nameask_is_open(&g_ask)) return;
    g_rename_opt = opt;
    g_pick_row = row;
    uui_nameask_open(&g_ask, g_app, "Rename folder", "Shown in the list as (empty: the folder's own name)",
                     folder_label(f), "Rename", 0, renamed, 0);
}

// --- Options: the defaults ----------------------------------------------------------

static const struct uui_sidebar_row OPT_PAGES[] = {
    { "General", UUI_SIDEBAR_TOP, 0, 0 },
    { "Display", UUI_SIDEBAR_TOP, 0, 1 },
    { "Sound",   UUI_SIDEBAR_TOP, 0, 2 },
    { "Drives",  UUI_SIDEBAR_TOP, 0, 3 },
    { "Folders", UUI_SIDEBAR_TOP, 0, 4 },
    { "Help",    UUI_SIDEBAR_TOP, 0, 5 },
};
static const char *const FAULT_NAMES[] = { "Show the Error box", "Write the report only" };
static const char *const FROM_NAMES[] = { "Run it", "Ask if it may not work", "Always ask", "Show it here" };
static const char *const OPT_NOTES[] = {
    "What programs are told, what happens when one fails, and what opening one in the File Manager does.",
    "How a program's windows look, and whose fonts its text is drawn in.",
    "What a program plays, and whose sounds its events use.",
    "Each drive letter a program sees, and the folder it is here. A program's INI files go in C:\\WINDOWS.",
    "The folders programs are listed from, in the sidebar's order.",
    "What opens when a program asks for its Help file.",
};

// The Drives list: C:, the drives set, then Z:; what each holds, worked
// out when the list changes rather than as it paints.
static struct uui_listedit g_drives;
static char g_drive_holds[W16_DRIVES + 2][64];
static const struct uui_table_column DRIVE_COLS[] = {
    { "Drive", 6, UUI_TALIGN_LEFT, 0, 0 }, { "Folder", 0, UUI_TALIGN_LEFT, 0, 0 }, { "Holds", 24, UUI_TALIGN_LEFT, 0, 0 },
};

static int has_ci(const char *dir, const char *name, int *fonts) {
    struct sys_dirent *ents;
    int n = ulistdir(dir, &ents), found = 0;
    for (int i = 0; i < n; i++) {
        if (name && !strcasecmp(ents[i].name, name)) found = 1;
        size_t l = strlen(ents[i].name);
        if (fonts && l > 4 && !strcasecmp(ents[i].name + l - 4, ".fon")) (*fonts)++;
    }
    free(n >= 0 ? ents : 0);
    return found;
}

// A drive's Holds column, short; its card (drive_details) says the rest.
static void drive_holds(char d, char *out, size_t cap) {
    const char *root = d == 'Z' ? "/" : drive_slot(&g_edit, d);
    if (d == 'Z') { snprintf(out, cap, "all of toy-os"); return; }
    int n = count_programs(root);
    char sys[W16_PATH_MAX + 32];
    snprintf(sys, sizeof sys, "%s/WINDOWS/SYSTEM", root);
    int fonts = 0, dll = d == 'C' && has_ci(sys, "COMMDLG.DLL", &fonts);
    snprintf(out, cap, "%s%d program%s", dll || fonts ? "Windows files, " : "", n, n == 1 ? "" : "s");
}

// The card under the list for the selected drive: its folder, the programs
// in it by name, and for C: what of Windows 3.x's own files it holds.
// Built when the list changes or the selection moves, not as it paints.
static char *g_drive_card;
static int g_drive_card_row = -2;

static void card_add(char **t, size_t *n, size_t *cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void card_add(char **t, size_t *n, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(0, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return;
    if (*n + (size_t)need + 1 > *cap) {
        size_t c = (*n + (size_t)need + 1) * 2;
        char *g = realloc(*t, c);
        if (!g) return;
        *t = g;
        *cap = c;
    }
    va_start(ap, fmt);
    vsnprintf(*t + *n, *cap - *n, fmt, ap);
    va_end(ap);
    *n += (size_t)need;
}

static const char *drive_details(void *ctx, int row) {
    (void)ctx;
    if (row == g_drive_card_row && g_drive_card) return g_drive_card;
    free(g_drive_card);
    g_drive_card = 0;
    g_drive_card_row = row;
    if (row < 0 || row >= g_ndrive_rows) return 0;
    char d = g_drive_rows[row];
    const char *root = d == 'Z' ? "/" : drive_slot(&g_edit, d);
    size_t n = 0, cap = 0;
    char *t = 0;
    card_add(&t, &n, &cap, "Drive %c:\t%s\n", d, d == 'Z' ? "/ (all of toy-os)" : root);
    if (d != 'Z') {
        // The programs, by name, as many as there are.
        struct sys_dirent *ents;
        int k = ulistdir(root, &ents), np = 0;
        card_add(&t, &n, &cap, "Programs\t");
        for (int i = 0; i < k; i++)
            if (is_exe(ents[i].name)) card_add(&t, &n, &cap, "%s%s", np++ ? ", " : "", ents[i].name);
        if (!np) card_add(&t, &n, &cap, "none");
        card_add(&t, &n, &cap, "\n");
        free(k >= 0 ? ents : 0);
    }
    if (d == 'C') {
        char sys[W16_PATH_MAX + 32];
        snprintf(sys, sizeof sys, "%s/WINDOWS/SYSTEM", root);
        int fonts = 0, dll = has_ci(sys, "COMMDLG.DLL", &fonts);
        card_add(&t, &n, &cap, "WINDOWS folder\t%s\n", has_ci(root, "WINDOWS", 0)
                 ? "Found -- programs' INI files go here" : "Not yet -- made when a program first runs");
        card_add(&t, &n, &cap, "COMMDLG.DLL\t%s\n", dll ? "Found -- the Open dialog's own pictures"
                                                            : "Missing -- toy-os's own pictures stand in");
        if (fonts) card_add(&t, &n, &cap, "Fonts\t%d in WINDOWS\\SYSTEM\n", fonts);
        else card_add(&t, &n, &cap, "Fonts\tNone -- toy-os's stand-ins draw the text\n");
    }
    card_add(&t, &n, &cap, "Programs may\t%s", d == 'Z' ? "read everything, and write under /home"
                                         : !strncmp(root, "/home/", 6) || !strcmp(root, "/home") || d == 'C'
                                         ? "read and write here" : "only read here (it is outside /home)");
    g_drive_card = t;
    return t;
}

static void drive_cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (row < 0 || row >= g_ndrive_rows) { out[0] = 0; return; }
    char d = g_drive_rows[row];
    if (col == 0) snprintf(out, (size_t)cap, "%c:", d);
    else if (col == 1) snprintf(out, (size_t)cap, "%s", d == 'Z' ? "/" : drive_slot(&g_edit, d));
    else snprintf(out, (size_t)cap, "%s", g_drive_holds[row]);
}

static void opt_drives_changed(int select) {
    int at = -1;
    g_ndrive_rows = 0;
    for (char d = 'C'; d <= 'Z'; d++) {
        const char *slot = d == 'Z' ? "/" : drive_slot(&g_edit, d);
        if (!slot || (d != 'C' && !slot[0])) continue;
        if (d == select) at = g_ndrive_rows;
        drive_holds(d, g_drive_holds[g_ndrive_rows], sizeof g_drive_holds[0]);
        g_drive_rows[g_ndrive_rows++] = d;
    }
    uui_listedit_set_rows(&g_drives, g_ndrive_rows);
    if (at >= 0) uui_listedit_select(&g_drives, at);
    g_drive_card_row = -2;                       // the card is redone for what the list now says
    uui_prefs_redraw();
}

static int drive_enabled(void *ctx, int code, int row) {
    (void)ctx;
    char d = row >= 0 && row < g_ndrive_rows ? g_drive_rows[row] : 0;
    switch (code) {
    case ID_DRV_ADD:
        for (char k = 'D'; k < 'D' + W16_DRIVES; k++) if (!drive_slot(&g_edit, k)[0]) return 1;
        return 0;
    case ID_DRV_CHANGE: return d && d != 'Z';
    case ID_DRV_REMOVE: return d && d != 'C' && d != 'Z';
    default: return 0;
    }
}

// The Folders list: Options' copy of the launcher's.
static struct uui_listedit g_ofolders;
static int *g_ofold_count;                   // each folder's programs, counted when the list changes
static const struct uui_table_column FOLDER_COLS[] = {
    { "Name", 16, UUI_TALIGN_LEFT, 0, 0 }, { "Folder", 0, UUI_TALIGN_LEFT, 0, 0 }, { "Programs", 9, UUI_TALIGN_RIGHT, 0, 0 },
};

static void folder_cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (row < 0 || row >= g_nofold) { out[0] = 0; return; }
    if (col == 0) snprintf(out, (size_t)cap, "%s", folder_label(&g_ofold[row]));
    else if (col == 1) snprintf(out, (size_t)cap, "%s", g_ofold[row].path);
    else snprintf(out, (size_t)cap, "%d", g_ofold_count ? g_ofold_count[row] : 0);
}

static void opt_folders_changed(int select) {
    int *c = realloc(g_ofold_count, sizeof *c * (size_t)(g_nofold ? g_nofold : 1));
    if (c) {
        g_ofold_count = c;
        for (int i = 0; i < g_nofold; i++) c[i] = count_programs(g_ofold[i].path);
    }
    uui_listedit_set_rows(&g_ofolders, g_nofold);
    if (select >= 0) uui_listedit_select(&g_ofolders, select);
    uui_prefs_redraw();
}

static int folder_enabled(void *ctx, int code, int row) {
    (void)ctx;
    int on = row >= 0 && row < g_nofold;
    switch (code) {
    case ID_FOL_ADD:  return 1;
    case ID_FOL_UP:   return on && row > 0;
    case ID_FOL_DOWN: return on && row < g_nofold - 1;
    default:          return on;
    }
}

static void opt_copy_folders(const struct w16_folder *from, int n) {
    w16_folders_free(g_ofold, g_nofold);
    g_ofold = 0;
    g_nofold = 0;
    for (int i = 0; i < n; i++) {
        int k = folders_add(&g_ofold, &g_nofold, from[i].path);
        if (k >= 0) set_owned(&g_ofold[k].name, from[i].name);
    }
}

static void opt_defaults(void) {
    static const struct w16_folder DEF[] = { { "/usr/share/w16", "" }, { "/home/Win16", "" } };
    opt_copy_folders(DEF, 2);
    opt_folders_changed(-1);
    opt_drives_changed(-1);
    retro_sounds_mode(g_edit.sounds);
}

static void opt_action(int code) {
    int row = uui_listedit_selected(&g_drives), fr = uui_listedit_selected(&g_ofolders);
    switch (code) {
    case ID_DRV_ADD: pick_folder(PICK_DRIVE_ADD, -1, "Choose the folder for a new drive", "/home"); break;
    case ID_DRV_CHANGE:
        if (row >= 0 && row < g_ndrive_rows)
            pick_folder(PICK_DRIVE_CHANGE, row, "Choose the drive's folder", drive_slot(&g_edit, g_drive_rows[row]));
        break;
    case ID_DRV_REMOVE:
        if (row >= 0 && row < g_ndrive_rows && g_drive_rows[row] != 'C' && g_drive_rows[row] != 'Z') {
            drive_slot(&g_edit, g_drive_rows[row])[0] = 0;
            opt_drives_changed(-1);
        }
        break;
    case ID_FOL_ADD:    pick_folder(PICK_OPT_ADD, -1, "Add a folder of programs", "/home"); break;
    case ID_FOL_CHANGE: if (fr >= 0) pick_folder(PICK_OPT_CHANGE, fr, "Choose the folder", g_ofold[fr].path); break;
    case ID_FOL_RENAME: if (fr >= 0) rename_folder(1, fr); break;
    case ID_FOL_REMOVE: folders_remove(g_ofold, &g_nofold, fr); opt_folders_changed(fr < g_nofold ? fr : g_nofold - 1); break;
    case ID_FOL_UP:     opt_folders_changed(folders_move(g_ofold, g_nofold, fr, -1)); break;
    case ID_FOL_DOWN:   opt_folders_changed(folders_move(g_ofold, g_nofold, fr, +1)); break;
    default: break;
    }
}

static void opt_ok(void) {
    static struct w16_prefs was;
    was = g_def;
    g_def = g_edit;
    uprefs_save(&w16_prefs_defaults, &g_def, &was);
    w16_folders_free(g_folders, g_nfolders);
    g_folders = 0;
    g_nfolders = 0;
    for (int i = 0; i < g_nofold; i++) {
        int k = folders_add(&g_folders, &g_nfolders, g_ofold[i].path);
        if (k >= 0) set_owned(&g_folders[k].name, g_ofold[i].name);
    }
    folders_changed(-1);
    uapp_redraw(g_app);
}

static void edit_defaults(void *edit) { uprefs_defaults(&w16_prefs_defaults, edit); }

// The Sound page's table follows the Sounds choice as it changes, and its
// Play cells play here.
static void opt_widget(int id, int reason) {
    (void)reason;
    retro_sounds_mode(uui_prefs_current(offsetof(struct w16_prefs, sounds), g_edit.sounds));
    if (id == ID_SOUNDS) retro_sounds_play(uui_table_take_action(&g_retro_sounds, 0));
}

// Is there a WINHELP.EXE on drive C:? Looked for as Windows looked: in WINDOWS, then the root -- the
// names matched without case, as a 3.11 set copied off a case-sensitive disk has them either way.
static int winhelp_on_c(const char *drive_c) {
    static const char *const DIRS[] = { "/WINDOWS", "" };
    for (unsigned i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++) {
        char dir[W16_PATH_MAX + 16];
        snprintf(dir, sizeof dir, "%s%s", drive_c, DIRS[i]);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (!strcasecmp(e->d_name, "winhelp.exe")) { closedir(d); return 1; }
        closedir(d);
    }
    return 0;
}

static struct uui_label g_help_note;
static char g_help_note_text[200];

static void help_note_set(void) {
    snprintf(g_help_note_text, sizeof g_help_note_text, "%s",
             winhelp_on_c(g_def.drive_c)
                 ? "WINHELP.EXE is on drive C:. Chosen, it runs under w16 in place of the Help Viewer."
                 : "WINHELP.EXE is not on drive C: (WINDOWS or the root). The Help Viewer opens a program's Help either way.");
    uui_label_init(&g_help_note, g_help_note_text);
    g_help_note.dim = 1;
    g_help_note.wrap = 1;
    g_help_note.rows = 3;
}

static void open_options(void) {
    g_edit = g_def;
    static const struct uui_prefs_desc d = {
        .title = "Retro Launcher Options", .pages = OPT_PAGES, .page_count = 6,
        .log_prefix = "retrolauncher options", .on_defaults = opt_defaults, .on_ok = opt_ok, .on_action = opt_action,
        .on_widget = opt_widget,
        .notes = OPT_NOTES, .edit = &g_edit, .edit_defaults = edit_defaults,
    };
    uui_prefs_begin(&d);
    uui_prefs_choice(0, "Windows version", VERSION_NAMES, 0, 3, offsetof(struct w16_prefs, version), "version");
    uui_prefs_choice(0, "When a program fails", FAULT_NAMES, 0, 2, offsetof(struct w16_prefs, on_fault), "on_fault");
    uui_prefs_check(0, "Debugging", "Trace every program's calls to a file", offsetof(struct w16_prefs, trace), "trace");
    uui_prefs_choice(0, "From the File Manager", FROM_NAMES, 0, 4, offsetof(struct w16_prefs, from_files), "from_files");
    uui_prefs_check(0, "", "Add a program's folder here the first time it runs from elsewhere",
                    offsetof(struct w16_prefs, add_folder), "add_folder");
    uui_prefs_choice(1, "Scale", SCALE_NAMES, 0, W16_SCALE_COUNT, offsetof(struct w16_prefs, scale), "scale");
    uui_prefs_choice(1, "Scaled by", METHOD_NAMES, 0, 3, offsetof(struct w16_prefs, method), "method");
    uui_prefs_choice(1, "Window frame", FRAME_NAMES, 0, 2, offsetof(struct w16_prefs, frame), "frame");
    // 3.11's own fonts, where drive C: has them: what w16 found, asked of it now.
    uui_prefs_choice(1, "Fonts", FONTS_NAMES, 0, 2, offsetof(struct w16_prefs, fonts), "fonts");
    retro_fontcard_load(&g_retro_fontcard);
    uui_prefs_row(1, "", &retro_fontcard_ops, &g_retro_fontcard, ID_OPT_FONTCARD, "fontcard", UUI_FILL_W);
    uui_prefs_choice(2, "Sounds", SOUNDS_NAMES, 0, 3, offsetof(struct w16_prefs, sounds), "sounds");
    uui_prefs_check(2, "Old sound driver", "Play SOUND.DRV notes (Minesweeper's)",
                    offsetof(struct w16_prefs, sound_drv), "sound_drv");
    retro_sounds_load(g_edit.sounds);
    uui_prefs_row(2, "", &uui_table_ops, &g_retro_sounds, ID_SOUNDS, "sound-table", UUI_FILL_W | UUI_FILL_H);
    opt_drives_changed('C');
    uui_prefs_row(3, "", &uui_listedit_ops, &g_drives, 0, "drives", UUI_FILL_W | UUI_FILL_H);
    opt_copy_folders(g_folders, g_nfolders);
    uui_listedit_select(&g_ofolders, -1);
    opt_folders_changed(-1);
    uui_prefs_row(4, "", &uui_listedit_ops, &g_ofolders, 0, "folder-list", UUI_FILL_W | UUI_FILL_H);
    uui_prefs_choice(5, "Help opens in", HELP_NAMES, 0, 2, offsetof(struct w16_prefs, help), "help");
    help_note_set();
    uui_prefs_row(5, "", &uui_label_ops, &g_help_note, 0, "help-note", UUI_FILL_W);
    uui_prefs_open(g_app);
}

static void command(struct uapp *a, int cmd) {
    struct prog *p = cur();
    switch (cmd) {
    case CMD_RUN:     if (p && p->ok) run(p->path); break;
    case CMD_CALLS:   if (p && p->ok) show_calls(p->path); break;
    case CMD_PROPS:   g_panel = !g_panel; break;
    case CMD_ADD:    pick_folder(PICK_ADD, -1, "Add a folder of programs", "/home"); break;
    case CMD_CHANGE: {
        int f = target_folder();
        if (f >= 0 && f < g_nfolders) pick_folder(PICK_CHANGE, f, "Choose the folder", g_folders[f].path);
        break;
    }
    case CMD_RENAME: { int f = target_folder(); if (f >= 0 && f < g_nfolders) rename_folder(0, f); break; }
    case CMD_UP: case CMD_DOWN: {
        int f = target_folder();
        if (f >= 0) folders_changed(folders_move(g_folders, g_nfolders, f, cmd == CMD_UP ? -1 : 1));
        break;
    }
    case CMD_REMOVE: {
        int f = target_folder();
        if (f < 0) break;
        folders_remove(g_folders, &g_nfolders, f);
        folders_changed(-1);
        break;
    }
    case CMD_REFRESH: find_programs(); build_view(); break;
    case CMD_OPTIONS: open_options(); break;
    case CMD_EXIT:    uapp_quit(a, 0); break;
    default: break;
    }
    g_ctx_folder = -1;
    fill_panel();
}

// --- drawing --------------------------------------------------------------------------

static void tile(struct ugfx_surface *s, void *ctx, int index, int x, int y, int w, int h, int state) {
    (void)ctx;
    (void)h;
    const struct prog *p = &g_prog[g_view[index]];
    int sel = state & UUI_GRID_SELECTED, lh = ugfx_char_h();
    uint32_t ink = sel ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, ground = sel ? UTHEME_ACCENT : uui_grid_c_bg(&g_grid);
    int ix = x + (w - 32) / 2, iy = y + lh / 2;
    if (p->iw) ugfx_blit_alpha(s, ix + (32 - p->iw) / 2, iy + (32 - p->ih) / 2, p->iw, p->ih, p->icon, p->iw);
    else {
        const struct uimg *ico = icon_get("win16", 32);
        if (ico) ugfx_blit_alpha(s, ix, iy, ico->w, ico->h, ico->px, ico->w);
    }
    // The how-far badge: red when it last stopped, green when it ran to
    // the end last time or w16 has every call in full, amber when some are
    // stand-ins, grey when some are missing.
    int missing = p->imports - p->has;
    uint32_t badge = !p->checked ? UTHEME_OUTLINE
                   : p->last == 2 ? utheme_severity(UTHEME_SEV_ERROR)
                   : p->last == 1 || (!missing && !p->partly) ? utheme_action(UTHEME_ACT_CREATE)
                   : missing ? uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED)
                   : utheme_severity(UTHEME_SEV_WARNING);
    ugfx_fill_rect(s, ix + 28, iy + 28, 9, 9, ground);
    ugfx_fill_rect(s, ix + 29, iy + 29, 7, 7, badge);
    int ty = iy + 32 + lh / 3;
    int tw = ugfx_text_width(str(p->title));
    ugfx_draw_string_clipped(s, x + (tw < w ? (w - tw) / 2 : 2), ty, w - 4, str(p->title), ink, ground);
    char st[48];
    if (!p->checked) snprintf(st, sizeof st, "Checking...");
    else if (p->last == 2) snprintf(st, sizeof st, "Stopped last time");
    else if (missing) snprintf(st, sizeof st, p->last == 1 ? "Ran last time" : "%d missing", missing);
    else if (p->partly) snprintf(st, sizeof st, "Runs \xb7 %d partly", p->partly);
    else snprintf(st, sizeof st, "Runs");
    int sw = ugfx_text_width(st);
    ugfx_draw_string_clipped(s, x + (sw < w ? (w - sw) / 2 : 2), ty + lh, w - 4, st,
                             sel ? ink : uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), ground);
}

static void line(struct ugfx_surface *s, int x, int *y, int w, const char *t, uint32_t col) {
    ugfx_draw_string_clipped(s, x, *y, w, t, col, UTHEME_PANEL_BG);
    *y += ugfx_char_h() + 3;
}

static int g_dd_y[NSET], g_ddc_y;
static struct uui_stackbar g_bar;            // works, partly, missing

// Each "Partly there" row's Why? link, where it was last drawn: one x
// span, a y per row (allocated to the rows); the press that armed one.
static int g_why_x, g_why_w, *g_why_y, g_why_n, g_why_armed = -1;

// THE PANEL SCROLLS ABOVE A FOOTER that holds Run and the calls button --
// the "Retro Launcher details scroll" canvas's B. It is drawn by hand, its
// dropdowns placed over it, so the scrolling is the panel's own: an
// offset, a clip, the shared bar (uui_sbar), and a dropdown shown only
// while all of it is in view.
static int g_panel_for = -1;                 // the grid row the panel was last scrolled for
static int g_pscroll, g_pcontent_h;          // pixels scrolled; how tall the content was last drawn
static struct uui_sbar g_psb;

static int footer_h(void) { return ugfx_char_h() + ugfx_char_w() * 3; }
static int view_y(void) { return g_py; }
static int view_h(void) { return g_ph - footer_h(); }
static int row_h(void) { return ugfx_char_h() + ugfx_char_w(); }
static int scroll_max(void) { return g_pcontent_h > view_h() ? g_pcontent_h - view_h() : 0; }

static void bold_line(struct ugfx_surface *s, int x, int *y, int w, const char *t, uint32_t col) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    line(s, x, y, w, t, col);
    ugfx_set_font(was);
}

// The scrolling part: what the program is, its calls, its last run, its settings.
static void draw_details(struct ugfx_surface *s, const struct prog *p, int x, int y0, int w) {
    int pad = ugfx_char_w(), lh = ugfx_char_h(), y = y0;
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    char t[160];
    bold_line(s, x, &y, w, str(p->name), UTHEME_TEXT);
    line(s, x, &y, w, p->path, dim);
    if (p->ok) {
        snprintf(t, sizeof t, "Module %s", str(p->module));
        line(s, x, &y, w, t, dim);
        y += lh / 2;
        snprintf(t, sizeof t, "The %d calls it imports", p->imports);
        bold_line(s, x, &y, w, t, utheme_action(UTHEME_ACT_EDIT));
        int missing = p->imports - p->has, bh;
        uui_stackbar_set(&g_bar, 0, (uint64_t)(p->has - p->partly), 0);
        uui_stackbar_set(&g_bar, 1, (uint64_t)p->partly, 0);
        uui_stackbar_set(&g_bar, 2, (uint64_t)missing, 0);
        snprintf(g_bar.seg[0].value, sizeof g_bar.seg[0].value, "%d", p->has - p->partly);
        snprintf(g_bar.seg[1].value, sizeof g_bar.seg[1].value, "%d", p->partly);
        snprintf(g_bar.seg[2].value, sizeof g_bar.seg[2].value, "%d", missing);
        g_bar.seg[0].color = utheme_action(UTHEME_ACT_CREATE);
        g_bar.seg[1].color = utheme_severity(UTHEME_SEV_WARNING);
        g_bar.seg[2].color = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
        uui_stackbar_natural_size(&g_bar, &(int){ 0 }, &bh);
        g_bar.x = x; g_bar.y = y; g_bar.w = w; g_bar.h = bh;
        uui_stackbar_draw(s, &g_bar);
        y += bh + lh / 2;
        for (int i = 0; i < p->nmissing; i++) {
            snprintf(t, sizeof t, "Missing: %s%s", str(p->missing[i]),
                     i == p->nmissing - 1 && missing > p->nmissing ? ", ..." : "");
            line(s, x, &y, w, t, dim);
        }
        // What w16 answers without doing, every one: the panel scrolls.
        if (p->npart) {
            y += lh / 3;
            bold_line(s, x, &y, w, "Partly there", utheme_action(UTHEME_ACT_EDIT));
            int nw = w * 2 / 5, ww = ugfx_text_width("Why?");
            int *gy = realloc(g_why_y, sizeof *g_why_y * (size_t)p->npart);
            if (gy) g_why_y = gy;
            g_why_n = gy ? p->npart : 0;
            g_why_x = x + w - ww;
            g_why_w = ww;
            for (int i = 0; i < p->npart; i++) {
                bold_line(s, x, &(int){ y }, nw - pad, str(p->part[i].names), UTHEME_TEXT);
                uint32_t link = g_why_armed == i ? UTHEME_TEXT : UTHEME_ACCENT;
                ugfx_draw_string_clipped(s, g_why_x, y, ww, "Why?", link, UTHEME_PANEL_BG);
                ugfx_fill_rect(s, g_why_x, y + lh - 1, ww, 1, link);
                if (i < g_why_n) g_why_y[i] = y;
                line(s, x + nw, &y, w - nw - ww - pad, str(p->part[i].note), dim);
            }
        }
        // Why it stopped last time; the footer says when.
        if (p->last == 2 && p->last_text && p->last_text[0]) {
            y += lh / 3;
            bold_line(s, x, &y, w, "Why it stopped", utheme_severity(UTHEME_SEV_ERROR));
            line(s, x, &y, w, p->last_text, dim);
        }
    } else if (p->checked) {
        line(s, x, &y, w, p->foreign ? p->foreign : "Not a Windows 3.x program w16 can read.", dim);
    }
    y += lh / 2;
    bold_line(s, x, &y, w, "This program's settings", utheme_action(UTHEME_ACT_EDIT));
    for (int i = 0; i <= NSET; i++) {
        if (i < NSET) g_dd_y[i] = y;
        else g_ddc_y = y;
        ugfx_draw_string_clipped(s, x, y + (row_h() - lh) / 2, w / 2, i < NSET ? SETTINGS[i].caption : "Drive C:",
                                 UTHEME_TEXT, UTHEME_PANEL_BG);
        y += row_h() + pad / 2;
    }
    g_pcontent_h = y - y0 + pad * 2;
}

// The footer's line beside its buttons: when it last ran.
static void last_run_text(const struct prog *p, char *t, size_t cap, uint32_t *col) {
    *col = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    if (!p->last) { snprintf(t, cap, "Not run yet"); return; }
    char when[32] = "";
    time_t tt = (time_t)p->last_when;
    struct tm *tm = localtime(&tt);
    if (tm) strftime(when, sizeof when, "%Y-%m-%d %H:%M", tm);
    snprintf(t, cap, "%s %s", p->last == 2 ? "Stopped" : "Ran", when);
    if (p->last == 2) *col = utheme_severity(UTHEME_SEV_ERROR);
}

// Run, then the calls button, in the footer: their widths, and where the
// line beside them starts.
static int run_w(void) { return ugfx_text_width("Run") + ugfx_char_w() * 6; }
static const char *calls_label(const struct prog *p) {
    return p && p->imports > p->has ? "Missing calls..." : "Calls it imports...";
}
static int calls_w(const struct prog *p) { return ugfx_text_width(calls_label(p)) + ugfx_char_w() * 4; }

static void draw_panel(struct ugfx_surface *s) {
    ugfx_fill_rect(s, g_px, g_py, g_pw, g_ph, UTHEME_PANEL_BG);
    ugfx_fill_rect(s, g_px, g_py, 1, g_ph, UTHEME_SEPARATOR);
    const struct prog *p = cur();
    int pad = ugfx_char_w(), x = g_px + pad * 2, w = g_pw - pad * 3 - uui_sbar_width();
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    if (!p) {
        int y = g_py + pad;
        line(s, x, &y, w, g_nview ? "Pick a program." : "No programs here. Add a folder of them.", dim);
        g_pcontent_h = 0;
        return;
    }
    if (g_pscroll > scroll_max()) g_pscroll = scroll_max();
    ugfx_set_clip_rect(s, g_px + 1, view_y(), g_pw - 1, view_h());
    draw_details(s, p, x, view_y() + pad - g_pscroll, w);
    ugfx_clear_clip_rect(s);
    if (g_pscroll > scroll_max()) {              // the content got shorter: once more, in range
        g_pscroll = scroll_max();
        uapp_redraw(g_app);
    }
    uui_sbar_place(&g_psb, g_px + g_pw - uui_sbar_width(), view_y(), view_h());
    uui_sbar_set(&g_psb, g_pcontent_h, view_h(), g_pscroll);
    uui_sbar_draw(&g_psb, s, UTHEME_PANEL_BG, UTHEME_TEXT);
    // The footer, pinned.
    int fy = g_py + g_ph - footer_h();
    ugfx_fill_rect(s, g_px + 1, fy, g_pw - 1, 1, UTHEME_SEPARATOR);
    if (p->ok) {
        char t[64];
        uint32_t col;
        last_run_text(p, t, sizeof t, &col);
        int tx = g_px + pad * 4 + run_w() + calls_w(p);
        ugfx_draw_string_clipped(s, tx, fy + (footer_h() - ugfx_char_h()) / 2, g_px + g_pw - pad - tx, t, col,
                                 UTHEME_PANEL_BG);
    }
}

// --- scrolling the panel: the wheel anywhere over it, and its bar ---------------

static void pscroll_to(int top) {
    g_pscroll = top < 0 ? 0 : top > scroll_max() ? scroll_max() : top;
}

static void ps_natural(const void *w, int *ow, int *oh) { (void)w; *ow = 0; *oh = 0; }
static void ps_geometry(void *w, int x, int y, int ww, int hh) { (void)w; (void)x; (void)y; (void)ww; (void)hh; }
static void ps_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    (void)w;
    *x = g_px; *y = view_y(); *ow = g_pw; *oh = view_h();
}
static void ps_draw(struct ugfx_surface *s, const void *w) { (void)s; (void)w; }   // draw_panel() did
static int ps_hit(const void *w, int cx, int cy) {
    (void)w;
    return cx >= g_px && cx < g_px + g_pw && cy >= view_y() && cy < view_y() + view_h();
}
// The "Partly there" row whose Why? is at (cx, cy), or -1.
static int why_at(int cx, int cy) {
    const struct prog *p = cur();
    if (!p || cx < g_why_x || cx >= g_why_x + g_why_w || cy < view_y() || cy >= view_y() + view_h()) return -1;
    for (int i = 0; i < g_why_n && i < p->npart; i++)
        if (cy >= g_why_y[i] - 2 && cy < g_why_y[i] + ugfx_char_h() + 2) return i;
    return -1;
}

static int ps_press(void *w, int cx, int cy, unsigned mods) {
    (void)w; (void)mods;
    int r = uui_sbar_press(&g_psb, cx, cy);
    if (r & UUI_SBAR_MOVED) pscroll_to(g_psb.top);
    if (r & UUI_SBAR_TOOK) return 1;
    // A link arms on the press and acts on a release over it.
    g_why_armed = why_at(cx, cy);
    return g_why_armed >= 0;
}
static int ps_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)w;
    int r = uui_sbar_motion(&g_psb, cx, cy, buttons);
    if (r & UUI_SBAR_MOVED) pscroll_to(g_psb.top);
    return r != 0;
}
static int ps_release(void *w, int cx, int cy) {
    (void)w;
    int armed = g_why_armed;
    g_why_armed = -1;
    const struct prog *p = cur();
    if (armed >= 0 && p && armed < p->npart && why_at(cx, cy) == armed) {
        const struct partly *q = &p->part[armed];
        char module[16];
        const char *dot = strchr(q->key, '.');
        snprintf(module, sizeof module, "%.*s", dot ? (int)(dot - q->key) : 0, q->key);
        if (dot) retro_why_open(g_app, module, (unsigned)strtoul(dot + 1, 0, 10), q->first, 1);
        return 1;
    }
    return uui_sbar_release(&g_psb) != 0 || armed >= 0;
}
static int ps_wheel(void *w, int notches) {
    (void)w;
    if (!scroll_max()) return 0;
    pscroll_to(g_pscroll - notches * row_h() * 2);   // up is positive, as uui_listbox_wheel()
    return 1;
}
static const struct uui_widget_ops g_pscroll_ops = {
    .natural_size = ps_natural, .set_geometry = ps_geometry, .bounds = ps_bounds, .draw = ps_draw,
    .hit = ps_hit, .press = ps_press, .motion = ps_motion, .release = ps_release, .wheel = ps_wheel,
};
static int g_pscroller;                      // the widget is the panel's state above; this is its handle

static struct uui_menu_item program_items[] = {
    UUI_MENU("Run", CMD_RUN, "Enter"),
    UUI_MENU("Calls it imports...", CMD_CALLS, 0),
    UUI_MENU("Details panel", CMD_PROPS, "F4"),
    UUI_MENU("Refresh", CMD_REFRESH, "F5"),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
// The Folder menu, and a folder's right-click menu.
static struct uui_menu_item folder_items[] = {
    UUI_MENU("Add folder...", CMD_ADD, 0),
    UUI_MENU_SEP,
    UUI_MENU("Change folder...", CMD_CHANGE, 0),
    UUI_MENU("Rename...", CMD_RENAME, 0),
    UUI_MENU_SEP,
    UUI_MENU("Move up", CMD_UP, 0),
    UUI_MENU("Move down", CMD_DOWN, 0),
    UUI_MENU_SEP,
    UUI_MENU("Remove from the list", CMD_REMOVE, 0),
};
#define FOLDER_CTX_FIRST 2                   // the right-click menu leaves out Add folder...
static struct uui_menu_item tools_items[] = {
    UUI_MENU("Options...", CMD_OPTIONS, 0),
};
static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("Program", program_items),
    UUI_SUBMENU("Folder", folder_items),
    UUI_SUBMENU("Tools", tools_items),
};
static struct uui_toolbar_item tb_items[] = {
    { "tb-play",    "Run the program (Enter)",   CMD_RUN,     "Run",           0, "Enter", UTHEME_ACT_CREATE },
    UUI_TOOLBAR_SEP,
    { "tb-mkdir",   "Add a folder of programs",  CMD_ADD,     "Add folder...", 0, 0,       UTHEME_ACT_CREATE },
    { "tb-info",    "Details panel (F4)",        CMD_PROPS,   "Details",       0, "F4",    UTHEME_ACT_VIEW },
    { "tb-refresh", "Refresh (F5)",              CMD_REFRESH, 0,               0, "F5",    UTHEME_ACT_VIEW },
    UUI_TOOLBAR_SEP,
    { "tb-gear",    "Options for every program", CMD_OPTIONS, "Options...",    0, 0,       UTHEME_ACT_EDIT },
};

static unsigned item_flags(int code) {
    const struct prog *p = cur();
    if ((code == CMD_RUN || code == CMD_CALLS) && !(p && p->ok)) return UUI_MI_DISABLED;
    int f = target_folder();
    if ((code == CMD_REMOVE || code == CMD_CHANGE || code == CMD_RENAME) && f < 0) return UUI_MI_DISABLED;
    if ((code == CMD_UP && f <= 0) || (code == CMD_DOWN && (f < 0 || f >= g_nfolders - 1))) return UUI_MI_DISABLED;
    if (code == CMD_PROPS && g_panel) return UUI_MI_CHECKED;
    return 0;
}

static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,  .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,    .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_sidebar_ops,   .widget = &g_side,  .id = ID_SIDEBAR, .name = "folders" },
    { .ops = &uui_grid_ops,      .widget = &g_grid,  .id = ID_GRID,    .name = "programs" },
    // Under the dropdowns, so a click on one is theirs and the wheel over
    // them (a closed dropdown takes none) scrolls the panel.
    { .ops = &g_pscroll_ops,     .widget = &g_pscroller, .name = "details" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[0], .id = ID_SCALE,   .name = "scale" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[1], .id = ID_METHOD,  .name = "method" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[2], .id = ID_VERSION, .name = "version" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[3], .id = ID_FRAME,   .name = "frame" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[4], .id = ID_FONTS,   .name = "fonts" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[5], .id = ID_SOUND,   .name = "sound" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[6], .id = ID_HELP,    .name = "help" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[7], .id = ID_TRACE,   .name = "trace" },
    { .ops = &uui_dropdown_ops,  .widget = &g_ddc,   .id = ID_DRIVEC,  .name = "drive-c" },
    { .ops = &uui_button_ops,    .widget = &g_run,   .id = ID_RUN,     .name = "run" },
    { .ops = &uui_button_ops,    .widget = &g_calls, .id = ID_CALLS,   .name = "calls" },
    { .ops = &uui_statusbar_ops, .widget = &g_sb,    .id = ID_STATUS,  .name = "status" },
    { .ops = &uui_menubar_ops,   .widget = &g_ctx,   .id = ID_CTX,     .name = "ctxmenu" },
};
#define W_PSCROLL 4 // the panel's scrolling
#define W_PANEL 5   // then the NSET dropdowns, drive C:, Run, the calls button

static void layout(int cw, int ch) {
    int pad = ugfx_char_w();
    int mh = uui_menubar_height(&g_menu);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mh);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    int th = uui_toolbar_height(&g_tb);
    uui_toolbar_ops.set_geometry(&g_tb, 0, mh, cw, th);
    int sbh = uui_statusbar_height(&g_sb);
    uui_statusbar_set_geometry(&g_sb, 0, ch - sbh, cw, sbh);
    int top = mh + th + 1, sidew = pad * 20, body_h = ch - sbh - top;
    uui_sidebar_set_geometry(&g_side, 0, top, sidew, body_h);
    g_pw = g_panel ? pad * 34 : 0;
    g_px = cw - g_pw;
    g_py = top;
    g_ph = body_h;
    uui_grid_ops.set_geometry(&g_grid, sidew + 1, top, g_px - sidew - 1, body_h);
    // The settings rows exist only beside a program, each only while all of
    // it is in the panel's scrolled view; Run and the calls button are the
    // footer's.
    const struct prog *p = cur();
    int show = g_panel && p, ok = show && p->ok, rh = row_h();
    g_widgets[W_PSCROLL].hidden = !show;
    for (int i = 0; i <= NSET; i++) {
        int y = i < NSET ? g_dd_y[i] : g_ddc_y;
        g_widgets[W_PANEL + i].hidden = !show || y < view_y() || y + rh > view_y() + view_h();
    }
    g_widgets[W_PANEL + NSET + 1].hidden = !ok;
    g_widgets[W_PANEL + NSET + 2].hidden = !ok;
    if (!g_panel) return;
    int dx = g_px + pad * 2 + pad * 14, dw = g_pw - pad * 17 - uui_sbar_width();
    for (int i = 0; i < NSET; i++) uui_dropdown_set_geometry(&g_dd[i], dx, g_dd_y[i], dw, rh);
    uui_dropdown_set_geometry(&g_ddc, dx, g_ddc_y, dw, rh);
    int bw = run_w();
    int by = g_py + g_ph - footer_h() + (footer_h() - rh) / 2;
    uui_button_set_geometry(&g_run, g_px + pad * 2, by, bw, rh);
    g_run.disabled = !ok;
    // The missing calls, or the whole list when none are.
    g_calls.label = calls_label(p);
    uui_button_set_geometry(&g_calls, g_px + pad * 3 + bw, by, calls_w(p), rh);
}

static void status(void) {
    int runs = 0, need = 0, ok = 0;
    for (int i = 0; i < g_nprog; i++) {
        if (!g_prog[i].ok) continue;
        ok++;
        if (g_prog[i].last == 1 || g_prog[i].has == g_prog[i].imports) runs++;   // stand-ins run
        else need++;
    }
    snprintf(g_st_count, sizeof g_st_count, "%d programs", ok);
    snprintf(g_st_runs, sizeof g_st_runs, "%d ready", runs);
    snprintf(g_st_need, sizeof g_st_need, "%d %s more of Windows", need, need == 1 ? "needs" : "need");
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = d->surface;
    status();
    // Laid out on both sides of the panel: before, so a resized window's
    // panel is drawn where it now is, not the last frame's place; after,
    // because drawing it places the dropdown rows the boxes go on.
    layout(s->w, s->h);
    if (g_panel) draw_panel(s);
    layout(s->w, s->h);
    ugfx_fill_rect(s, 0, g_py - 1, s->w, 1, UTHEME_SEPARATOR);
    ugfx_fill_rect(s, g_side.w, g_py, 1, g_ph, UTHEME_SEPARATOR);
    uapp_log_layout(a, "retrolauncher");
    // Each Why? link, for a test to click (it is drawn, not a widget).
    for (int i = 0; g_panel && i < g_why_n; i++)
        uapp_logf_layout("retrolauncher: layout why %d %d %d %d %d\n", i, g_why_x, g_why_y[i], g_why_w,
                         ugfx_char_h());
}

// --- input --------------------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    int code;
    switch (id) {
    case ID_MENU:    if ((code = uui_menubar_take_code(&g_menu)) > 0) command(a, code); break;
    case ID_TOOLBAR: if ((code = uui_toolbar_take_code(&g_tb)) > 0) command(a, code); break;
    case ID_SIDEBAR: build_view(); fill_panel(); break;
    case ID_GRID:
        if (g_grid.selected != g_panel_for) { g_panel_for = g_grid.selected; g_pscroll = 0; }
        fill_panel();
        if ((code = uui_grid_take(&g_grid)) >= 0) command(a, CMD_RUN);
        break;
    case ID_SCALE: case ID_METHOD: case ID_VERSION: case ID_FRAME: case ID_FONTS: case ID_SOUND: case ID_HELP: case ID_TRACE:
        save_panel();
        break;
    case ID_DRIVEC: drive_c_chosen(); break;
    case ID_CTX:
        if ((code = uui_menubar_take_code(&g_ctx)) > 0) command(a, code);
        else if (!uui_menubar_is_open(&g_ctx)) g_ctx_folder = -1;
        break;
    default: break;
    }
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    if (code == ID_RUN) command(a, CMD_RUN);
    if (code == ID_CALLS) command(a, CMD_CALLS);
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int code;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code > 0) command(a, code);
        uapp_redraw(a);
        return;
    }
    switch (key) {
    case KEY_F4: command(a, CMD_PROPS); break;
    case KEY_F5: command(a, CMD_REFRESH); break;
    default: break;
    }
    uapp_redraw(a);
}

// A few programs a tick, so a big folder fills in rather than holding the
// window shut. 0 when there was nothing left to check.
static int on_tick(struct uapp *a) {
    int did = 0;
    for (int k = 0; k < 3 && g_next_check < g_nprog; k++, did++) check(&g_prog[g_next_check++]);
    if (did) {
        build_view();
        fill_panel();
        uapp_redraw(a);
    }
    return uui_toolbar_tick(&g_tb) | did;
}

static void consume_show(void);

// Back from a program: what it did is in its last-run record.
static void on_focus(struct uapp *a, int focused) {
    if (!focused) return;
    consume_show();
    for (int i = 0; i < g_nprog; i++) if (g_prog[i].checked) read_last(&g_prog[i]);
    uapp_redraw(a);
}

// A right-click on a folder in the sidebar: its menu, for that folder.
static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    if (!(buttons & 0x2)) return;
    if (uui_menubar_is_open(&g_ctx)) {
        uui_menubar_close(&g_ctx);
        g_ctx_folder = -1;
        uapp_redraw(a);
        return;
    }
    int row = uui_sidebar_hit(&g_side, x, y);
    if (row < 1 || row > g_nfolders) return;     // row 0 is All programs
    g_ctx_folder = g_rows[row].id;
    uui_menubar_open_at(&g_ctx, folder_items + FOLDER_CTX_FIRST,
                        (int)(sizeof folder_items / sizeof folder_items[0]) - FOLDER_CTX_FIRST, x, y);
    uapp_redraw(a);
}

static void on_open(struct uapp *a) { g_app = a; }

static void default_size(int *w, int *h) {
    *w = ugfx_char_w() * 110;
    *h = ugfx_char_h() * 40;
}

// --- from elsewhere: `retrolauncher FILE` (the File Manager's Open), --show, --details ----

// Asks the launcher -- the one open, or the one about to open -- to show
// `path`: a file it reads when it opens and whenever it gets the focus,
// since a second copy of it only raises the first (UAPP_SINGLE_INSTANCE).
#define SHOW_FILE W16_STATE_DIR "/show"
static void request_show(const char *path) {
    mkdir(W16_STATE_DIR, 0755);
    FILE *f = fopen(SHOW_FILE, "w");
    if (f) { fprintf(f, "%s\n", path); fclose(f); }
}

static void path_dir(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash) *(slash == out ? slash + 1 : slash) = 0;
}

// `path`'s folder in the launcher's list, added when it is not there; its index.
static int remember_folder(const char *path) {
    char dir[1024];
    path_dir(path, dir, sizeof dir);
    for (int i = 0; i < g_nfolders; i++)
        if (!strcmp(g_folders[i].path, dir)) return i;
    int i = folders_add(&g_folders, &g_nfolders, dir);
    if (i >= 0) w16_folders_save(g_folders, g_nfolders);
    return i;
}

// A pending --show: its folder listed and chosen, the program selected.
static void consume_show(void) {
    char *line = 0;
    size_t cap = 0;
    FILE *f = fopen(SHOW_FILE, "r");
    if (!f) return;
    ssize_t got = getline(&line, &cap, f);
    fclose(f);
    unlink(SHOW_FILE);
    if (got > 0) {
        line[strcspn(line, "\n")] = 0;
        int had = g_nfolders, k = remember_folder(line);
        if (k >= 0) {
            if (g_nfolders != had) folders_changed(k);
            else { uui_sidebar_select_id(&g_side, k); build_view(); }
            for (int i = 0; i < g_nview; i++)
                if (!strcmp(g_prog[g_view[i]].path, line)) g_grid.selected = i;
            fill_panel();
        }
    }
    free(line);
}

static void run_from_elsewhere(const char *path) {
    if (g_def.add_folder) remember_folder(path);
    char *args[] = { "w16", (char *)path, 0 };
    sys_spawn_argv(W16_BIN, args);
}

// `retrolauncher --details FILE`: the File Manager's rows for it (an X-Details
// command, lib/uopen.h), "Label<tab>value".
static int details_main(const char *path) {
    static struct prog p;
    p.path = strdup(path);
    if (!p.path) return 1;
    check(&p);
    int missing = p.imports - p.has;
    if (!p.ok) {
        printf("Program\t%s\n", p.foreign ? p.foreign : "not one w16 can read");
        printf("Runs\tNo\n");
    } else {
        printf("Program\tWindows 3.x, %s\n", str(p.module));
        if (missing) printf("Runs\tPartly: %d call%s missing\n", missing, missing == 1 ? "" : "s");
        else if (p.partly) printf("Runs\tYes, %d stand-in%s\n", p.partly, p.partly == 1 ? "" : "s");
        else printf("Runs\tYes\n");
    }
    prog_free(&p);
    return 0;
}

static void card_done(void *ctx, const char *path, int kind, int act) {
    (void)ctx;
    (void)kind;
    if (act != ULAUNCH_ASK && uui_runask_always()) {
        static struct w16_prefs was;
        was = g_def;
        g_def.from_files = W16_FROM_RUN;
        uprefs_save(&w16_prefs_defaults, &g_def, &was);
    }
    if (act == ULAUNCH_RUN) run_from_elsewhere(path);
    else if (act == ULAUNCH_SIDE1) show_calls(path);
    else if (act == ULAUNCH_SIDE2) {
        request_show(path);
        char *args[] = { "retrolauncher", 0 };
        sys_spawn_argv("/bin/wm/apps/retrolauncher", args);
    }
}

// `retrolauncher FILE`: what the File Manager's Open does with a program, by the
// setting -- run it, or first the run card (ui/uui_runask.h) saying what
// w16 lacks of it, or what it is when it is no Windows 3.x program at all.
// -1 when the launcher's own window should open and show it.
static int open_from_elsewhere(const char *path) {
    static struct prog p;
    p.path = strdup(path);
    if (!p.path) return 1;
    check(&p);
    int missing = p.imports - p.has;
    if (p.ok && (g_def.from_files == W16_FROM_RUN || (g_def.from_files == W16_FROM_CHECK && !missing))) {
        run_from_elsewhere(path);
        return 0;
    }
    if (p.ok && g_def.from_files == W16_FROM_SHOW) {
        request_show(path);
        return -1;
    }
    if (!ugfx_font_init()) return 2;
    static struct ulaunch_info info;
    info.kind = ULAUNCH_FOREIGN;
    info.runnable = 1;
    info.interp_found = p.ok;
    snprintf(info.app, sizeof info.app, "win16");
    if (!p.ok) {
        snprintf(info.what, sizeof info.what, "%s", p.foreign ? p.foreign : "a file w16 cannot read");
        snprintf(info.note, sizeof info.note, "w16 runs Windows 3.x programs only.");
    } else {
        snprintf(info.what, sizeof info.what, "a Windows 3.x program");
        if (missing)
            snprintf(info.note, sizeof info.note, "w16 lacks %d of the calls it uses, so it may stop partway.", missing);
        else
            snprintf(info.note, sizeof info.note, "w16 has every call it uses%s.", p.partly ? ", some as stand-ins" : "");
        int n = 0;
        if (p.nmissing) {
            char list[ULAUNCH_HEAD_COLS + 1] = "";
            for (int i = 0; i < p.nmissing; i++)
                snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", str(p.missing[i]));
            // A sample when there are more, and said to be one.
            if (missing > p.nmissing) {
                snprintf(info.head[n++], sizeof info.head[0], "Missing %d calls, among them: %s", missing, list);
                snprintf(info.head[n++], sizeof info.head[0], "...and %d more -- Show calls lists every one.",
                         missing - p.nmissing);
            } else {
                snprintf(info.head[n++], sizeof info.head[0], "Missing: %s", list);
            }
        }
        for (int i = 0; i < p.npart && n < ULAUNCH_HEAD_LINES; i++)
            snprintf(info.head[n++], sizeof info.head[0], "%s: %s", str(p.part[i].names), str(p.part[i].note));
        info.head_lines = n;
        snprintf(info.always, sizeof info.always, "Do not ask again");
        snprintf(info.side[0], sizeof info.side[0], "Show calls");
        snprintf(info.side[1], sizeof info.side[1], "Settings...");
    }
    struct uapp_desc d = { 0 };
    uui_runask_app_desc(&d, path, &info, card_done, 0);
    return uapp_run(&d);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--calls")) return retro_calls_main(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--details")) return details_main(argv[2]);
    w16_prefs_load(&g_def, 0);
    g_nfolders = w16_folders_load(&g_folders);
    if (argc == 3 && !strcmp(argv[1], "--show")) request_show(argv[2]);
    else if (argc == 2 && argv[1][0] != '-') {   // the File Manager's Open
        int rc = open_from_elsewhere(argv[1]);
        if (rc >= 0) return rc;
    }
    if (!ugfx_font_init()) return 2;
    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = item_flags;
    uui_menubar_init(&g_ctx, 0, 0);
    g_ctx.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    g_tb.overflow = 1;
    uui_sidebar_init(&g_side, 0, 0, 0, 0, g_rows, 0);
    uui_grid_init(&g_grid, ugfx_char_w() * 12, ugfx_char_h() * 4 + 36);
    g_grid.cell = tile;
    for (int i = 0; i < NSET; i++) uui_dropdown_init(&g_dd[i], 0, 0, 0, 0, g_dd_items[i], 1);
    uui_dropdown_init(&g_ddc, 0, 0, 0, 0, g_dc_items, 1);
    static const char *const DRV_LABELS[] = { "Add drive...", "Change...", "Remove" };
    static const int DRV_CODES[] = { ID_DRV_ADD, ID_DRV_CHANGE, ID_DRV_REMOVE };
    static const char *const DRV_NAMES[] = { "drive-add", "drive-change", "drive-remove" };
    uui_listedit_init(&g_drives, DRIVE_COLS, 3, drive_cell, 0, 0, "drive-table", DRV_LABELS, DRV_CODES, DRV_NAMES, 3);
    g_drives.enabled = drive_enabled;
    g_drives.details = drive_details;
    static const char *const FOL_LABELS[] = { "Add...", "Change...", "Rename...", "Remove", "Move up", "Move down" };
    static const int FOL_CODES[] = { ID_FOL_ADD, ID_FOL_CHANGE, ID_FOL_RENAME, ID_FOL_REMOVE, ID_FOL_UP, ID_FOL_DOWN };
    static const char *const FOL_NAMES[] = { "folder-add", "folder-change", "folder-rename", "folder-remove",
                                             "folder-up", "folder-down" };
    uui_listedit_init(&g_ofolders, FOLDER_COLS, 3, folder_cell, 0, 0, "folder-table", FOL_LABELS, FOL_CODES,
                      FOL_NAMES, 6);
    g_ofolders.enabled = folder_enabled;
    retro_sounds_init();
    uui_button_init(&g_run, 0, 0, 0, 0, "Run", UUI_ROLE(accent), UUI_ROLE(accent_text), ID_RUN);
    uui_button_init(&g_calls, 0, 0, 0, 0, "Calls it imports...", UUI_COLOR_UNSET, UUI_COLOR_UNSET, ID_CALLS);
    uui_stackbar_init(&g_bar, 3);
    uui_sbar_init(&g_psb);
    uui_stackbar_add(&g_bar, "Work", 0);
    uui_stackbar_add(&g_bar, "Partly", 0);
    uui_stackbar_add(&g_bar, "Missing", 0);
    uui_statusbar_init(&g_sb);
    g_sb.count = 3;
    g_sb.panes[0].text = g_st_count; g_sb.panes[0].chars = 14;
    g_sb.panes[1].text = g_st_runs;  g_sb.panes[1].chars = 10;
    g_sb.panes[2].text = g_st_need;  g_sb.panes[2].chars = 26;
    find_programs();
    build_sidebar();
    uui_sidebar_select_id(&g_side, -1);
    build_view();
    fill_panel();
    consume_show();

    struct uapp_desc desc = {
        .title        = "Retro Launcher",
        .app_id       = "retrolauncher",
        .on_size      = default_size,
        .flags        = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE,
        // From the font, as layout() is: the sidebar (20 'n'), a grid of
        // at least one tile and a bit (16), the details panel (34) -- a
        // fixed 640 let the panel slide over the sidebar at 1.5x.
        .min_w        = ugfx_char_w() * (20 + 16 + 34) + 2,
        .min_h        = ugfx_char_h() * 20,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .tick_ms      = 50,
        .on_tick      = on_tick,
        .on_open      = on_open,
        .on_press     = on_press,
        .on_focus     = on_focus,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
    };
    return uapp_run(&desc);
}
