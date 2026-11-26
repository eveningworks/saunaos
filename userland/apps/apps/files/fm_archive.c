// The inside of an archive as a folder -- "archive:<archive>[/<folder in
// it>]", a uui_fileview source over base/uarchive.h: .zip, .tar, .tar.gz
// and .tgz, and a lone .gz. Explorer's compressed folders and KDE's zip:/
// and tar:/ workers: opening an archive lists it, its folders open in
// place, and it is READ-ONLY -- nothing is renamed, deleted or pasted
// inside; what is wanted comes out with Extract (a job, fm_jobs.c).
//
// An archive stores only full names ("a/b/c.txt"), so the folders of a
// level are DERIVED from the names below it, explicit folder records or
// not -- unzip(1) and every shell do the same.
#include "fm_internal.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <caltime.h>
#include "kpath.h"
#include "base/uarchive.h"
#include "base/human.h"
#include "base/udate.h"
#include "ui/uui_table.h"
#include "ui/uui_filedialog.h"
#include "base/ulog.h"
#include "base/ugrow.h"
#include "base/udirs.h"

// What one pane's listing holds, by index. PER PANE: the two panes may
// show two archives, or two folders of one, and a row index from one
// must never land in the other's rows. Grown to the view's own capacity,
// which doubles until the listing fits.
struct arc_list {
    struct ulist_entry *ent; int ent_cap;            // the rows, our own copy
    unsigned long long *packed; int packed_cap;
    char **zpath; int zpath_cap;                    // each row's own "archive:..." name, heap
    int has_packed;                                 // 0: a tar's members, packed as one stream
    int n;
};
static struct arc_list g_arc[2];

// "archive:/home/x.tar.gz/a/b" -> archive "/home/x.tar.gz", inner "a/b".
char *arc_split(const char *dir, const char **inner) {
    if (strncmp(dir, FM_ARC, sizeof FM_ARC - 1)) return 0;
    const char *path = dir + sizeof FM_ARC - 1;
    int cap = (int)strlen(path) + 1;   // the archive is a prefix of it
    char *archive = malloc((size_t)cap);
    if (archive && uarchive_split(path, archive, cap, inner)) return archive;
    free(archive);
    return 0;
}

// Rows [0, n) of `a` lose their names.
static void zpath_clear(struct arc_list *a) {
    for (int i = 0; i < a->n; i++) { free(a->zpath[i]); a->zpath[i] = 0; }
    a->n = 0;
}

int is_archive_name(const char *path) {
    return uarchive_kind_of(path) != UARC_NONE;
}

struct fill {
    struct arc_list *a;
    const char *inner;     // "" at the archive's top, else "a/b"
    size_t ilen;
    const char *dir;       // the view's own name, rows' paths start with it
    struct ulist_entry *out;
    int cap;
    int refused;           // members whose names would leave the folder
    int oom;               // a row's path could not be had: the listing failed
};

static int add(void *ctx, const struct uarchive_entry *ae) {
    struct fill *f = ctx;
    struct arc_list *a = f->a;
    const char *name = ae->name;
    unsigned long long size = ae->size, packed = ae->packed;
    a->has_packed = ae->has_packed;
    // Never listed: Extract would pass it by (uarchive_name_safe), and a
    // row named ".." would read as the folder above.
    if (!uarchive_name_safe(name)) { f->refused++; return 0; }
    if (f->ilen) {
        if (strncmp(name, f->inner, f->ilen) || name[f->ilen] != '/') return 0;
        name += f->ilen + 1;
    }
    if (!name[0]) return 0;                       // the folder's own record
    const char *slash = strchr(name, '/');
    int len = slash ? (int)(slash - name) : (int)strlen(name);
    if (len <= 0 || len >= (int)sizeof f->out[0].name) return 0;
    for (int i = 0; i < a->n; i++)                // a folder seen already
        if (f->out[i].is_dir && !strncmp(f->out[i].name, name, (size_t)len) && !f->out[i].name[len]) {
            a->packed[i] += packed;
            return 0;
        }
    if (a->n >= f->cap) return 1;                 // full: the view grows and lists again
    struct ulist_entry *e = &f->out[a->n];
    memset(e, 0, sizeof *e);
    memcpy(e->name, name, (size_t)len);
    e->name[len] = '\0';
    e->is_dir = slash != 0;
    e->size = e->is_dir ? 0 : (uint32_t)size;
    e->modified = ae->modified;
    a->packed[a->n] = packed;
    a->zpath[a->n] = 0;
    if (asprintf(&a->zpath[a->n], "%s/%s", f->dir, e->name) < 0) {
        a->zpath[a->n] = 0;
        f->oom = 1;
        return 1;   // stop: a row with no path cannot be opened
    }
    a->ent[a->n] = *e;
    a->n++;
    return 0;
}

static int z_list(void *ctx, const char *dir, struct ulist_entry *out, int cap) {
    struct arc_list *a = ctx;
    char err[96];
    const char *inner;
    zpath_clear(a);
    char *archive = arc_split(dir, &inner);
    if (!archive) return -EINVAL;
    // The view's name without a trailing '/', so rows join cleanly.
    char *base = strdup(dir);
    if (!base || !UGROW(a->ent, a->ent_cap, cap) || !UGROW(a->packed, a->packed_cap, cap) ||
        !UGROW(a->zpath, a->zpath_cap, cap)) {
        ulogf("files: archive %s: out of memory at %d rows\n", archive, cap);
        free(base);
        free(archive);
        return -ENOMEM;
    }
    size_t bl = strlen(base);
    if (bl && base[bl - 1] == '/') base[bl - 1] = '\0';
    struct fill f = { a, inner, strlen(inner), base, out, cap, 0, 0 };
    if (f.ilen && inner[f.ilen - 1] == '/') f.ilen--;
    a->has_packed = 1;
    int rc = uarchive_list(archive, add, &f, err, sizeof err);
    if (rc >= 0 && f.oom) {
        ulogf("files: archive %s: out of memory naming row %d\n", archive, a->n);
        rc = -ENOMEM;
    }
    if (rc < 0) {
        if (!f.oom) snprintf(g_stat_note, sizeof g_stat_note, "%s", err);
        if (!f.oom) ulogf("files: archive %s: %s\n", archive, err);
        zpath_clear(a);
    } else if (f.refused) {
        snprintf(g_stat_note, sizeof g_stat_note, "%d member%s with unsafe names not shown",
                 f.refused, f.refused == 1 ? "" : "s");
        ulogf("files: archive %s: %d members name a place outside it\n", archive, f.refused);
    }
    free(base);
    free(archive);
    return rc < 0 ? rc : a->n;
}

static char *z_path(void *ctx, int i) {
    const struct arc_list *a = ctx;
    return i >= 0 && i < a->n ? strdup(a->zpath[i]) : 0;
}

static const struct uui_table_column zip_cols[] = {
    { "Name",     0,  UUI_TALIGN_LEFT  },
    { "Size",     9,  UUI_TALIGN_RIGHT },
    { "Modified", 20, UUI_TALIGN_LEFT  },
    { "Packed",   9,  UUI_TALIGN_RIGHT },
};

static void z_cell(void *ctx, int i, int col, char *out, int cap) {
    const struct arc_list *a = ctx;
    out[0] = '\0';
    if (i < 0 || i >= a->n) return;
    const struct ulist_entry *e = &a->ent[i];
    if (col == 1 && !e->is_dir) human_size(out, (unsigned long)cap, e->size);
    else if (col == 2) udate_format(out, (unsigned long)cap, &e->modified, UDATE_DATE | UDATE_TIME);
    // A tar's members have no size of their own in the archive -- gzip
    // packs the stream as one -- so the cell stays empty rather than
    // show a number that is not one.
    else if (col == 3 && !a->has_packed) out[0] = '\0';
    else if (col == 3) human_size(out, (unsigned long)cap, (unsigned long)a->packed[i]);
}

static int z_compare(void *ctx, int a, int b, int col) {
    const struct arc_list *l = ctx;
    const struct ulist_entry *x = &l->ent[a], *y = &l->ent[b];
    if (col == 1) return x->size < y->size ? -1 : x->size > y->size;
    if (col == 3) return l->packed[a] < l->packed[b] ? -1 : l->packed[a] > l->packed[b];
    uint64_t tx = cal_rtc_to_epoch(&x->modified), ty = cal_rtc_to_epoch(&y->modified);
    return tx < ty ? -1 : tx > ty;
}

// Up inside an archive: the folder above in it, and from its top the
// folder the archive is in -- "zip:/home" names nothing.
static char *z_up(void *ctx, const char *dir) {
    (void)ctx;
    const char *inner;
    char *archive = arc_split(dir, &inner), *out = 0;
    if (!archive) return 0;
    if (!inner[0]) {
        out = upath_dirname(archive);
    } else if ((out = strdup(dir))) {
        char *s = strrchr(out, '/');
        if (s && s >= out + sizeof FM_ARC - 1 + strlen(archive)) {
            *s = '\0';
        } else {
            free(out);
            out = 0;
        }
    }
    free(archive);
    return out;
}

#define ARC_SRC(p) { .ctx = &g_arc[p], .list = z_list, .path = z_path, \
                    .cols = zip_cols, .ncols = 4, .cell = z_cell, .compare = z_compare, \
                    .descends = 1, .up = z_up }
static const struct uui_fileview_source g_src[2] = { ARC_SRC(0), ARC_SRC(1) };

const struct uui_fileview_source *arc_source(int pane) { return &g_src[pane ? 1 : 0]; }
int in_archive(const struct uui_fileview *fv) {
    const struct uui_fileview_source *s = uui_fileview_source(fv);
    return s == &g_src[0] || s == &g_src[1];
}

// Read-only: nothing is made, renamed, cut, pasted or deleted inside.
// Copy to the other pane (F5) EXTRACTS there, so it stays.
int arc_item_flags(int code, unsigned *out) {
    if (!in_archive(active())) return 0;
    switch (code) {
    case CMD_MENU_NEW: case CMD_MKDIR: case CMD_NEW_FILE: case CMD_CLIP_PASTE:
    case CMD_CLIP_CUT: case CMD_CLIP_COPY: case CMD_RENAME: case CMD_MOVE:
    case CMD_DELETE: case CMD_DELETE_FOREVER: case CMD_EDIT: case CMD_PROPERTIES:
    case CMD_COMPRESS_ZIP: case CMD_COMPRESS_ASK:
        *out = UUI_MI_DISABLED;
        return 1;
    case CMD_EXTRACT_SEL:
        *out = operand_count() > 0 ? 0 : UUI_MI_DISABLED;
        return 1;
    case CMD_EXTRACT_TO:
        *out = 0;
        return 1;
    default:
        return 0;
    }
}

// Where Extract puts things by default: beside the archive, in a folder
// named for it -- "x.tar.gz" -> "x", then "x (2)" if that is taken.
// Explorer's and Ark's default.
char *arc_default_dest(const char *archive) {
    char stem[ULIST_NAME_MAX];
    char *dir = uarchive_stem(archive, stem, sizeof stem) ? upath_dirname(archive) : 0;
    struct sys_stat st;
    for (int n = 1; dir && n < 100; n++) {
        char name[ULIST_NAME_MAX + 8];
        if (n == 1) snprintf(name, sizeof name, "%s", stem);
        else snprintf(name, sizeof name, "%s (%d)", stem, n);
        char *out = upath_join(dir, name);
        if (!out || sys_stat(out, &st) != 0) { free(dir); return out; }
        free(out);
    }
    free(dir);
    return 0;
}

// --- Extract here / Extract to... -----------------------------------------
//
// ON AN ARCHIVE FILE in an ordinary folder (Dolphin's and Ark's pair):
// Extract here makes the folder Extract all would, beside it, and stays
// with that folder selected; Extract to... asks for a folder and goes
// there. INSIDE an archive, Extract to... takes what is selected -- or
// everything, with nothing selected -- to the folder asked for.

static struct uui_filedialog g_xfd;   // ~20 KB: never a local
static char *g_x_archive;   // outside: the archive; NULL inside one
// INSIDE one, what the chooser was opened on: it is a window of its own,
// so the panes stay live behind it, and the answer must not extract
// whatever came to be selected meanwhile.
static char *g_x_dir;
static int g_x_pane, g_x_operands;

// The archive the selected row is, outside one (free() it) -- NULL when
// it is not.
char *selected_archive(void) {
    struct uui_fileview *fv = active();
    if (in_archive(fv) || in_bin(fv) || uui_fileview_selected_is_dir(fv)) return 0;
    const char *name = uui_fileview_selected_name(fv);
    if (!name || !is_archive_name(name)) return 0;
    return uui_fileview_selected_path(fv);
}

static void extract_to_chosen(void *ctx, const char *path) {
    (void)ctx;
    if (!path) { set_note("cancelled"); return; }
    if (g_x_archive) { do_extract_archive(g_x_archive, path, 1); return; }
    if (g_active != g_x_pane || !g_x_dir || strcmp(uui_fileview_dir(active()), g_x_dir) ||
        operand_count() != g_x_operands) {
        set_note("the selection changed -- nothing was extracted");
        return;
    }
    do_extract(g_x_operands == 0, path, 1);
}

void extract_command(int code) {
    const char *inner;
    char *archive = selected_archive();
    if (code == CMD_EXTRACT_HERE) {
        char *dest = archive ? arc_default_dest(archive) : 0;
        if (!archive) set_note("not an archive");
        else if (!dest) set_note("no free name to extract into");
        else do_extract_archive(archive, dest, 3);
        free(dest);
        free(archive);
        return;
    }
    if (uui_filedialog_is_open(&g_xfd)) { free(archive); return; }
    // Extract to...: from an archive file, or from inside one.
    free(g_x_archive);
    free(g_x_dir);
    g_x_archive = g_x_dir = 0;
    if (archive) {
        g_x_archive = strdup(archive);
    } else if ((archive = arc_split(uui_fileview_dir(active()), &inner))) {
        g_x_pane = g_active;
        g_x_dir = strdup(uui_fileview_dir(active()));
        g_x_operands = operand_count();
    } else {
        set_note("not an archive");
        return;
    }
    char *dir = upath_dirname(archive);
    free(archive);
    if (!dir || !(g_x_archive || g_x_dir)) {
        ulogf("files: out of memory opening Extract to\n");
        set_note("not enough memory");
        free(dir);
        return;
    }
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_FOLDER,
        .title = "Extract to",
        .start_dir = dir,
    };
    if (!uui_filedialog_open(g_app, &g_xfd, &o, extract_to_chosen, 0))
        set_note("could not open the folder chooser");
    free(dir);
}
