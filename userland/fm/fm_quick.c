// Quick access, the File Manager's start page: the "quick:/" virtual
// folder -- a uui_fileview source -- in three bands, the pinned folders,
// the files opened lately and the drives. Windows 10's Quick access and
// Windows 11's Home are this same thing, a folder view with groups, not
// a page of its own; "Home" here already names /home.
//
// Every row is something real elsewhere, so the page holds no state: it
// is rebuilt from lib/upins.h, lib/urecent.h and the places list on each
// listing, and opening a row goes where the row points.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include <string.h>
#include <stdio.h>
#include <caltime.h>
#include "kpath.h"
#include "lib/upins.h"
#include "lib/urecent.h"
#include "lib/human.h"
#include "lib/udate.h"
#include "ui/uui_table.h"
#include "ui/ulog.h"

#define QUICK_RECENT 8   // the newest few; Recent holds the rest

enum { Q_PIN, Q_RECENT, Q_DRIVE };

struct qrow {
    int kind;
    char path[PATH_MAX_LEN];
    char where[PATH_MAX_LEN];   // the Where column
    char what[48];              // the Kind column
    const char *icon;           // a drive's, else NULL (by the name)
    long long when;             // a recent file's opening, else 0
};

// ONE LISTING FOR BOTH PANES, as Recent's and the bin's.
static struct qrow g_rows[UPINS_MAX + QUICK_RECENT + UUI_PLACES_MAX];
static int g_n;

static const struct uui_table_column quick_cols[] = {
    { "Name",   0,  UUI_TALIGN_LEFT, "name",   0 },
    { "Where",  24, UUI_TALIGN_LEFT, "where",  0 },
    { "Opened", 16, UUI_TALIGN_LEFT, "opened", 0 },
    { "Kind",   16, UUI_TALIGN_LEFT, "kind",   0 },
};

static int add(int kind, const char *path, const char *name, struct sys_dirent *out, int cap) {
    if (g_n >= cap || g_n >= (int)(sizeof g_rows / sizeof g_rows[0])) return 0;
    struct qrow *r = &g_rows[g_n];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    strlcpy(r->path, path, sizeof r->path);
    struct sys_dirent *e = &out[g_n];
    memset(e, 0, sizeof *e);
    strlcpy(e->name, name, sizeof e->name);
    struct sys_stat st;
    if (sys_stat(path, &st) == 0) {
        e->is_dir = st.is_dir;
        e->size = st.is_dir ? 0 : (uint32_t)st.size;
        e->modified = st.modified;
    }
    g_n++;
    return 1;
}

static int quick_list(void *ctx, const char *dir, struct sys_dirent *out, int cap) {
    (void)ctx; (void)dir;
    g_n = 0;

    // The pinned FOLDERS: Recent and the bin are places, not folders, and
    // are a click away in the column beside this.
    static char pins[UPINS_MAX][UPINS_PATH];
    int np = upins_load(pins, UPINS_MAX);
    for (int i = 0; i < np; i++) {
        char path[PATH_MAX_LEN], label[64];
        if (upins_resolve(pins[i], path, sizeof path, label, sizeof label, 0)) continue;
        if (!add(Q_PIN, path, label, out, cap)) break;
        strlcpy(g_rows[g_n - 1].where, path, sizeof g_rows[0].where);
        strlcpy(g_rows[g_n - 1].what, "Pinned folder", sizeof g_rows[0].what);
    }

    static struct urecent_item items[QUICK_RECENT];
    int nr = urecent_list(items, QUICK_RECENT);
    for (int i = 0; i < nr; i++) {
        if (!add(Q_RECENT, items[i].path, k_path_basename(items[i].path), out, cap)) break;
        struct qrow *r = &g_rows[g_n - 1];
        k_path_dirname(items[i].path, r->where, sizeof r->where);
        strlcpy(r->what, items[i].app, sizeof r->what);
        r->when = items[i].when;
    }

    // The drives, as the column lists them: its mounts, with their space.
    for (int i = g_places.places; i < g_places.count; i++) {
        const struct uui_place *p = &g_places.row[i];
        if (!p->device) continue;
        if (!add(Q_DRIVE, p->path, p->label, out, cap)) break;
        struct qrow *r = &g_rows[g_n - 1];
        out[g_n - 1].is_dir = 1;
        uui_places_short_note(p, r->where, sizeof r->where);
        strlcpy(r->what, p->remote ? "Network share" : p->removable ? "Removable drive"
                         : p->persistent ? "Drive" : "In memory", sizeof r->what);
        r->icon = p->icon;
    }
    return g_n;
}

static int quick_path(void *ctx, int i, char *out, int cap) {
    (void)ctx;
    return i >= 0 && i < g_n && strlcpy(out, g_rows[i].path, (size_t)cap) < (size_t)cap;
}

static void quick_cell(void *ctx, int i, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (i < 0 || i >= g_n) return;
    const struct qrow *r = &g_rows[i];
    if (col == 1) strlcpy(out, r->where, (size_t)cap);
    else if (col == 2 && r->when) {
        struct rtc_time t;
        cal_epoch_to_rtc((uint64_t)r->when, &t);
        udate_format(out, (unsigned long)cap, &t, UDATE_DATE | UDATE_TIME);
    } else if (col == 3) strlcpy(out, r->what, (size_t)cap);
}

// THE PAGE'S OWN ORDER, whatever the column: pins as the person ordered
// them, recent newest first, drives as the column lists them.
static int quick_compare(void *ctx, int a, int b, int col) {
    (void)ctx; (void)col;
    return a - b;
}

static int quick_group(void *ctx, int i) {
    (void)ctx;
    return i >= 0 && i < g_n ? g_rows[i].kind : Q_DRIVE;
}

static void quick_group_title(void *ctx, int g, char *out, int cap) {
    (void)ctx;
    static const char *const names[] = { "Pinned folders", "Recent files", "Drives" };
    strlcpy(out, g >= 0 && g <= Q_DRIVE ? names[g] : "", (size_t)cap);
}

static const char *quick_icon(void *ctx, int i) {
    (void)ctx;
    return i >= 0 && i < g_n ? g_rows[i].icon : 0;
}

static const struct uui_fileview_source g_quick_src = {
    .list = quick_list, .path = quick_path,
    .cols = quick_cols, .ncols = 4,
    .cell = quick_cell, .compare = quick_compare,
    .group = quick_group, .group_title = quick_group_title,
    .sort_col = 2, .sort_dir = 1,
    .icon = quick_icon,
};

const struct uui_fileview_source *quick_source(void) { return &g_quick_src; }

int in_quick(const struct uui_fileview *fv) { return uui_fileview_source(fv) == &g_quick_src; }

// Nothing is made, pasted, renamed, moved or deleted here: the rows are
// other places' things, and their own folders are where those happen.
int quick_item_flags(int code, unsigned *out) {
    if (!in_quick(active())) return 0;
    switch (code) {
    case CMD_UP: case CMD_MENU_NEW: case CMD_MKDIR: case CMD_NEW_FILE:
    case CMD_CLIP_CUT: case CMD_CLIP_PASTE: case CMD_RENAME: case CMD_MOVE:
    case CMD_DELETE: case CMD_DELETE_FOREVER: case CMD_COMPRESS_ZIP: case CMD_COMPRESS_ASK:
    case CMD_TERMINAL_HERE: case CMD_PIN_HERE:
        *out = UUI_MI_DISABLED;
        return 1;
    default:
        return 0;
    }
}
