// win16.c -- Win16 Programs: the Windows 3.x programs in a few folders,
// each with its own icon and how far w16 runs it, run with a double click
// and set up per program (docs/w16-design.md). The design is the
// "Win16 Programs launcher" canvas's L1 and S2: folders on the left,
// program tiles, a details panel with the program's own settings, and an
// Options window for the defaults (w16/w16prefs.h, which w16 reads too).
//
// THIS APP NEVER PARSES A PROGRAM. A Windows executable is hostile input;
// `w16 --check` reads it inside w16's sandbox and answers in plain lines
// -- its name, what it imports that w16 lacks, its icon as pixels -- and
// that answer is all this reads. A folder is checked a few programs a
// tick, so a big one fills in rather than holding the window shut.
//
// `win16 FILE` runs that program and exits without a window: what the
// File Manager's Open does for an .exe (Handles= in its desktop entry).
// `win16 --calls FILE` is the window listing what it imports
// (win16/calls.c), which the details panel's button opens.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
#include "ui/uui_menubar.h"
#include "ui/uui_prefs.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_stackbar.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_textbox.h"
#include "ui/uui_toolbar.h"
#include "w16/w16prefs.h"
#include "win16/win16.h"

#define MISSING_SHOWN 3                      // the panel names this many; the window lists all

// A stand-in's note and the calls w16 answers that way: "CreateDC, Escape"
// share "no printer to draw on" when w16 says the same of both.
struct partly {
    char *names;
    char *note;
};

// Every string is allocated (prog_free()); `icon` is w16_icon()'s largest.
struct prog {
    char *path;
    char *name;                      // its description, else its file name
    char *title;                     // what its tile says: win16_short_title()
    char *module;
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
static struct w16_prefs g_def;               // the defaults, and the folders
static struct uapp *g_app;

enum { CMD_RUN = 1, CMD_PROPS, CMD_ADD, CMD_REMOVE, CMD_REFRESH, CMD_OPTIONS, CMD_EXIT, CMD_CALLS };
enum { ID_MENU = 1, ID_TOOLBAR, ID_SIDEBAR, ID_GRID, ID_STATUS, ID_SCALE, ID_VERSION, ID_FRAME,
       ID_TRACE, ID_RUN, ID_OPT_DRIVE, ID_CALLS, ID_METHOD, ID_FONTS, ID_OPT_FONTCARD };

static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static struct uui_sidebar g_side;
static struct uui_grid g_grid;
static struct uui_statusbar g_sb;
#define NSET 6                               // the per-program settings: SETTINGS below
static struct uui_dropdown g_dd[NSET];
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
    free(p->path); free(p->name); free(p->title); free(p->module); free(p->last_text);
    for (int i = 0; i < p->nmissing; i++) free(p->missing[i]);
    for (int i = 0; i < p->npart; i++) { free(p->part[i].names); free(p->part[i].note); }
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
        if (!p->part[i].names || !p->part[i].note) {
            free(p->part[i].names); free(p->part[i].note);
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
    char *out = win16_check(p->path);
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
    p->title = win16_short_title(p->name);
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
    for (int f = 0; f < W16_FOLDERS_MAX; f++) {
        if (!g_def.folder[f][0]) continue;
        struct sys_dirent *ents;
        int n = ulistdir(g_def.folder[f], &ents);
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
            size_t len = strlen(g_def.folder[f]) + strlen(ents[i].name) + 2;
            if ((p->path = malloc(len))) snprintf(p->path, len, "%s/%s", g_def.folder[f], ents[i].name);
            set_str(&p->name, ents[i].name);
            set_str(&p->title, ents[i].name);
            p->folder = f;
            if (!p->path) g_nprog--;
        }
        free(n >= 0 ? ents : 0);
    }
}

// --- the sidebar: All programs, then each folder ---------------------------------

static struct uui_sidebar_row g_rows[W16_FOLDERS_MAX + 2];
static char g_row_label[W16_FOLDERS_MAX][48];

static void folder_label(int f, char *out, size_t cap) {
    const char *path = g_def.folder[f];
    snprintf(out, cap, "%s", !strcmp(path, "/usr/share/w16") ? "Samples" : file_name(path));
}

static void build_sidebar(void) {
    int n = 0;
    g_rows[n++] = (struct uui_sidebar_row){ "All programs", UUI_SIDEBAR_TOP, 0, -1 };
    for (int f = 0; f < W16_FOLDERS_MAX; f++) {
        if (!g_def.folder[f][0]) continue;
        folder_label(f, g_row_label[f], sizeof g_row_label[f]);
        g_rows[n++] = (struct uui_sidebar_row){ g_row_label[f], UUI_SIDEBAR_TOP, 0, f };
    }
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
    { "Trace its calls", TRACE_NAMES,   2, 4, offsetof(struct w16_prefs, trace) },
};

static int *field_of(struct w16_prefs *p, int i) { return (int *)((char *)p + SETTINGS[i].field); }

static void fill_panel(void) {
    struct prog *p = cur();
    unsigned mask = p ? w16_prefs_overridden(p->path) : 0;
    struct w16_prefs eff;
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
}

static void save_panel(void) {
    struct prog *p = cur();
    if (!p) return;
    struct w16_prefs eff;
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

// --- actions --------------------------------------------------------------------

static void run(const char *path) {
    if (!path) return;
    uapp_spawn(g_app, W16_BIN, path);
}

// The calls window, its own process: `win16 --calls PATH`.
static void show_calls(const char *path) {
    char *args[] = { "win16", "--calls", (char *)path, 0 };
    sys_spawn_argv("/bin/wm/apps/win16", args);
}

static void folder_chosen(void *ctx, const char *path) {
    (void)ctx;
    if (!path) return;
    for (int f = 0; f < W16_FOLDERS_MAX; f++)
        if (!strcmp(g_def.folder[f], path)) return;
    for (int f = 0; f < W16_FOLDERS_MAX; f++) {
        if (g_def.folder[f][0]) continue;
        snprintf(g_def.folder[f], sizeof g_def.folder[f], "%s", path);
        uprefs_save(&w16_prefs_defaults, &g_def, 0);
        find_programs();
        build_sidebar();
        uui_sidebar_select_id(&g_side, f);
        build_view();
        return;
    }
}

// --- Options: the defaults ----------------------------------------------------------

static struct w16_prefs g_edit;
static struct uui_textbox g_drive;
static const struct uui_sidebar_row OPT_PAGES[] = {
    { "General", UUI_SIDEBAR_TOP, 0, 0 },
    { "Display", UUI_SIDEBAR_TOP, 0, 1 },
    { "Files",   UUI_SIDEBAR_TOP, 0, 2 },
};
static const char *const FAULT_NAMES[] = { "Show the Error box", "Write the report only" };
static const char *const OPT_NOTES[] = {
    "What programs are told, and what happens when one fails.",
    "How a program's windows look, and whose fonts its text is drawn in.",
    "Drive C: is a folder here; a program's INI files go in its WINDOWS folder.",
};

static void opt_defaults(void) { uui_textbox_set_text(&g_drive, "/home/Win16"); }

static void opt_ok(void) {
    char drive[W16_PATH_MAX];
    snprintf(drive, sizeof drive, "%s", uui_textbox_text(&g_drive));
    if (drive[0] == '/') snprintf(g_edit.drive_c, sizeof g_edit.drive_c, "%s", drive);
    struct w16_prefs was = g_def;
    g_def = g_edit;
    uprefs_save(&w16_prefs_defaults, &g_def, &was);
    fill_panel();
    uapp_redraw(g_app);
}

static void edit_defaults(void *edit) {
    struct w16_prefs keep = *(struct w16_prefs *)edit;
    uprefs_defaults(&w16_prefs_defaults, edit);
    memcpy(((struct w16_prefs *)edit)->folder, keep.folder, sizeof keep.folder);   // folders are not options
}

static void open_options(void) {
    g_edit = g_def;
    static const struct uui_prefs_desc d = {
        .title = "Win16 Programs Options", .pages = OPT_PAGES, .page_count = 3,
        .log_prefix = "win16 options", .on_defaults = opt_defaults, .on_ok = opt_ok,
        .notes = OPT_NOTES, .edit = &g_edit, .edit_defaults = edit_defaults,
    };
    uui_prefs_begin(&d);
    uui_prefs_choice(0, "Windows version", VERSION_NAMES, 0, 3, offsetof(struct w16_prefs, version), "version");
    uui_prefs_choice(0, "When a program fails", FAULT_NAMES, 0, 2, offsetof(struct w16_prefs, on_fault), "on_fault");
    uui_prefs_check(0, "Debugging", "Trace every program's calls to a file", offsetof(struct w16_prefs, trace), "trace");
    uui_prefs_choice(1, "Scale", SCALE_NAMES, 0, W16_SCALE_COUNT, offsetof(struct w16_prefs, scale), "scale");
    uui_prefs_choice(1, "Scaled by", METHOD_NAMES, 0, 3, offsetof(struct w16_prefs, method), "method");
    uui_prefs_choice(1, "Window frame", FRAME_NAMES, 0, 2, offsetof(struct w16_prefs, frame), "frame");
    // 3.11's own fonts, where drive C: has them: what w16 found, asked of it now.
    uui_prefs_choice(1, "Fonts", FONTS_NAMES, 0, 2, offsetof(struct w16_prefs, fonts), "fonts");
    win16_fontcard_load(&g_win16_fontcard);
    uui_prefs_row(1, "", &win16_fontcard_ops, &g_win16_fontcard, ID_OPT_FONTCARD, "fontcard", UUI_FILL_W);
    uui_textbox_set_text(&g_drive, g_def.drive_c);
    uui_prefs_row(2, "Drive C:", &uui_textbox_ops, &g_drive, ID_OPT_DRIVE, "drive", 0);
    uui_prefs_open(g_app);
}

static void command(struct uapp *a, int cmd) {
    struct prog *p = cur();
    switch (cmd) {
    case CMD_RUN:     if (p && p->ok) run(p->path); break;
    case CMD_CALLS:   if (p && p->ok) show_calls(p->path); break;
    case CMD_PROPS:   g_panel = !g_panel; break;
    case CMD_ADD: {
        if (uui_filedialog_is_open(&g_chooser)) break;
        struct uui_filedialog_opts o = { .mode = UUI_FILEDIALOG_FOLDER, .title = "Add a folder of programs",
                                         .start_dir = "/home" };
        uui_filedialog_open(a, &g_chooser, &o, folder_chosen, a);
        break;
    }
    case CMD_REMOVE: {
        int f = uui_sidebar_selected_id(&g_side);
        if (f < 0) break;
        g_def.folder[f][0] = 0;
        uprefs_save(&w16_prefs_defaults, &g_def, 0);
        find_programs();
        build_sidebar();
        uui_sidebar_select_id(&g_side, -1);
        build_view();
        break;
    }
    case CMD_REFRESH: find_programs(); build_view(); break;
    case CMD_OPTIONS: open_options(); break;
    case CMD_EXIT:    uapp_quit(a, 0); break;
    default: break;
    }
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

static int g_dd_y[NSET], g_calls_y;
static struct uui_stackbar g_bar;            // works, partly, missing

// The settings block under the panel's details, and the Run button below
// it: what the "Partly there" list must leave room for.
static int settings_h(void) {
    int pad = ugfx_char_w(), lh = ugfx_char_h();
    return lh / 2 + lh + 3 + NSET * (lh + pad + pad / 2) + lh + pad * 3;
}

static void bold_line(struct ugfx_surface *s, int x, int *y, int w, const char *t, uint32_t col) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    line(s, x, y, w, t, col);
    ugfx_set_font(was);
}

static void draw_panel(struct ugfx_surface *s) {
    ugfx_fill_rect(s, g_px, g_py, g_pw, g_ph, UTHEME_PANEL_BG);
    ugfx_fill_rect(s, g_px, g_py, 1, g_ph, UTHEME_SEPARATOR);
    const struct prog *p = cur();
    int pad = ugfx_char_w(), x = g_px + pad * 2, w = g_pw - pad * 3, y = g_py + pad, lh = ugfx_char_h();
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    g_calls_y = -1;
    if (!p) {
        line(s, x, &y, w, g_nview ? "Pick a program." : "No programs here. Add a folder of them.", dim);
        return;
    }
    bold_line(s, x, &y, w, str(p->name), UTHEME_TEXT);
    line(s, x, &y, w, p->path, dim);
    char t[160];
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
        // What w16 answers without doing: as many as leave the settings
        // their room, then how many more the calls window lists.
        if (p->npart) {
            y += lh / 3;
            bold_line(s, x, &y, w, "Partly there", utheme_action(UTHEME_ACT_EDIT));
            // Below it: the calls button and a line of the last run, two
            // when it stopped and says why.
            int last_lines = p->last == 2 && p->last_text && p->last_text[0] ? 2 : 1;
            int bottom = g_py + g_ph - settings_h() - (lh + pad + pad / 2) - last_lines * (lh + 3);
            int room = (bottom - y) / (lh + 3);
            int shown = p->npart <= room ? p->npart : room > 1 ? room - 1 : 0;
            int nw = w * 2 / 5;
            for (int i = 0; i < shown; i++) {
                bold_line(s, x, &(int){ y }, nw - pad, str(p->part[i].names), UTHEME_TEXT);
                line(s, x + nw, &y, w - nw, str(p->part[i].note), dim);
            }
            if (shown < p->npart) {
                snprintf(t, sizeof t, "and %d more: see the calls", p->npart - shown);
                line(s, x, &y, w, t, dim);
            }
        }
        g_calls_y = y + 2;                       // the calls button, placed by layout()
        y += lh + pad + pad / 2;
        if (p->last) {
            char when[32] = "";
            time_t tt = (time_t)p->last_when;
            struct tm *tm = localtime(&tt);
            if (tm) strftime(when, sizeof when, "%Y-%m-%d %H:%M", tm);
            snprintf(t, sizeof t, "Last run %s: %s", when, p->last == 2 ? "stopped" : "ended normally");
            line(s, x, &y, w, t, p->last == 2 ? utheme_severity(UTHEME_SEV_ERROR) : UTHEME_TEXT);
            if (p->last == 2 && p->last_text && p->last_text[0]) line(s, x, &y, w, p->last_text, dim);
        } else {
            line(s, x, &y, w, "Not run yet", dim);
        }
    } else if (p->checked) {
        line(s, x, &y, w, "Not a Windows 3.x program w16 can read.", dim);
    }
    y += lh / 2;
    bold_line(s, x, &y, w, "This program's settings", utheme_action(UTHEME_ACT_EDIT));
    for (int i = 0; i < NSET; i++) {
        g_dd_y[i] = y;
        ugfx_draw_string_clipped(s, x, y + (lh + pad - lh) / 2, w / 2, SETTINGS[i].caption, UTHEME_TEXT,
                                 UTHEME_PANEL_BG);
        y += lh + pad + pad / 2;
    }
}

static struct uui_menu_item program_items[] = {
    UUI_MENU("Run", CMD_RUN, "Enter"),
    UUI_MENU("Calls it imports...", CMD_CALLS, 0),
    UUI_MENU("Details panel", CMD_PROPS, "F4"),
    UUI_MENU_SEP,
    UUI_MENU("Add folder...", CMD_ADD, 0),
    UUI_MENU("Remove this folder", CMD_REMOVE, 0),
    UUI_MENU("Refresh", CMD_REFRESH, "F5"),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static struct uui_menu_item tools_items[] = {
    UUI_MENU("Options...", CMD_OPTIONS, 0),
};
static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("Program", program_items),
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
    if (code == CMD_REMOVE && uui_sidebar_selected_id(&g_side) < 0) return UUI_MI_DISABLED;
    if (code == CMD_PROPS && g_panel) return UUI_MI_CHECKED;
    return 0;
}

static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,  .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,    .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_sidebar_ops,   .widget = &g_side,  .id = ID_SIDEBAR, .name = "folders" },
    { .ops = &uui_grid_ops,      .widget = &g_grid,  .id = ID_GRID,    .name = "programs" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[0], .id = ID_SCALE,   .name = "scale" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[1], .id = ID_METHOD,  .name = "method" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[2], .id = ID_VERSION, .name = "version" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[3], .id = ID_FRAME,   .name = "frame" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[4], .id = ID_FONTS,   .name = "fonts" },
    { .ops = &uui_dropdown_ops,  .widget = &g_dd[5], .id = ID_TRACE,   .name = "trace" },
    { .ops = &uui_button_ops,    .widget = &g_run,   .id = ID_RUN,     .name = "run" },
    { .ops = &uui_button_ops,    .widget = &g_calls, .id = ID_CALLS,   .name = "calls" },
    { .ops = &uui_statusbar_ops, .widget = &g_sb,    .id = ID_STATUS,  .name = "status" },
};
#define W_PANEL 4   // then the NSET dropdowns, Run, the calls button

static void layout(int cw, int ch) {
    int pad = ugfx_char_w(), lh = ugfx_char_h();
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
    // The settings rows exist only beside a program.
    for (int i = 0; i < NSET + 2; i++) g_widgets[W_PANEL + i].hidden = !g_panel || (i != NSET && !cur());
    g_widgets[W_PANEL + NSET + 1].hidden |= g_calls_y < 0;
    if (!g_panel) return;
    int dx = g_px + pad * 2 + pad * 14, dw = g_pw - pad * 17;
    for (int i = 0; i < NSET; i++) uui_dropdown_set_geometry(&g_dd[i], dx, g_dd_y[i], dw, lh + pad);
    int bw = ugfx_text_width("Run") + pad * 6;
    uui_button_set_geometry(&g_run, g_px + pad * 2, g_py + g_ph - lh - pad * 3, bw, lh + pad);
    g_run.disabled = !(cur() && cur()->ok);
    // The missing calls, or the whole list when none are.
    const struct prog *p = cur();
    g_calls.label = p && p->imports > p->has ? "Missing calls..." : "Calls it imports...";
    int cw2 = ugfx_text_width(g_calls.label) + pad * 4;
    if (g_calls_y >= 0) uui_button_set_geometry(&g_calls, g_px + pad * 2, g_calls_y, cw2, lh + pad);
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
    if (g_panel) draw_panel(s);      // first: it places the dropdown rows layout() puts the boxes on
    layout(s->w, s->h);
    ugfx_fill_rect(s, 0, g_py - 1, s->w, 1, UTHEME_SEPARATOR);
    ugfx_fill_rect(s, g_side.w, g_py, 1, g_ph, UTHEME_SEPARATOR);
    uapp_log_layout(a, "win16");
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
        fill_panel();
        if ((code = uui_grid_take(&g_grid)) >= 0) command(a, CMD_RUN);
        break;
    case ID_SCALE: case ID_METHOD: case ID_VERSION: case ID_FRAME: case ID_FONTS: case ID_TRACE: save_panel(); break;
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

// Back from a program: what it did is in its last-run record.
static void on_focus(struct uapp *a, int focused) {
    if (!focused) return;
    for (int i = 0; i < g_nprog; i++) if (g_prog[i].checked) read_last(&g_prog[i]);
    uapp_redraw(a);
}

static void on_open(struct uapp *a) { g_app = a; }

static void default_size(int *w, int *h) {
    *w = ugfx_char_w() * 110;
    *h = ugfx_char_h() * 40;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--calls")) return win16_calls_main(argv[2]);
    if (argc == 2 && argv[1][0] != '-') {        // the File Manager's Open: run it, no window
        char *args[] = { "w16", argv[1], 0 };
        return sys_spawn_argv(W16_BIN, args) < 0 ? 1 : 0;
    }
    if (!ugfx_font_init()) return 2;
    w16_prefs_load(&g_def, 0);
    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    g_tb.overflow = 1;
    uui_sidebar_init(&g_side, 0, 0, 0, 0, g_rows, 0);
    uui_grid_init(&g_grid, ugfx_char_w() * 12, ugfx_char_h() * 4 + 36);
    g_grid.cell = tile;
    for (int i = 0; i < NSET; i++) uui_dropdown_init(&g_dd[i], 0, 0, 0, 0, g_dd_items[i], 1);
    uui_button_init(&g_run, 0, 0, 0, 0, "Run", UUI_ROLE(accent), UUI_ROLE(accent_text), ID_RUN);
    uui_button_init(&g_calls, 0, 0, 0, 0, "Calls it imports...", UUI_COLOR_UNSET, UUI_COLOR_UNSET, ID_CALLS);
    uui_textbox_init(&g_drive, "");
    uui_stackbar_init(&g_bar, 3);
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

    struct uapp_desc desc = {
        .title        = "Win16 Programs",
        .app_id       = "win16",
        .on_size      = default_size,
        .flags        = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE,
        .min_w        = 640,
        .min_h        = 400,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .tick_ms      = 50,
        .on_tick      = on_tick,
        .on_open      = on_open,
        .on_focus     = on_focus,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
    };
    return uapp_run(&desc);
}
