// Hex Viewer -- any file's bytes, read-only, with what they mean.
//
// **A THIN WINDOW AROUND THREE PIECES**: the byte grid is ui/uui_hexview.h,
// the data inspector ui/uui_byteinspect.h, and what the bytes ARE -- an
// ELF's headers, a WAD's lumps, a disk's partitions -- lib/ubinmap.h's
// decoders, shown in a uui_tree. This file owns which file is open, the
// commands, Find and Go to, and the status bar. Layout H3 (2026-10-31):
// the structure map on the LEFT, the grid, the inspector on the RIGHT,
// both panes toggled from the bar and a key.
//
// **THE FILE IS NEVER HELD**: lib/urange.h reads it a page at a time
// through a small cache, so a disk image of gigabytes scrolls; Find scans
// it on a worker thread with its own descriptor.
//
// **IT PARSES UNTRUSTED INPUT, SO IT PLEDGES FIRST** (`hexview_sandbox()`):
// before the first byte is read the process gives up spawning, the
// network and devices, keeps reading everywhere, and writes only under
// /home -- where Save selection as... puts what it saves.
//
// Run as `hexview [FILE] [--offset N]`.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "kpath.h"
#include "keyboard.h"
#include "sandbox_abi.h"
#include "lib/urange.h"
#include "lib/ubinmap.h"
#include "lib/ubyteval.h"
#include "lib/uclip.h"
#include "lib/udirs.h"
#include "lib/unum.h"
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/ugfx.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/umonofont.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_tree.h"
#include "ui/uui_hexview.h"
#include "ui/uui_byteinspect.h"
#include "ui/uui_findbar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_nameask.h"
#include "ui/uui_dialog.h"
#include "ui/uui_keysheet.h"

#define PATH_LEN 256

enum { ID_MENU = 1, ID_TOOLBAR, ID_FINDMODE, ID_FIND, ID_TREE, ID_GRID, ID_INSPECT, ID_DIALOG };

// ONE TABLE OF COMMAND IDS: the menus, the bar, the grid's right-click
// menu and the keys all run these.
enum {
    CMD_OPEN = 1, CMD_SAVE_SEL, CMD_EXIT,
    CMD_COPY, CMD_COPY_HEX, CMD_COPY_TEXT, CMD_COPY_C, CMD_SELECT_ALL,
    CMD_FIND, CMD_FIND_NEXT, CMD_FIND_PREV,
    CMD_GOTO, CMD_GOTO_VALUE, CMD_GO_START, CMD_GO_END,
    CMD_ROW16, CMD_ROW32, CMD_STRUCTURE, CMD_INSPECTOR, CMD_ENDIAN,
    CMD_KEYS, CMD_ABOUT, CMD_READONLY,
};

enum { USER_FIND = 1 };

static struct uapp *g_app;
static char g_path[PATH_LEN];
static struct urange g_file;
static int g_open;
static struct ubinmap g_map;
static struct ubinmap_span *g_spans;
static int g_nspans;
static struct umonofont g_mono;

static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static struct uui_statusbar g_status;
static struct uui_tree g_tree;
static struct uui_hexview g_grid;
static struct uui_byteinspect g_insp;
static struct uui_findbar g_find;
static struct uui_segmented g_findmode;
static struct uui_dialog g_dlg;
static struct uui_nameask g_goto;
static struct uui_filedialog g_fd;

static int g_show_struct = 1, g_show_insp = 1, g_find_open;
static int g_tree_pick = -1;         // the node a tree click chose, kept while the cursor is in it
static char g_stat_off[48], g_stat_sel[64], g_stat_size[40], g_stat_fmt[80], g_stat_path[160];
static char g_note[160];             // a sentence for the last thing that went wrong, in the format pane's place
static int g_pane_y, g_pane_title_h, g_grid_x, g_grid_w, g_top, g_bottom;

// --- the structure map as a tree -------------------------------------------------
//
// The map is flat with depths; the tree shows the nodes whose ancestors are
// all open. EXPANSION IS THIS APP'S (UUI_TREE_CLOSED/OPEN), a byte per map
// node, so a WAD's thousands of lumps cost nothing until "Lumps" opens.
static struct uui_tree_node *g_tnodes;
static int g_ntnodes;
static unsigned char *g_expanded;

static uint32_t tint_colour(int slot) {
    static const int roles[UBINMAP_TINTS] = { UTHEME_ACT_NAV, UTHEME_ACT_VIEW, UTHEME_ACT_CREATE, UTHEME_ACT_EDIT,
                                              UTHEME_ACT_MEDIA, UTHEME_ACT_ARRANGE, UTHEME_ACT_DANGER,
                                              UTHEME_ACT_NAV };
    return slot >= 1 && slot <= UBINMAP_TINTS ? utheme_action(roles[slot - 1]) : 0;
}

static int map_is_parent(int i) {
    return i + 1 < g_map.count && g_map.nodes[i + 1].depth > g_map.nodes[i].depth;
}

static void rebuild_tree(int keep) {
    free(g_tnodes);
    g_tnodes = 0;
    g_ntnodes = 0;
    if (g_map.count) g_tnodes = calloc((size_t)g_map.count, sizeof *g_tnodes);
    if (!g_tnodes) { uui_tree_set_nodes(&g_tree, 0, 0); return; }
    int hide_below = -1;
    for (int i = 0; i < g_map.count; i++) {
        int d = g_map.nodes[i].depth;
        if (hide_below >= 0 && d > hide_below) continue;
        hide_below = -1;
        struct uui_tree_node *t = &g_tnodes[g_ntnodes++];
        t->label = ubinmap_label(&g_map, i);
        t->note = ubinmap_value(&g_map, i)[0] ? ubinmap_value(&g_map, i) : 0;
        t->depth = d;
        t->id = i;
        if (map_is_parent(i)) {
            t->kind = g_expanded[i] ? UUI_TREE_OPEN : UUI_TREE_CLOSED;
            if (!g_expanded[i]) hide_below = d;
        }
        if (d == 0 && g_map.nodes[i].tint) {
            t->swatch_on = 1;
            t->swatch = tint_colour(g_map.nodes[i].tint);
        }
    }
    if (keep) uui_tree_set_nodes_keep(&g_tree, g_tnodes, g_ntnodes);
    else uui_tree_set_nodes(&g_tree, g_tnodes, g_ntnodes);
    // What a test clicks a row by: its place, its node and its label (a
    // small tree only -- a WAD's thousands of lumps are not listed).
    if (g_ntnodes <= 200)
        for (int k = 0; k < g_ntnodes; k++)
            ulogf("hexview: tree row %d node %d \"%s\"\n", k, g_tnodes[k].id, g_tnodes[k].label);
}

static void on_toggle(void *ctx, int id, int expand) {
    (void)ctx;
    if (id < 0 || id >= g_map.count) return;
    int sel = uui_tree_selected_id(&g_tree);
    g_expanded[id] = (unsigned char)expand;
    rebuild_tree(1);
    if (sel >= 0) uui_tree_select_id(&g_tree, sel);
}

// The tree follows the cursor: the deepest node covering it that is
// SHOWN (nothing is opened for it) -- unless a click chose a node the
// cursor is still inside.
static void follow_cursor(void) {
    if (!g_map.count) return;
    uint64_t c = g_grid.cursor;
    if (g_tree_pick >= 0 && g_tree_pick < g_map.count) {
        const struct ubinmap_node *n = &g_map.nodes[g_tree_pick];
        if (c >= n->off && c - n->off < n->len) return;
        g_tree_pick = -1;
    }
    int best = -1, best_depth = -1;
    for (int k = 0; k < g_ntnodes; k++) {
        const struct ubinmap_node *n = &g_map.nodes[g_tnodes[k].id];
        if (c >= n->off && c - n->off < n->len && n->depth >= best_depth) {
            best = g_tnodes[k].id;
            best_depth = n->depth;
        }
    }
    if (best >= 0) uui_tree_select_id(&g_tree, best);
    else g_tree.selected = -1;
}

// --- the byte grid's decoration ------------------------------------------------------

static uint32_t grid_tint(void *ctx, uint64_t off) {
    (void)ctx;
    if (!g_show_struct) return UUI_HEXVIEW_NO_TINT;
    int k = ubinmap_span_at(g_spans, g_nspans, off);
    return k >= 0 ? tint_colour(g_spans[k].tint) : UUI_HEXVIEW_NO_TINT;
}

// --- find, on a worker thread ------------------------------------------------------------
//
// THE WHOLE FILE IS SCANNED, its own descriptor and a MiB at a time, so
// the window keeps painting through a disk image. Every match is kept,
// to FIND_HITS_MAX -- past it the scan stops and says so, since "00" in
// a disk image matches billions of times.
#define FIND_HITS_MAX 100000
#define FIND_PAT_MAX  256
#define FIND_CHUNK    (1u << 20)

struct find_job {
    char path[PATH_LEN];
    uint8_t pat[FIND_PAT_MAX];
    int n;
    uint64_t size;
    atomic_int cancel;
    uint64_t *hits;
    int nhits, cap, capped, failed;
    int gen;
};

static pthread_t g_find_thread;
static struct find_job *g_job;         // running, or NULL
static int g_find_gen, g_find_goto;    // go to the first match once it lands
static uint64_t *g_hits;
static int g_nhits, g_hits_capped, g_hit_cur = -1, g_hit_len;

static int add_hit(struct find_job *j, uint64_t at) {
    if (j->nhits == j->cap) {
        if (j->cap >= FIND_HITS_MAX) { j->capped = 1; return 0; }
        int cap = j->cap ? j->cap * 2 : 256;
        if (cap > FIND_HITS_MAX) cap = FIND_HITS_MAX;
        uint64_t *h = realloc(j->hits, (size_t)cap * sizeof *h);
        if (!h) { j->capped = 1; return 0; }
        j->hits = h;
        j->cap = cap;
    }
    j->hits[j->nhits++] = at;
    return 1;
}

static void *find_main(void *arg) {
    struct find_job *j = arg;
    int fd = open(j->path, O_RDONLY);
    uint8_t *buf = malloc(FIND_CHUNK + FIND_PAT_MAX);
    if (fd < 0 || !buf) { j->failed = 1; goto done; }
    // `keep` bytes from the last chunk's tail lead each read, so a match across the seam is seen.
    size_t keep = 0;
    uint64_t base = 0;   // the file offset of buf[0]
    for (;;) {
        if (atomic_load(&j->cancel)) break;
        ssize_t r = read(fd, buf + keep, FIND_CHUNK);
        if (r <= 0) break;
        size_t have = keep + (size_t)r;
        for (size_t i = 0; i + (size_t)j->n <= have; i++) {
            if (buf[i] != j->pat[0] || memcmp(buf + i, j->pat, (size_t)j->n)) continue;
            if (!add_hit(j, base + i)) goto stop;
        }
        keep = have >= (size_t)j->n - 1 ? (size_t)j->n - 1 : have;
        memmove(buf, buf + have - keep, keep);
        base += have - keep;
    }
stop:
done:
    if (fd >= 0) close(fd);
    free(buf);
    uapp_post(g_app, USER_FIND, j->gen);
    return NULL;
}

static void find_stop(void) {
    if (!g_job) return;
    atomic_store(&g_job->cancel, 1);
    pthread_join(g_find_thread, NULL);
    free(g_job->hits);
    free(g_job);
    g_job = 0;
}

static void clear_hits(void) {
    free(g_hits);
    g_hits = 0;
    g_nhits = g_hits_capped = 0;
    g_hit_cur = -1;
    g_grid.hits = 0;
    g_grid.nhits = 0;
    g_grid.hit_cur = -1;
    g_find.matches = g_find.current = 0;
}

// The query as bytes: hex pairs, or the text's own UTF-8. -1 when it is
// not hex in the hex mode.
static int query_bytes(uint8_t *out) {
    const char *q = uui_findbar_query(&g_find);
    if (g_findmode.selected == 0) return ubyteval_parse_hex(q, out, FIND_PAT_MAX);
    int n = (int)strlen(q);
    if (n > FIND_PAT_MAX) n = FIND_PAT_MAX;
    memcpy(out, q, (size_t)n);
    return n;
}

static void find_start(struct uapp *a) {
    find_stop();
    clear_hits();
    uint8_t pat[FIND_PAT_MAX];
    int n = query_bytes(pat);
    if (!g_open || n <= 0) {
        if (n < 0) snprintf(g_note, sizeof g_note, "Type the bytes as hex pairs: C0 2B");
        uapp_redraw(a);
        return;
    }
    struct find_job *j = calloc(1, sizeof *j);
    if (!j) return;
    strlcpy(j->path, g_path, sizeof j->path);
    memcpy(j->pat, pat, (size_t)n);
    j->n = n;
    j->size = g_file.size;
    j->gen = ++g_find_gen;
    g_hit_len = n;
    g_job = j;
    g_find_goto = 1;
    snprintf(g_note, sizeof g_note, "Searching...");
    if (pthread_create(&g_find_thread, NULL, find_main, j) != 0) {
        free(j);
        g_job = 0;
        snprintf(g_note, sizeof g_note, "Find could not start a thread");
    }
    uapp_redraw(a);
}

static void select_hit(int i) {
    if (i < 0 || i >= g_nhits) return;
    g_hit_cur = i;
    g_grid.hit_cur = i;
    g_find.current = i + 1;
    uui_hexview_goto(&g_grid, g_hits[i], (uint64_t)g_hit_len);
    ulogf("hexview: find at 0x%llX (%d of %d)\n", (unsigned long long)g_hits[i], i + 1, g_nhits);
}

// The first match at or after the cursor (dir +1) or before it (-1), wrapping.
static void find_step(int dir) {
    if (!g_nhits) return;
    uint64_t c = g_grid.cursor;
    int i;
    if (g_hit_cur >= 0 && g_hits[g_hit_cur] == c) {
        i = (g_hit_cur + dir + g_nhits) % g_nhits;
    } else if (dir > 0) {
        for (i = 0; i < g_nhits && g_hits[i] < c; i++) {}
        if (i == g_nhits) i = 0;
    } else {
        for (i = g_nhits - 1; i >= 0 && g_hits[i] >= c; i--) {}
        if (i < 0) i = g_nhits - 1;
    }
    select_hit(i);
}

static void find_done(struct uapp *a, int gen) {
    if (!g_job || g_job->gen != gen) return;
    pthread_join(g_find_thread, NULL);
    struct find_job *j = g_job;
    g_job = 0;
    clear_hits();
    g_hits = j->hits;
    g_nhits = j->nhits;
    g_hits_capped = j->capped;
    g_hit_len = j->n;
    g_grid.hits = g_hits;
    g_grid.nhits = g_nhits;
    g_grid.hit_len = g_hit_len;
    g_find.matches = g_nhits;
    if (j->failed) snprintf(g_note, sizeof g_note, "Find could not read the file");
    else if (g_hits_capped) {
        snprintf(g_note, sizeof g_note, "Over %d matches: the first %d are marked", FIND_HITS_MAX, g_nhits);
        ulogf("hexview: find stopped at %d matches (the cap)\n", g_nhits);
    } else g_note[0] = '\0';
    ulogf("hexview: find done, %d match%s\n", g_nhits, g_nhits == 1 ? "" : "es");
    free(j);
    if (g_find_goto && g_nhits) find_step(+1);
    g_find_goto = 0;
    uapp_redraw(a);
}

// --- the file ----------------------------------------------------------------------------

static void close_file(void) {
    find_stop();
    clear_hits();
    if (g_open) urange_close(&g_file);
    g_open = 0;
    ubinmap_free(&g_map);
    free(g_spans);
    g_spans = 0;
    g_nspans = 0;
    free(g_expanded);
    g_expanded = 0;
    rebuild_tree(0);
    uui_hexview_set_source(&g_grid, 0, 0, 0);
    g_insp.have = 0;
    g_tree_pick = -1;
}

static int open_file(struct uapp *a, const char *path) {
    close_file();
    int rc = urange_open(&g_file, path);
    if (rc < 0) {
        snprintf(g_note, sizeof g_note, "Could not open %s", k_path_basename(path));
        ulogf("hexview: could not open %s (%d)\n", path, rc);
        if (a) uapp_set_title(a, "Hex Viewer");
        return 0;
    }
    g_open = 1;
    strlcpy(g_path, path, sizeof g_path);
    uui_hexview_set_source(&g_grid, g_file.size, urange_reader, &g_file);
    struct ubinmap_src src = { g_file.size, urange_reader, &g_file };
    ubinmap_decode(&src, &g_map);
    g_nspans = ubinmap_tints(&g_map, &g_spans);
    if (g_nspans < 0) g_nspans = 0;
    g_expanded = calloc((size_t)(g_map.count ? g_map.count : 1), 1);
    // The top level opens, unless a node is a long table (a WAD's lumps):
    // that one waits to be asked, as Okteta's and ImHex's long arrays do.
    for (int i = 0; g_expanded && i < g_map.count; i++) {
        if (g_map.nodes[i].depth != 0) continue;
        int kids = 0;
        for (int k = i + 1; k < g_map.count && g_map.nodes[k].depth > 0; k++) kids += g_map.nodes[k].depth == 1;
        g_expanded[i] = kids <= 64;
    }
    rebuild_tree(0);
    g_tree.selected = -1;
    g_note[0] = '\0';
    if (g_map.note[0]) snprintf(g_note, sizeof g_note, "%s", g_map.note);
    if (g_map.dropped) ulogf("hexview: structure: %d nodes past the cap of %d not listed\n", g_map.dropped,
                             UBINMAP_NODES_MAX);
    ulogf("hexview: opened %s size %llu format \"%s\" nodes %d spans %d\n", path,
          (unsigned long long)g_file.size, g_map.format, g_map.count, g_nspans);
    if (g_map.note[0]) ulogf("hexview: structure note: %s\n", g_map.note);
    if (a) {
        char title[PATH_LEN + 16];
        snprintf(title, sizeof title, "%s - Hex Viewer", k_path_basename(path));
        uapp_set_title(a, title);
        if (g_find_open && uui_findbar_query(&g_find)[0]) find_start(a);
    }
    return 1;
}

// --- the selection, read and copied -------------------------------------------------------

// The selection's bytes in a fresh allocation, or NULL; `*n` its length.
static uint8_t *selection_bytes(size_t *n, size_t limit) {
    uint64_t lo, len;
    if (!uui_hexview_selection(&g_grid, &lo, &len) || len > limit) return 0;
    uint8_t *b = malloc((size_t)len ? (size_t)len : 1);
    if (!b) return 0;
    *n = urange_read(&g_file, lo, b, (size_t)len);
    return b;
}

static void copy_as(int how) {
    uint64_t lo, len;
    if (!uui_hexview_selection(&g_grid, &lo, &len)) return;
    // The clipboard holds UCLIP_TEXT_MAX: a selection whose text would not
    // fit is REFUSED with a sentence, never copied short.
    size_t need = how == CMD_COPY_HEX ? ubyteval_hex_size((size_t)len)
                : how == CMD_COPY_C ? ubyteval_c_size((size_t)len) : (size_t)len + 1;
    if (len > UCLIP_TEXT_MAX || need > UCLIP_TEXT_MAX + 1) {
        snprintf(g_note, sizeof g_note, "Too much to copy as text: the clipboard holds %d KiB",
                 UCLIP_TEXT_MAX / 1024);
        ulogf("hexview: copy refused, %llu bytes\n", (unsigned long long)len);
        return;
    }
    size_t n = 0;
    uint8_t *b = selection_bytes(&n, UCLIP_TEXT_MAX);
    char *t = b ? malloc(need) : 0;
    int w = -1;
    if (t) {
        if (how == CMD_COPY_HEX) w = ubyteval_hex_text(b, n, t, need);
        else if (how == CMD_COPY_C) w = ubyteval_c_array(b, n, t, need);
        else w = ubyteval_ascii_text(b, n, t, need);
    }
    int ok = w >= 0 && uclip_set_text(t, w);
    snprintf(g_note, sizeof g_note, ok ? "Copied %llu byte%s as %s" : "The clipboard refused the copy",
             (unsigned long long)len, len == 1 ? "" : "s",
             how == CMD_COPY_HEX ? "hex" : how == CMD_COPY_C ? "a C array" : "text");
    ulogf("hexview: copied %llu bytes as %s: %s\n", (unsigned long long)len,
          how == CMD_COPY_HEX ? "hex" : how == CMD_COPY_C ? "c" : "text", ok ? "ok" : "FAILED");
    free(t);
    free(b);
}

static void save_chosen(void *ctx, const char *path) {
    struct uapp *a = ctx;
    uint64_t lo, len;
    if (!path || !uui_hexview_selection(&g_grid, &lo, &len)) { uapp_redraw(a); return; }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(g_note, sizeof g_note, "Could not write %s -- the Hex Viewer saves under /home only",
                 k_path_basename(path));
        ulogf("hexview: save refused for %s\n", path);
        uapp_redraw(a);
        return;
    }
    static uint8_t chunk[64 * 1024];
    uint64_t done = 0;
    while (done < len) {
        size_t want = len - done > sizeof chunk ? sizeof chunk : (size_t)(len - done);
        size_t got = urange_read(&g_file, lo + done, chunk, want);
        if (!got || write(fd, chunk, got) != (ssize_t)got) break;
        done += got;
    }
    close(fd);
    snprintf(g_note, sizeof g_note, done == len ? "Saved %llu bytes to %s" : "Saved only %llu bytes to %s",
             (unsigned long long)done, k_path_basename(path));
    ulogf("hexview: saved %llu of %llu bytes to %s\n", (unsigned long long)done, (unsigned long long)len, path);
    uapp_redraw(a);
}

static void save_dialog(struct uapp *a) {
    if (uui_filedialog_is_open(&g_fd)) return;
    uui_menubar_close(&g_menu);
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_SAVE,
        .title = "Save Selection As",
        .start_dir = udir_path(UDIR_DOCUMENTS),
        .initial_name = "selection.bin",
        .no_mount = 1,   // the sandbox cannot start smbfsd
    };
    uui_filedialog_open(a, &g_fd, &o, save_chosen, a);
}

static void open_chosen(void *ctx, const char *path) {
    struct uapp *a = ctx;
    if (path) open_file(a, path);
    uapp_redraw(a);
}

static void open_dialog(struct uapp *a) {
    if (uui_filedialog_is_open(&g_fd)) return;
    uui_menubar_close(&g_menu);
    char dir[PATH_LEN] = "/";
    if (g_open && !k_path_dirname(g_path, dir, sizeof dir)) strlcpy(dir, "/", sizeof dir);
    struct uui_filedialog_opts o = { .mode = UUI_FILEDIALOG_OPEN, .title = "Open File", .start_dir = dir,
                                     .no_mount = 1 };   // the sandbox cannot start smbfsd
    uui_filedialog_open(a, &g_fd, &o, open_chosen, a);
}

// --- Go to -------------------------------------------------------------------------------

static const char *goto_check(void *ctx, const char *text) {
    (void)ctx;
    uint64_t v;
    const char *why = 0;
    return ubyteval_parse_offset(text, g_grid.cursor, g_file.size, &v, &why) ? 0 : why;
}

static void goto_done(void *ctx, const char *text) {
    struct uapp *a = ctx;
    uint64_t v;
    const char *why;
    if (ubyteval_parse_offset(text, g_grid.cursor, g_file.size, &v, &why)) {
        g_tree_pick = -1;
        uui_hexview_goto(&g_grid, v, 0);
        ulogf("hexview: go to 0x%llX\n", (unsigned long long)v);
    }
    uapp_redraw(a);
}

// --- Help -------------------------------------------------------------------------------

static const struct uui_keysheet_row KEYS_MOVE[] = {
    { "Byte, row", "[Left] [Right] [Up] [Down]" },
    { "A screenful", "[Page Up] [Page Down]" },
    { "Start, end of the row", "[Home] [End]" },
    { "Start, end of the file", "[Ctrl]+[Home] [Ctrl]+[End]" },
    { "Select while moving", "hold [Shift]" },
    { "Hex or text column", "[Tab]" },
    { "Go to an offset", "[Ctrl]+[G]" },
};
static const struct uui_keysheet_row KEYS_DO[] = {
    { "Find", "[Ctrl]+[F]" },
    { "Next, previous match", "[F3] [Shift]+[F3]" },
    { "Copy", "[Ctrl]+[C]" },
    { "Select all", "[Ctrl]+[A]" },
    { "Open", "[Ctrl]+[O]" },
    { "Structure, Inspector", "[Ctrl]+[1] [Ctrl]+[2]" },
    { "16, 32 bytes a row", "[Ctrl]+[3] [Ctrl]+[4]" },
    { "Swap byte order", "[Ctrl]+[E]" },
};
static const struct uui_keysheet_group KEY_GROUPS[] = {
    { "Moving", KEYS_MOVE, (int)(sizeof KEYS_MOVE / sizeof KEYS_MOVE[0]) },
    { "Commands", KEYS_DO, (int)(sizeof KEYS_DO / sizeof KEYS_DO[0]) },
};
static struct uui_keysheet g_sheet;
static struct uui_item g_sheet_item = { .ops = &uui_keysheet_ops, .widget = &g_sheet, .name = "keys" };

static void show_dialog(struct uapp *a, int keys) {
    static const struct uui_dialog_button close_btn[] = { { "Close", 1, UUI_DLG_PRIMARY } };
    static const char *const about[] = {
        "Shows any file's bytes, and what they are:",
        "ELF programs, DOOM WADs, fonts, disks, FAT, QOI and BMP.",
        "Read-only: nothing here changes the file.",
    };
    uui_menubar_close(&g_menu);
    uui_dialog_set_bounds(&g_dlg, 0, 0, uapp_width(a), uapp_height(a));
    if (keys) {
        int w, h;
        uui_keysheet_natural_size(&g_sheet, &w, &h);
        uui_dialog_set_body(&g_dlg, &g_sheet_item, w, h);
        uui_dialog_open(&g_dlg, "Keyboard shortcuts", 0, 0, close_btn, 1, 0, 1);
    } else {
        uui_dialog_set_body(&g_dlg, 0, 0, 0);
        uui_dialog_open(&g_dlg, "About Hex Viewer", about, 3, close_btn, 1, 0, 1);
    }
}

// --- menus and the command bar ------------------------------------------------------------

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Open...", CMD_OPEN, "Ctrl+O"),
    UUI_MENU("Save selection as...", CMD_SAVE_SEL, 0),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static const struct uui_menu_item edit_items[] = {
    UUI_MENU("Copy", CMD_COPY, "Ctrl+C"),
    UUI_MENU("Copy as hex", CMD_COPY_HEX, 0),
    UUI_MENU("Copy as text", CMD_COPY_TEXT, 0),
    UUI_MENU("Copy as C array", CMD_COPY_C, 0),
    UUI_MENU_SEP,
    UUI_MENU("Select all", CMD_SELECT_ALL, "Ctrl+A"),
    UUI_MENU_SEP,
    UUI_MENU("Find...", CMD_FIND, "Ctrl+F"),
    UUI_MENU("Find next", CMD_FIND_NEXT, "F3"),
    UUI_MENU("Find previous", CMD_FIND_PREV, "Shift+F3"),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU("16 bytes per row", CMD_ROW16, "Ctrl+3"),
    UUI_MENU("32 bytes per row", CMD_ROW32, "Ctrl+4"),
    UUI_MENU_SEP,
    UUI_MENU("Structure", CMD_STRUCTURE, "Ctrl+1"),
    UUI_MENU("Inspector", CMD_INSPECTOR, "Ctrl+2"),
    UUI_MENU("Big-endian values", CMD_ENDIAN, "Ctrl+E"),
};
static const struct uui_menu_item go_items[] = {
    UUI_MENU("Go to offset...", CMD_GOTO, "Ctrl+G"),
    UUI_MENU("Go to the value at the cursor", CMD_GOTO_VALUE, "Ctrl+J"),
    UUI_MENU_SEP,
    UUI_MENU("Start of file", CMD_GO_START, "Ctrl+Home"),
    UUI_MENU("End of file", CMD_GO_END, "Ctrl+End"),
};
static const struct uui_menu_item help_items[] = {
    UUI_MENU("Keyboard shortcuts", CMD_KEYS, "F1"),
    UUI_MENU("About Hex Viewer", CMD_ABOUT, 0),
};
static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Edit", edit_items),
    UUI_SUBMENU("View", view_items),
    UUI_SUBMENU("Go", go_items),
    UUI_SUBMENU("Help", help_items),
};

// The grid's right-click rows: the same commands.
static const struct uui_menu_item grid_menu[] = {
    UUI_MENU("Copy as hex", CMD_COPY_HEX, 0),
    UUI_MENU("Copy as text", CMD_COPY_TEXT, 0),
    UUI_MENU("Copy as C array", CMD_COPY_C, 0),
    UUI_MENU_SEP,
    UUI_MENU("Go to this offset as a value", CMD_GOTO_VALUE, "Ctrl+J"),
    UUI_MENU_SEP,
    UUI_MENU("Select all", CMD_SELECT_ALL, "Ctrl+A"),
};

// COLOURED BY WHAT EACH DOES: moving about, looking, copying.
static const struct uui_toolbar_item tb_items[] = {
    { "tb-open",   "Open a file (Ctrl+O)",           CMD_OPEN,      0, 0, "Ctrl+O", UTHEME_ACT_NAV },
    UUI_TOOLBAR_SEP,
    { "tb-goto",   "Go to an offset (Ctrl+G)",       CMD_GOTO,      0, 0, "Ctrl+G", UTHEME_ACT_NAV },
    { "tb-find",   "Find bytes or text (Ctrl+F)",    CMD_FIND,      0, 0, "Ctrl+F", UTHEME_ACT_VIEW },
    UUI_TOOLBAR_SEP,
    { "tb-copy",   "Copy the selection as hex",      CMD_COPY_HEX,  "Copy as hex", 0, 0, UTHEME_ACT_EDIT },
    UUI_TOOLBAR_SEP,
    { 0,           "16 bytes per row (Ctrl+3)",      CMD_ROW16,     "16 per row", 0, "Ctrl+3", 0 },
    { 0,           "32 bytes per row (Ctrl+4)",      CMD_ROW32,     "32 per row", 0, "Ctrl+4", 0 },
    UUI_TOOLBAR_SEP,
    { "tb-tree",   "Structure (Ctrl+1)",             CMD_STRUCTURE, 0, 0, "Ctrl+1", UTHEME_ACT_ARRANGE },
    { "tb-details", "Inspector (Ctrl+2)",            CMD_INSPECTOR, 0, 0, "Ctrl+2", UTHEME_ACT_ARRANGE },
    { 0,           0,                                CMD_READONLY,  "Read-only", UUI_TB_END | UUI_TB_TEXT, 0, 0 },
};

// ONE STATE SOURCE for the menu's ticks and greying and the bar's latches.
static unsigned item_flags(int code) {
    uint64_t lo, len, o;
    int sel = g_open && uui_hexview_selection(&g_grid, &lo, &len);
    switch (code) {
    case CMD_SAVE_SEL: return sel ? 0 : UUI_MI_DISABLED;
    case CMD_COPY: case CMD_COPY_HEX: case CMD_COPY_TEXT: case CMD_COPY_C:
        return sel ? 0 : UUI_MI_DISABLED;
    case CMD_SELECT_ALL: case CMD_FIND: case CMD_GOTO: case CMD_GO_START: case CMD_GO_END:
        return g_open && g_file.size ? 0 : UUI_MI_DISABLED;
    case CMD_FIND_NEXT: case CMD_FIND_PREV: return g_nhits ? 0 : UUI_MI_DISABLED;
    case CMD_GOTO_VALUE: return uui_byteinspect_offset(&g_insp, &o) ? 0 : UUI_MI_DISABLED;
    case CMD_ROW16: return g_grid.per_row == 16 ? UUI_MI_CHECKED : 0;
    case CMD_ROW32: return g_grid.per_row == 32 ? UUI_MI_CHECKED : 0;
    case CMD_STRUCTURE: return g_show_struct ? UUI_MI_CHECKED : 0;
    case CMD_INSPECTOR: return g_show_insp ? UUI_MI_CHECKED : 0;
    case CMD_ENDIAN: return g_insp.big_endian ? UUI_MI_CHECKED : 0;
    default: return 0;
    }
}

static const char *why(int code) {
    switch (code) {
    case CMD_COPY_HEX: return g_open ? "Select some bytes first" : "Open a file first";
    case CMD_GOTO: case CMD_FIND: return "Open a file first";
    default: return 0;
    }
}

static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,     .widget = &g_menu,     .id = ID_MENU,     .name = "menu" },
    { .ops = &uui_toolbar_ops,     .widget = &g_tb,       .id = ID_TOOLBAR,  .name = "toolbar" },
    { .ops = &uui_segmented_ops,   .widget = &g_findmode, .id = ID_FINDMODE, .name = "findmode", .hidden = 1 },
    { .ops = &uui_findbar_ops,     .widget = &g_find,     .id = ID_FIND,     .name = "find", .hidden = 1 },
    { .ops = &uui_tree_ops,        .widget = &g_tree,     .id = ID_TREE,     .name = "structure" },
    { .ops = &uui_hexview_ops,     .widget = &g_grid,     .id = ID_GRID,     .name = "grid" },
    { .ops = &uui_byteinspect_ops, .widget = &g_insp,     .id = ID_INSPECT,  .name = "inspector" },
    { .ops = &uui_dialog_ops,      .widget = &g_dlg,      .id = ID_DIALOG,   .name = "dialog" },
};
#define W_FINDMODE 2
#define W_FIND 3
#define W_TREE 4
#define W_INSPECT 6

// --- layout and drawing -----------------------------------------------------------------

static uint32_t pane_bg(void) { return ugfx_blend(UTHEME_CHROME, UTHEME_WHITE, 110); }

static void layout_all(int cw, int ch) {
    int lh = ugfx_char_h();
    int mb = uui_menubar_height(&g_menu), tb = uui_toolbar_height(&g_tb), sb = uui_statusbar_height(&g_status);
    int fh = 0;
    if (g_find_open) {
        int fw;
        uui_findbar_natural_size(&g_find, &fw, &fh);
    }
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_toolbar_ops.set_geometry(&g_tb, 0, mb, cw, tb);
    uui_toolbar_set_bounds(&g_tb, 0, 0, cw, ch);
    if (g_find_open) {
        int sw, sh;
        uui_segmented_natural_size(&g_findmode, &sw, &sh);
        int p = utheme_pad();
        uui_segmented_set_geometry(&g_findmode, p * 2, mb + tb + (fh - sh) / 2);
        int fx = p * 4 + sw, fw = cw / 2 > lh * 26 ? lh * 26 : cw / 2;
        uui_findbar_set_geometry(&g_find, fx, mb + tb, fw, fh);
    }
    g_top = mb + tb + fh;
    g_bottom = ch - sb;
    uui_statusbar_set_geometry(&g_status, 0, g_bottom, cw, sb);

    int sw = g_show_struct ? lh * 15 : 0, iw = 0, ih = 0;
    if (g_show_insp) uui_byteinspect_ops.natural_size(&g_insp, &iw, &ih);
    if (sw > cw / 3) sw = cw / 3;
    if (iw > cw / 3) iw = cw / 3;
    g_pane_y = g_top;
    g_pane_title_h = lh + utheme_pad() * 3;
    uui_tree_ops.set_geometry(&g_tree, 0, g_top + g_pane_title_h, sw, g_bottom - g_top - g_pane_title_h);
    g_grid_x = sw;
    g_grid_w = cw - sw - iw;
    uui_hexview_ops.set_geometry(&g_grid, g_grid_x, g_top, g_grid_w, g_bottom - g_top);
    uui_byteinspect_ops.set_geometry(&g_insp, cw - iw, g_top, iw, g_bottom - g_top);
    g_widgets[W_TREE].hidden = !g_show_struct;
    g_widgets[W_INSPECT].hidden = !g_show_insp;
    g_widgets[W_FIND].hidden = g_widgets[W_FINDMODE].hidden = !g_find_open;
    uui_dialog_set_bounds(&g_dlg, 0, 0, cw, ch);
}

static void fmt_count(char *out, size_t cap, uint64_t n, const char *unit) {
    snprintf(out, cap, "%llu", (unsigned long long)n);
    unum_localize(out, (unsigned long)cap, UNUM_GROUP);
    size_t k = strlen(out);
    snprintf(out + k, cap - k, " %s", unit);
}

// The panes and the inspector from the grid's state, and the log line a
// test reads, once per change.
static void refresh(void) {
    uint64_t lo = 0, len = 0, c = g_grid.cursor;
    int sel = g_open && uui_hexview_selection(&g_grid, &lo, &len);
    if (g_open && g_file.size) {
        snprintf(g_stat_off, sizeof g_stat_off, "Offset 0x%llX (%llu)", (unsigned long long)c, (unsigned long long)c);
        uint8_t b[8];
        int got = (int)urange_read(&g_file, c, b, sizeof b);
        uui_byteinspect_set(&g_insp, c, b, got, g_file.size);
    } else {
        g_stat_off[0] = '\0';
        g_insp.have = 0;
    }
    if (sel) {
        if (len == 1) snprintf(g_stat_sel, sizeof g_stat_sel, "Selected 1 byte, 0x%llX", (unsigned long long)lo);
        else snprintf(g_stat_sel, sizeof g_stat_sel, "Selected %llu bytes, 0x%llX-0x%llX", (unsigned long long)len,
                      (unsigned long long)lo, (unsigned long long)(lo + len - 1));
        uint8_t p[UUI_BYTEINSPECT_PREVIEW];
        int got = (int)urange_read(&g_file, lo, p, len < sizeof p ? (size_t)len : sizeof p);
        uui_byteinspect_set_selection(&g_insp, 1, lo, len, p, got);
    } else {
        snprintf(g_stat_sel, sizeof g_stat_sel, g_open ? "Nothing selected" : "");
        uui_byteinspect_set_selection(&g_insp, 0, 0, 0, 0, 0);
    }
    if (g_open) fmt_count(g_stat_size, sizeof g_stat_size, g_file.size, g_file.size == 1 ? "byte" : "bytes");
    else g_stat_size[0] = '\0';
    if (g_note[0]) snprintf(g_stat_fmt, sizeof g_stat_fmt, "%s", g_note);
    else snprintf(g_stat_fmt, sizeof g_stat_fmt, "%s", !g_open ? "No file" : g_map.format[0] ? g_map.format : "Data");
    g_stat_path[0] = '\0';
    if (g_open) ubinmap_path(&g_map, c, g_stat_path, sizeof g_stat_path);
    follow_cursor();

    static char last[256];
    char now[256];
    snprintf(now, sizeof now, "hexview: state cursor %llu sel %llu %llu col %s per_row %d top %llu tree %d path \"%s\"\n",
             (unsigned long long)c, (unsigned long long)(sel ? lo : 0), (unsigned long long)(sel ? len : 0),
             g_grid.text_col ? "text" : "hex", g_grid.per_row, (unsigned long long)g_grid.top,
             uui_tree_selected_id(&g_tree), g_stat_path);
    if (strcmp(now, last)) {
        strlcpy(last, now, sizeof last);
        ulog(now);
    }
}

static void draw_pane_title(struct ugfx_surface *s) {
    if (!g_show_struct) return;
    int w = g_tree.w, p = utheme_pad() * 2;
    uint32_t bg = pane_bg();
    ugfx_fill_rect(s, 0, g_pane_y, w, g_pane_title_h, bg);
    const char *t = !g_open ? "No file" : g_map.format[0] ? g_map.format : "No structure known";
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, p, g_pane_y + (g_pane_title_h - ugfx_char_h()) / 2, w - 2 * p, t,
                             g_map.format[0] ? UTHEME_TEXT : utheme_current()->text_muted, bg);
    ugfx_set_font(was);
    ugfx_fill_rect(s, w - 1, g_pane_y, 1, g_bottom - g_pane_y, UTHEME_SEPARATOR);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    int cw = d->surface->w, ch = d->surface->h;
    const struct ugfx_font *mono = umonofont_get(&g_mono, 0, "hexview");
    g_grid.font = mono;
    g_insp.mono = mono;
    g_tree.note_font = mono;
    g_grid.focused = uapp_focused(a) && !g_find.field.active;
    layout_all(cw, ch);
    ugfx_fill_rect(d->surface, 0, g_top, cw, g_bottom - g_top, UTHEME_WHITE);
    draw_pane_title(d->surface);
    refresh();
    // The line between the bars and the content.
    ugfx_fill_rect(d->surface, 0, g_top - 1, cw, 1, UTHEME_SEPARATOR);
    uui_statusbar_draw(d->surface, &g_status);
    uapp_log_layout(a, "hexview");
}

static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    if (g_open) return;
    // An empty window says what to do, in the grid's place.
    const char *t = "Open a file to see its bytes: File > Open (Ctrl+O)";
    int w = ugfx_text_width(t);
    ugfx_draw_string_clipped(d->surface, g_grid_x + (g_grid_w - w) / 2, g_top + (g_bottom - g_top) / 2, g_grid_w,
                             t, utheme_current()->text_muted, UTHEME_WHITE);
}

// --- commands ----------------------------------------------------------------------------

static void open_find(struct uapp *a) {
    if (!g_open) return;
    g_find_open = 1;
    uui_findbar_activate(&g_find);
    layout_all(uapp_width(a), uapp_height(a));
}

static void do_command(struct uapp *a, int code) {
    uint64_t o;
    if (item_flags(code) & UUI_MI_DISABLED) return;
    // dispatch-ok: THE SET IS THE APP'S OWN CMD_* LIST, which the menus,
    // the command bar, the grid's menu and the key table all name.
    switch (code) {
    case CMD_OPEN: open_dialog(a); return;
    case CMD_SAVE_SEL: save_dialog(a); return;
    case CMD_EXIT: uapp_quit(a, 0); return;
    case CMD_COPY: copy_as(g_grid.text_col ? CMD_COPY_TEXT : CMD_COPY_HEX); break;
    case CMD_COPY_HEX: case CMD_COPY_TEXT: case CMD_COPY_C: copy_as(code); break;
    case CMD_SELECT_ALL:
        g_grid.anchor = 0;
        g_grid.cursor = g_file.size - 1;
        g_grid.has_sel = 1;
        g_grid.changed = 1;
        break;
    case CMD_FIND: open_find(a); break;
    case CMD_FIND_NEXT: find_step(+1); break;
    case CMD_FIND_PREV: find_step(-1); break;
    case CMD_GOTO:
        if (uui_nameask_is_open(&g_goto)) return;
        uui_menubar_close(&g_menu);
        uui_nameask_open(&g_goto, a, "Go to offset", "Offset: 0x2BC0, 11200, +16 or -0x10", "0x", "Go",
                         goto_check, goto_done, a);
        return;
    case CMD_GOTO_VALUE:
        if (uui_byteinspect_offset(&g_insp, &o)) {
            g_tree_pick = -1;
            uui_hexview_goto(&g_grid, o, 0);
            ulogf("hexview: go to the value 0x%llX\n", (unsigned long long)o);
        }
        break;
    case CMD_GO_START: uui_hexview_key(&g_grid, KEY_HOME, KEY_MOD_CTRL); break;
    case CMD_GO_END: uui_hexview_key(&g_grid, KEY_END, KEY_MOD_CTRL); break;
    case CMD_ROW16: uui_hexview_set_per_row(&g_grid, 16); ulog("hexview: 16 per row\n"); break;
    case CMD_ROW32: {
        uui_hexview_set_per_row(&g_grid, 32);
        // Twice the bytes need twice the room: the window grows to show
        // the text column, as far as the screen lets it.
        int need = uapp_width(a) - g_grid_w + uui_hexview_width_for(&g_grid, 32);
        if (need > uapp_width(a)) uapp_resize(a, need, uapp_height(a));
        ulog("hexview: 32 per row\n");
        break;
    }
    case CMD_STRUCTURE: g_show_struct = !g_show_struct; ulogf("hexview: structure %s\n", g_show_struct ? "on" : "off"); break;
    case CMD_INSPECTOR: g_show_insp = !g_show_insp; ulogf("hexview: inspector %s\n", g_show_insp ? "on" : "off"); break;
    case CMD_ENDIAN: g_insp.big_endian = !g_insp.big_endian; break;
    case CMD_KEYS: show_dialog(a, 1); break;
    case CMD_ABOUT: show_dialog(a, 0); break;
    default: return;
    }
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) { do_command(a, code); }

static void on_widget(struct uapp *a, int id, int reason) {
    switch (id) {
    case ID_MENU: {
        int code = uui_menubar_take_code(&g_menu);
        if (code > 0) do_command(a, code);
        break;
    }
    case ID_TOOLBAR: {
        int code = uui_toolbar_take_code(&g_tb);
        if (code > 0) do_command(a, code);
        break;
    }
    case ID_GRID: {
        // A click in the grid takes the keys back from the find field.
        if (reason == UUI_REASON_PRESS || reason == UUI_REASON_RELEASE) {
            uui_textbox_set_active(&g_find.field, 0);
            g_tree_pick = -1;
        }
        int code = uui_hexview_take_cmd(&g_grid);
        if (code > 0) do_command(a, code);
        break;
    }
    case ID_TREE: {
        int sel = uui_tree_selected_id(&g_tree);
        if (reason == UUI_REASON_RELEASE && sel >= 0 && sel < g_map.count) {
            const struct ubinmap_node *n = &g_map.nodes[sel];
            g_tree_pick = sel;
            uui_hexview_goto(&g_grid, n->off, n->len);
            ulogf("hexview: structure picked %d \"%s\" 0x%llX+%llu\n", sel, ubinmap_label(&g_map, sel),
                  (unsigned long long)n->off, (unsigned long long)n->len);
        }
        break;
    }
    case ID_INSPECT:
        switch (uui_byteinspect_take(&g_insp)) {
        case UUI_BI_ENDIAN: ulogf("hexview: %s-endian\n", g_insp.big_endian ? "big" : "little"); break;
        case UUI_BI_GOTO: do_command(a, CMD_GOTO_VALUE); break;
        case UUI_BI_COPY_HEX: do_command(a, CMD_COPY_HEX); break;
        case UUI_BI_COPY_C: do_command(a, CMD_COPY_C); break;
        case UUI_BI_SAVE: do_command(a, CMD_SAVE_SEL); break;
        default: break;
        }
        break;
    case ID_FINDMODE:
        if (reason == UUI_REASON_RELEASE) find_start(a);
        break;
    case ID_FIND:
        switch (uui_findbar_take(&g_find)) {
        case UUI_FIND_CHANGED: find_start(a); break;
        case UUI_FIND_NEXT: find_step(+1); break;
        case UUI_FIND_PREV: find_step(-1); break;
        case UUI_FIND_CLOSE:
            g_find_open = 0;
            find_stop();
            clear_hits();
            g_note[0] = '\0';
            break;
        default: break;
        }
        break;
    case ID_DIALOG:
        uui_dialog_take_code(&g_dlg);
        break;
    default: break;
    }
    uapp_redraw(a);
}

// THE KEYS ARE A TABLE of command ids -- the ones the menu and the bar run.
// A Ctrl+letter arrives as its control code WITH KEY_MOD_CTRL (Ctrl+O is
// 0x0F), so `mods` is matched exactly: Ctrl+J (0x0A) is not Enter.
#define C KEY_MOD_CTRL
static const struct { int key; unsigned mods; int cmd; } KEYS[] = {
    { 0x0F, C, CMD_OPEN },          { 0x03, C, CMD_COPY },        { 0x01, C, CMD_SELECT_ALL },
    { 0x06, C, CMD_FIND },          { KEY_F3, 0, CMD_FIND_NEXT }, { KEY_F3, KEY_MOD_SHIFT, CMD_FIND_PREV },
    { 0x07, C, CMD_GOTO },          { 0x0A, C, CMD_GOTO_VALUE },  { 0x05, C, CMD_ENDIAN },
    { '1', C, CMD_STRUCTURE },      { '2', C, CMD_INSPECTOR },
    { '3', C, CMD_ROW16 },          { '4', C, CMD_ROW32 },
    { KEY_F1, 0, CMD_KEYS },
};
#undef C

static void on_key(struct uapp *a, int key, unsigned mods) {
    int code;
    if (uui_dialog_is_open(&g_dlg)) {
        uui_dialog_key(&g_dlg, key);
        uui_dialog_take_code(&g_dlg);
        uapp_redraw(a);
        return;
    }
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code > 0) do_command(a, code);
        uapp_redraw(a);
        return;
    }
    // The find field has the keys while it is active -- typing, its own
    // Ctrl+C, Enter, Esc -- all but Find's next and previous and Help.
    if (g_find_open && g_find.field.active && key != KEY_F3 && key != KEY_F1 &&
        uui_findbar_key(&g_find, key, mods)) {
        on_widget(a, ID_FIND, UUI_REASON_KEY);
        return;
    }
    for (unsigned k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++)
        if (KEYS[k].key == key && KEYS[k].mods == (mods & (KEY_MOD_SHIFT | KEY_MOD_CTRL))) {
            do_command(a, KEYS[k].cmd);
            return;
        }
    if (uui_key_is_shortcut(key, mods)) return;
    if (g_open && uui_hexview_key(&g_grid, key, mods)) {
        g_tree_pick = -1;
        uapp_redraw(a);
    }
}

static int on_user(struct uapp *a, int a0, int a1) {
    if (a0 == USER_FIND) { find_done(a, a1); return 1; }
    return 0;
}

static int on_tick(struct uapp *a) {
    (void)a;
    return uui_toolbar_tick(&g_tb);
}

static int on_close(struct uapp *a) {
    (void)a;
    find_stop();
    return 1;
}

// The window exists: the find worker can post to it, and a file named on
// the command line titles it.
static void on_open(struct uapp *a) {
    g_app = a;
    if (g_open) {
        char title[PATH_LEN + 16];
        snprintf(title, sizeof title, "%s - Hex Viewer", k_path_basename(g_path));
        uapp_set_title(a, title);
    }
}

static void on_size(int *w, int *h) {
    int c = ugfx_char_h();
    *w = c * 76;
    *h = c * 38;
}

// A file to read is untrusted input. Give up what a viewer does not do --
// spawning, the network, devices -- before the first byte, and refuse to
// run if that is not possible. It reads everywhere (any file it is asked
// to open, its fonts and settings), writes only beneath /home (Save
// selection as...), and keeps the window and the shared memory the
// desktop and the clipboard need.
static int hexview_sandbox(void) {
    if (unveil("/", "r") != 0) return -1;
    if (unveil("/home", "rwc") != 0) return -1;
    if (unveil(UNVEIL_SHM_PREFIX, "r") != 0) return -1;
    if (unveil(NULL, NULL) != 0) return -1;
    return pledge("stdio rpath wpath cpath shm window settings query", NULL);
}

int main(int argc, char **argv) {
    const char *file = 0, *offset = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--offset") && i + 1 < argc) offset = argv[++i];
        else if (argv[i][0] != '-' || !argv[i][1]) file = argv[i];
    }
    if (hexview_sandbox() != 0) {
        fprintf(stderr, "hexview: could not enter its sandbox -- not reading files\n");
        return 2;
    }
    if (!ugfx_font_init()) return 2;

    uui_menubar_init(&g_menu, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    g_tb.why = why;
    g_tb.accent_latch = 1;
    g_tb.overflow = 1;
    uui_statusbar_init(&g_status);
    g_status.panes[0] = (struct uui_status_pane){ .text = g_stat_off, .chars = 22 };
    g_status.panes[1] = (struct uui_status_pane){ .text = g_stat_sel, .chars = 34 };
    g_status.panes[2] = (struct uui_status_pane){ .text = g_stat_size, .chars = 16 };
    g_status.panes[3] = (struct uui_status_pane){ .text = g_stat_fmt, .chars = 26 };
    g_status.panes[4] = (struct uui_status_pane){ .text = g_stat_path, .chars = 0 };
    g_status.count = 5;
    uui_tree_init(&g_tree, 0, 0, 0, 0, 0, 0);
    uui_tree_set_on_toggle(&g_tree, on_toggle, 0);
    g_tree.sel_style = UUI_SEL_ROUNDED;
    g_tree.bg = pane_bg();
    uui_hexview_init(&g_grid);
    g_grid.tint = grid_tint;
    g_grid.menu = grid_menu;
    g_grid.menu_count = (int)(sizeof grid_menu / sizeof grid_menu[0]);
    g_grid.menu_flags = item_flags;
    uui_byteinspect_init(&g_insp);
    g_insp.save_ok = 1;
    uui_findbar_init(&g_find);
    g_find.field.placeholder = "Find bytes: C0 2B";
    static const char *const MODES[] = { "Hex", "Text" };
    uui_segmented_init(&g_findmode, MODES, 2, 0);
    uui_dialog_init(&g_dlg);
    uui_keysheet_init(&g_sheet, KEY_GROUPS, (int)(sizeof KEY_GROUPS / sizeof KEY_GROUPS[0]), 2);

    if (file) {
        char abs[PATH_LEN];
        if (file[0] != '/' && getcwd(abs, sizeof abs) && strlen(abs) + strlen(file) + 2 < sizeof abs) {
            strlcat(abs, abs[strlen(abs) - 1] == '/' ? "" : "/", sizeof abs);
            strlcat(abs, file, sizeof abs);
            file = abs;
        }
        if (open_file(0, file) && offset) {
            uint64_t v;
            const char *why_not;
            if (ubyteval_parse_offset(offset, 0, g_file.size, &v, &why_not)) uui_hexview_goto(&g_grid, v, 0);
            else snprintf(g_note, sizeof g_note, "--offset %s: %s", offset, why_not);
        }
    }

    struct uapp_desc desc = {
        .title        = "Hex Viewer",
        .app_id       = "hexview",
        .flags        = UAPP_RESIZABLE,
        .min_w        = 560,
        .min_h        = 360,
        .tick_ms      = 250,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_size      = on_size,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_draw_over = on_draw_over,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
        .on_tick      = on_tick,
        .on_user      = on_user,
        .on_close     = on_close,
    };
    return uapp_run(&desc);
}
