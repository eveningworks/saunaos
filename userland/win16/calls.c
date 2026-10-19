// calls.c -- "<program> - calls": every Windows function a
// Win16 program imports that w16 does not have yet, or all of them, in a
// table of module, ordinal, name and what it is for, a chip per module to
// narrow it, and a search. Dependency Walker's shape; the "Win16 missing
// calls" canvas's M2. Its own window and process, `win16 --calls PATH`,
// so it stays open beside the program it describes.
//
// Like the launcher it parses no program: `w16 --check` names each import
// ("call missing USER.1 MessageBox Dialogs") and this reads those lines.
#include "win16/win16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "lib/uclip.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui.h"
#include "ui/uui_chips.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_focus.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_table.h"
#include "ui/uui_textbox.h"
#include "ui/uui_toolbar.h"

struct call {
    char *module;          // "USER"
    unsigned ordinal;
    char *name;            // "MessageBox", "?" when w16 has no name for it
    char *kind;            // "Dialogs"
    int has;               // w16 implements it
};

static const char *g_path;
static char *g_name;                         // "Notepad": win16_short_title()
static char *g_title;                        // "Notepad - calls"
static struct call *g_calls;
static int g_ncalls, g_imports, g_has;
static int g_all;                            // every import, not just the missing

static char **g_mods;                        // the chips: "All", then each module shown
static int *g_mod_counts, g_nmods;
static int *g_rows, g_nrows;                 // the calls on screen, into g_calls

enum { CMD_COPY = 1, CMD_SAVE, CMD_ALL };
enum { ID_TOOLBAR = 1, ID_CHIPS, ID_SEARCH, ID_TABLE, ID_STATUS };

static struct uapp *g_app;
static struct uui_toolbar g_tb;
static struct uui_chips g_chips;
static struct uui_textbox g_search;
static struct uui_table g_table;
static struct uui_statusbar g_sb;
static struct uui_filedialog g_saver;        // ~20 KB: file scope
static char g_st_count[64], g_st_note[64];
static int g_copied = -1;                    // rows the last Copy put on the clipboard, for the log

static const struct uui_table_column COLS[] = {
    { "Module",   10, UUI_TALIGN_LEFT,  0, 0 },
    { "Ordinal",   8, UUI_TALIGN_RIGHT, 0, 0 },
    { "Function",  0, UUI_TALIGN_LEFT,  0, 0 },
    { "Kind",     16, UUI_TALIGN_LEFT,  0, 0 },
    { "w16",       9, UUI_TALIGN_LEFT,  0, 0 },
};

// --- what w16 says ------------------------------------------------------------

static char *dup_n(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}

// "call missing USER.1 MessageBox Dialogs": the kind is the rest of the
// line, and may hold spaces.
static int parse_call(char *l, struct call *c) {
    char *has = l + 5, *imp = strchr(has, ' ');
    if (!imp) return 0;
    char *dot = strchr(++imp, '.'), *name = strchr(imp, ' ');
    if (!dot || !name || dot > name) return 0;
    char *kind = strchr(++name, ' ');
    if (!kind) return 0;
    kind++;
    c->has = !strncmp(has, "has ", 4);
    c->ordinal = (unsigned)strtoul(dot + 1, 0, 10);
    c->module = dup_n(imp, (size_t)(dot - imp));
    c->name = dup_n(name, (size_t)(kind - 1 - name));
    c->kind = strdup(kind);
    return c->module && c->name && c->kind;
}

static int load(void) {
    char *out = win16_check(g_path);
    if (!out) return 0;
    char *desc = 0;
    int cap = 0;
    for (char *l = out; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (!strncmp(l, "description ", 12)) desc = l + 12;
        else if (!strncmp(l, "imports ", 8)) g_imports = atoi(l + 8);
        else if (!strncmp(l, "has ", 4)) g_has = atoi(l + 4);
        else if (!strncmp(l, "call ", 5)) {
            if (g_ncalls == cap) {
                struct call *g = realloc(g_calls, sizeof *g_calls * (size_t)(cap = cap ? cap * 2 : 128));
                if (!g) break;
                g_calls = g;
            }
            if (parse_call(l, &g_calls[g_ncalls])) g_ncalls++;
        }
        l = nl ? nl + 1 : 0;
    }
    if (g_has >= g_imports) g_all = 1;           // nothing missing: the whole list
    const char *base = strrchr(g_path, '/');
    g_name = win16_short_title(desc && *desc ? desc : base ? base + 1 : g_path);
    free(out);
    return 1;
}

static void make_title(void) {
    free(g_title);
    size_t n = strlen(g_name ? g_name : "") + 32;
    if ((g_title = malloc(n)))
        snprintf(g_title, n, "%s - calls", g_name ? g_name : "");
}

// --- what is on screen ----------------------------------------------------------

static int shown(const struct call *c) { return g_all || !c->has; }

// The chips: All, then each module among the calls shown, in the order
// they first appear (w16 lists them sorted by module number).
static void build_chips(void) {
    for (int i = 1; i < g_nmods; i++) free(g_mods[i]);
    g_nmods = 0;
    free(g_mods);
    free(g_mod_counts);
    g_mods = malloc(sizeof *g_mods * (size_t)(g_ncalls + 1));
    g_mod_counts = malloc(sizeof *g_mod_counts * (size_t)(g_ncalls + 1));
    if (!g_mods || !g_mod_counts) return;
    g_mods[0] = "All";
    g_mod_counts[0] = 0;
    g_nmods = 1;
    for (int i = 0; i < g_ncalls; i++) {
        if (!shown(&g_calls[i])) continue;
        g_mod_counts[0]++;
        int m = 1;
        while (m < g_nmods && strcmp(g_mods[m], g_calls[i].module)) m++;
        if (m == g_nmods) {
            if (!(g_mods[m] = strdup(g_calls[i].module))) continue;
            g_mod_counts[g_nmods++] = 0;
        }
        g_mod_counts[m]++;
    }
    int sel = g_chips.selected < g_nmods ? g_chips.selected : 0;
    uui_chips_init(&g_chips, (const char *const *)g_mods, g_mod_counts, g_nmods, sel < 0 ? 0 : sel);
}

static int contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < n && hay[k] && (hay[k] | 0x20) == (needle[k] | 0x20)) k++;
        if (k == n) return 1;
    }
    return !n;
}

static void build_rows(void) {
    free(g_rows);
    g_rows = malloc(sizeof *g_rows * (size_t)(g_ncalls + 1));
    g_nrows = 0;
    const char *q = uui_textbox_text(&g_search);
    const char *mod = g_chips.selected > 0 && g_chips.selected < g_nmods ? g_mods[g_chips.selected] : 0;
    for (int i = 0; g_rows && i < g_ncalls; i++) {
        const struct call *c = &g_calls[i];
        if (!shown(c) || (mod && strcmp(c->module, mod))) continue;
        char imp[48];
        snprintf(imp, sizeof imp, "%s.%u", c->module, c->ordinal);
        if (!contains(c->name, q) && !contains(c->kind, q) && !contains(imp, q)) continue;
        g_rows[g_nrows++] = i;
    }
    g_table.col_count = g_all ? 5 : 4;
    uui_table_set_rows(&g_table, g_nrows);
    int missing = g_imports - g_has;
    snprintf(g_st_count, sizeof g_st_count, "%d missing of %d imported", missing, g_imports);
    if (g_nrows != (g_all ? g_ncalls : missing))
        snprintf(g_st_note, sizeof g_st_note, "%d shown", g_nrows);
    else
        g_st_note[0] = 0;
}

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (row < 0 || row >= g_nrows) { out[0] = 0; return; }
    const struct call *c = &g_calls[g_rows[row]];
    switch (col) {
    case 0: snprintf(out, (size_t)cap, "%s", c->module); break;
    case 1: snprintf(out, (size_t)cap, "%u", c->ordinal); break;
    case 2: snprintf(out, (size_t)cap, "%s", c->name); break;
    case 3: snprintf(out, (size_t)cap, "%s", c->kind); break;
    default: snprintf(out, (size_t)cap, "%s", c->has ? "Has it" : "Missing"); break;
    }
}

static int compare(void *ctx, int a, int b, int col) {
    (void)ctx;
    const struct call *x = &g_calls[g_rows[a]], *y = &g_calls[g_rows[b]];
    int r = 0;
    switch (col) {
    case 0: r = strcmp(x->module, y->module); break;
    case 1: r = (x->ordinal > y->ordinal) - (x->ordinal < y->ordinal); break;
    case 2: r = strcmp(x->name, y->name); break;
    case 3: r = strcmp(x->kind, y->kind); break;
    default: r = x->has - y->has; break;
    }
    if (r || col == 0) return r ? r : (x->ordinal > y->ordinal) - (x->ordinal < y->ordinal);
    return strcmp(x->module, y->module);
}

// --- Copy and Save ----------------------------------------------------------------

// The rows on screen, in their order, a line each: "USER.1<tab>MessageBox<tab>Dialogs".
static char *as_text(size_t *len) {
    size_t cap = 1;
    for (int i = 0; i < g_nrows; i++) {
        const struct call *c = &g_calls[g_rows[i]];
        cap += strlen(c->module) + strlen(c->name) + strlen(c->kind) + 32;
    }
    char *t = malloc(cap);
    if (!t) return 0;
    size_t n = 0;
    for (int v = 0; v < uui_table_view_count(&g_table); v++) {
        int r = uui_table_source_row(&g_table, v);
        if (r < 0) continue;
        const struct call *c = &g_calls[g_rows[r]];
        n += (size_t)snprintf(t + n, cap - n, "%s.%u\t%s\t%s%s\n", c->module, c->ordinal, c->name,
                              c->kind, g_all ? (c->has ? "\thas it" : "\tmissing") : "");
    }
    *len = n;
    return t;
}

static void copy(void) {
    size_t n;
    char *t = as_text(&n);
    if (t && uclip_set_text(t, (int)n)) {
        snprintf(g_st_note, sizeof g_st_note, "Copied %d %s", g_nrows, g_nrows == 1 ? "call" : "calls");
        g_copied = g_nrows;
    } else {
        snprintf(g_st_note, sizeof g_st_note, "Too long to copy -- use Save as text");
    }
    free(t);
}

static void saved(void *ctx, const char *path) {
    (void)ctx;
    if (!path) return;
    size_t n;
    char *t = as_text(&n);
    FILE *f = t ? fopen(path, "w") : 0;
    int ok = f && fwrite(t, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    free(t);
    snprintf(g_st_note, sizeof g_st_note, ok ? "Saved %d calls" : "Could not save the list", g_nrows);
    uapp_redraw(g_app);
}

static void command(struct uapp *a, int cmd) {
    switch (cmd) {
    case CMD_COPY: copy(); break;
    case CMD_SAVE: {
        if (uui_filedialog_is_open(&g_saver)) break;
        struct uui_filedialog_opts o = { .mode = UUI_FILEDIALOG_SAVE, .title = "Save the list as text",
                                         .start_dir = "/home/Documents", .initial_name = "calls.txt" };
        uui_filedialog_open(a, &g_saver, &o, saved, 0);
        break;
    }
    case CMD_ALL:
        g_all = !g_all;
        build_chips();
        build_rows();
        make_title();
        if (g_title) uapp_set_title(a, g_title);
        break;
    default: break;
    }
}

static unsigned item_flags(int code) { return code == CMD_ALL && g_all ? UUI_MI_CHECKED : 0; }

static struct uui_toolbar_item TB[] = {
    { "tb-copy", "Copy the list (Ctrl+C)", CMD_COPY, "Copy",          0, "Ctrl+C", UTHEME_ACT_VIEW },
    { "tb-save", "Save the list as text",  CMD_SAVE, "Save as text...", 0, 0,      UTHEME_ACT_EDIT },
    UUI_TOOLBAR_SEP,
    { "tb-show", "List every call it imports, not just the missing", CMD_ALL, "Show all imports", 0, 0,
      UTHEME_ACT_VIEW },
};

static struct uui_focusable g_ring[] = {
    { &g_table,  &uui_table_ops },
    { &g_search, &uui_textbox_focus_ops },
    { &g_chips,  &uui_chips_ops },
};
enum { RING_TABLE = 0, RING_SEARCH };
static struct uui_focus g_focus;

static struct uui_item g_widgets[] = {
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,     .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_chips_ops,     .widget = &g_chips,  .id = ID_CHIPS,   .name = "modules" },
    { .ops = &uui_textbox_ops,   .widget = &g_search, .id = ID_SEARCH,  .name = "search" },
    { .ops = &uui_table_ops,     .widget = &g_table,  .id = ID_TABLE,   .name = "calls" },
    { .ops = &uui_statusbar_ops, .widget = &g_sb,     .id = ID_STATUS,  .name = "status" },
};

// --- the window --------------------------------------------------------------------

static void layout(int cw, int ch) {
    int pad = ugfx_char_w(), lh = ugfx_char_h();
    int th = uui_toolbar_height(&g_tb);
    uui_toolbar_ops.set_geometry(&g_tb, 0, 0, cw, th);
    int sbh = uui_statusbar_height(&g_sb);
    uui_statusbar_set_geometry(&g_sb, 0, ch - sbh, cw, sbh);
    int sw = pad * 22, y = th + pad / 2;
    int chips_w = cw - 3 * pad - sw;
    int chips_h = uui_chips_height_for(&g_chips, chips_w);
    uui_chips_set_geometry(&g_chips, pad, y, chips_w, chips_h);
    uui_textbox_set_geometry(&g_search, cw - pad - sw, y, sw, lh + 8);
    y += (chips_h > lh + 8 ? chips_h : lh + 8) + pad / 2;
    uui_table_ops.set_geometry(&g_table, 0, y, cw, ch - sbh - y);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    layout(d->surface->w, d->surface->h);
    ugfx_fill_rect(d->surface, 0, g_table.y - 1, d->surface->w, 1, UTHEME_SEPARATOR);
    uapp_log_layout(a, "win16-calls");
    // What a test asks: how many rows, the first in table order, whether
    // every import is listed, and what the last Copy took.
    int r = uui_table_source_row(&g_table, 0);
    const struct call *c = r >= 0 ? &g_calls[g_rows[r]] : 0;
    uapp_logf_layout("win16-calls: layout rows %d all %d first %s.%u %s copied %d\n", g_nrows, g_all,
                     c ? c->module : "-", c ? c->ordinal : 0, c ? c->name : "-", g_copied);
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    int code;
    switch (id) {
    case ID_TOOLBAR: if ((code = uui_toolbar_take_code(&g_tb)) > 0) command(a, code); break;
    case ID_CHIPS: case ID_SEARCH: build_rows(); break;
    default: break;
    }
    uapp_redraw(a);
}

// Keys the focused widget did not take. Ctrl+C in the search box is its
// own text's; anywhere else it copies the list.
static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == 0x03 && g_focus.current != RING_SEARCH) command(a, CMD_COPY);
    else if (key == 0x06) uui_focus_set(&g_focus, RING_SEARCH);   // Ctrl+F
    uapp_redraw(a);
}

static int on_tick(struct uapp *a) { (void)a; return uui_toolbar_tick(&g_tb); }
static void on_open(struct uapp *a) { g_app = a; }

static void default_size(int *w, int *h) {
    *w = ugfx_char_w() * 72;
    *h = ugfx_char_h() * 32;
}

int win16_calls_main(const char *path) {
    g_path = path;
    if (!ugfx_font_init()) return 2;
    if (!load()) return 1;
    uui_toolbar_init(&g_tb, TB, (int)(sizeof TB / sizeof TB[0]));
    g_tb.item_flags = item_flags;
    g_tb.overflow = 1;
    uui_chips_init(&g_chips, 0, 0, 0, 0);
    uui_textbox_init(&g_search, "");
    g_search.placeholder = "Search";
    uui_table_init(&g_table, 0, 0, 0, 0, COLS, 4, cell, 0);
    uui_table_set_compare(&g_table, compare);
    uui_statusbar_init(&g_sb);
    g_sb.count = 2;
    g_sb.panes[0].text = g_st_count; g_sb.panes[0].chars = 26;
    g_sb.panes[1].text = g_st_note;
    build_chips();
    build_rows();
    make_title();
    uui_focus_init(&g_focus, g_ring, (int)(sizeof g_ring / sizeof g_ring[0]));
    uui_focus_set(&g_focus, RING_TABLE);

    struct uapp_desc desc = {
        .title        = g_title ? g_title : "Win16 calls",
        .app_id       = "win16-calls",
        .on_size      = default_size,
        .flags        = UAPP_RESIZABLE,
        .min_w        = 480,
        .min_h        = 300,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .tick_ms      = 50,
        .on_tick      = on_tick,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .focus        = &g_focus,
    };
    return uapp_run(&desc);
}
