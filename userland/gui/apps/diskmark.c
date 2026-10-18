// Disk Mark -- sequential and random throughput, CrystalDiskMark's four
// readings with GNOME Disks' graph of the run (mockup D2, 2026-10-03):
// a command bar (Run, Stop, the size, the location), four meter cards, the
// throughput over the run with its four phases marked, and every run
// kept in a history compared with the one before.
//
// **IT DOES NO I/O ITSELF.** It spawns /bin/diskbench and polls the
// report that writes. Running the passes in on_tick, sliced across
// frames, was the first version and was wrong: the slice was bounded in
// time but one "transfer" was not, and the window read "(Not
// Responding)" for a pass. A child is also the shape CrystalDiskMark's
// worker and the File Manager's /bin/cp take.
//
// THE LABELS SAY WHAT THIS OS ACTUALLY DOES: plain `SEQ`, not CDM's
// `SEQ1M`, because the request size is whatever SYS_WRITE_MAX is, and
// the status bar names it.
#include <stdint.h>
#include "rt/sys.h"
#include "syscall_abi.h"   // SYS_WRITE_MAX -- named in the status bar
#include "kpath.h"
#include "ui/ulog.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui_meter.h"
#include "ui/uui_chart.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_table.h"
#include "ui/uui_dialog.h"
#include "ui/uui_sizefield.h"
#include "ui/uui_droprows.h"
#include "ui/uui_filedialog.h"
#include "lib/uprefs.h"
#include "lib/uvolinfo.h"
#include "lib/human.h"
#include "lib/uclip.h"
#include "lib/udate.h"
#include "lib/udirs.h"
#include <stdio.h>
#include "lib/unum.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <locale.h>
#include <time.h>
#include <sys/stat.h>
#include "tmppath.h"
#include "keyboard.h"

// TMP_VOLATILE: a report the GUI polls and then deletes, wanted for
// seconds and never across a boot.
static const char *result_path(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_VOLATILE, "diskmark.out");
    return p;
}
#define RESULT_PATH result_path()
// TMP_PERSISTENT, never TMP_VOLATILE: a disk benchmark pointed at the
// RAM scratch directory measures memcpy and reports a number that is
// enormous and meaningless, with nothing about it looking wrong.
static const char *root_work_path(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "diskmark.tmp");
    return p;
}
#define BENCH_PATH   "/bin/diskbench"
#define HISTORY_DIR  "/var/lib/diskmark"
#define HISTORY_PATH "/var/lib/diskmark/history"

// Display order is CrystalDiskMark's read-then-write; diskbench runs
// them write-first and names each result, so the orders never agree.
#define P_SEQ_READ  0
#define P_SEQ_WRITE 1
#define P_RND_READ  2
#define P_RND_WRITE 3
#define PROFILES    4

// What the log and tools/diskmark_test.py call them.
static const char *PROFILE_NAME[PROFILES] = {
    "SEQ Q1T1 READ", "SEQ Q1T1 WRITE", "RND4K Q1T1 READ", "RND4K Q1T1 WRITE",
};
// What the cards say: Q1T1 is the same for all four, so it is in the
// status bar once rather than on every card.
static const char *CAPTION[PROFILES] = { "SEQ READ", "SEQ WRITE", "RND4K READ", "RND4K WRITE" };
static const char *WIRE_NAME[PROFILES] = { "SEQ-read", "SEQ-write", "RND4K-read", "RND4K-write" };
static const char *PHASE_LABEL[PROFILES] = { "SEQ read", "SEQ write", "RND4K read", "RND4K write" };
static int is_write(int p) { return p == P_SEQ_WRITE || p == P_RND_WRITE; }

// The presets. A size that is none of them is the CUSTOM entry, listed
// after them and before "Custom...".
static const int PRESET_MIB[] = { 16, 64, 256, 512, 1024 };
#define PRESETS 5
#define CUSTOM_MAX_MIB (1 << 24)   // 16 TiB; the volume's room bounds it first

// The remembered choices, in the app's own options file
// (docs/conventions/gui.md) -- a size or a place picked once is the one
// next time. `location` is a volume's mount point or one of the folders.
#define DISKMARK_CONF "/etc/diskmark.conf"
#define FOLDERS_MAX 3
#define DIR_MAX UUI_DROPROWS_LABEL
struct dm_prefs { int size_mib; char location[DIR_MAX]; char folder[FOLDERS_MAX][DIR_MAX]; };
static const struct upref PREF_KEYS[] = {
    UPREF_INT_KEY("size", struct dm_prefs, size_mib, 1, CUSTOM_MAX_MIB, 16),
    UPREF_TEXT_KEY("location", struct dm_prefs, location, "", 0),
    UPREF_TEXT_KEY("folder1", struct dm_prefs, folder[0], "", 0),
    UPREF_TEXT_KEY("folder2", struct dm_prefs, folder[1], "", 0),
    UPREF_TEXT_KEY("folder3", struct dm_prefs, folder[2], "", 0),
};
static const struct uprefs PREFS = { DISKMARK_CONF, PREF_KEYS, 5 };
// Static, and so is every copy of it: 1 KiB of paths against the ring-3
// frame budget of 2 KiB.
static struct dm_prefs g_prefs, g_prefs_was;

static void prefs_save(void) {
    uprefs_save(&PREFS, &g_prefs, &g_prefs_was);
    g_prefs_was = g_prefs;
}

enum { ST_IDLE, ST_RUNNING, ST_STOPPING, ST_DONE, ST_FAILED };

static int g_state = ST_IDLE;
static int g_pid = -1;
static int g_running_p = -1;          // the profile being measured now
static uint64_t g_result[PROFILES];   // milli-MB/s
static uint64_t g_iops[PROFILES], g_lat_us[PROFILES];
static int g_run_mib;                 // what the CURRENT readings were taken at
static char g_run_vol[32];            // the history's field widths: %31s ...
static char g_run_dev[16];            // ... and %15s
static time_t g_run_when;

static char g_value[PROFILES][24];
static char g_detail[PROFILES][48];
static char g_status[DIR_MAX + 96];
static char g_size_pane[16], g_call_pane[24], g_free_pane[24], g_when_pane[24];

// The child's output, re-read whole each tick. Static: the ring-3 frame
// budget is 2 KiB.
static char g_out[4096];

// THE BIG NUMBER'S FONT, loaded once; a failed load falls back to the
// session bold.
static struct ugfx_font g_big;
static int g_big_ok;

// --- where the test file goes ----------------------------------------
//
// THE LOCATION: a volume, or a folder the person chose. The volumes are
// ANY MOUNTED, PERSISTENT, WRITABLE filesystem. A RAM-only one is
// refused (it would measure memcpy) and a read-only one -- /boot by
// policy (kernel/fs/mount.c) -- cannot hold the scratch file. A folder
// is held to the same rules by folder_usable(), except that a network
// share is allowed: it measures the share, and says so.
#define VOL_MAX 6
struct vol { char point[64]; char label[80]; char dev[16]; char fs[16]; int root; };
static struct vol g_vols[VOL_MAX];
static const char *g_vol_items[VOL_MAX];
static int g_nvols;

static void scan_volumes(void) {
    g_nvols = 0;
    struct query_fsinfo fs;
    for (unsigned i = 0; i < 8 && g_nvols < VOL_MAX; i++) {
        if (sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) < (int)sizeof fs) break;
        if (!(fs.flags & QUERY_FS_MOUNTED) || !(fs.flags & QUERY_FS_PERSISTENT) ||
            (fs.flags & QUERY_FS_RDONLY))
            continue;
        struct vol *v = &g_vols[g_nvols];
        strlcpy(v->point, fs.point, sizeof v->point);
        strlcpy(v->dev, fs.device[0] ? fs.device : "-", sizeof v->dev);
        strlcpy(v->fs, fs.name, sizeof v->fs);
        v->root = (fs.flags & QUERY_FS_ROOT) != 0;
        snprintf(v->label, sizeof v->label, "%s on %s", v->point, v->dev);
        g_vol_items[g_nvols] = v->label;
        g_nvols++;
    }
}

// The volumes, then the chosen folders, then "Choose folder...".
static struct uui_dropdown g_vol;
static struct uui_droprows g_locs;
static struct uui_filedialog g_chooser;   // ~20 KB: file scope, never a local

static const struct vol *chosen_vol(void) {
    int i = g_locs.sel_index;
    return g_locs.sel_kind == UUI_DROPROW_FIXED && i >= 0 && i < g_nvols ? &g_vols[i] : NULL;
}
static const char *chosen_folder(void) {
    return g_locs.sel_kind == UUI_DROPROW_OWN ? uui_droprows_own(&g_locs, g_locs.sel_index) : NULL;
}

// The scratch file: the root keeps the persistent temp directory, which
// is this app's alone; a volume's top or a chosen folder is the
// PERSON'S, so the name there is this run's own -- diskbench removes
// whatever is at its path first, and a fixed name would take a file of
// theirs with it. "" when there is no location.
static const char *work_path(void) {
    static char p[DIR_MAX + 32];
    const struct vol *v = chosen_vol();
    const char *dir = chosen_folder();
    if (v && v->root) return root_work_path();
    if (v) dir = v->point;
    char name[32];
    snprintf(name, sizeof name, "diskmark-%d.tmp", sys_getpid());
    if (!dir || !k_path_join(dir, name, p, sizeof p)) p[0] = 0;
    return p;
}
static char g_work[DIR_MAX + 32];   // the one the running child was given

// Can the test file go in `dir`? 1, or 0 with the reason in `why`.
static int folder_usable(const char *dir, char *why, int cap) {
    struct sys_stat st;
    struct uvolinfo vi;
    const char *says = NULL;
    if (sys_stat(dir, &st) != 0 || !st.is_dir) says = "no longer exists";
    else if (!uvolinfo_of(dir, &vi)) says = "is not on a mounted volume";
    else if (vi.readonly) says = "is read-only";
    else if (!vi.persistent && !vi.remote) says = "is in memory, not on a disk";
    if (!says) return 1;
    snprintf(why, (unsigned)cap, "%s %s -- choose another folder.", dir, says);
    return 0;
}

// The volume the chosen location is on; 0 when there is none, or its
// size is not known (a size that cannot be judged is not refused).
static int chosen_volinfo(struct uvolinfo *vi) {
    const char *w = work_path();
    return w[0] && uvolinfo_of(w, vi) && vi->total;
}
static int chosen_is_share(void) {
    struct uvolinfo vi;
    const char *w = work_path();
    return w[0] && uvolinfo_of(w, &vi) && vi.remote;
}

// The history's key for the location: its volume's mount point, as one
// word of its field's width. A folder is measured as its volume.
static void chosen_point(char *out, int cap) {
    struct uvolinfo vi;
    const struct vol *v = chosen_vol();
    const char *w = work_path();
    if (w[0] && uvolinfo_of(w, &vi)) strlcpy(out, vi.point, (unsigned)cap);
    else strlcpy(out, v ? v->point : "/", (unsigned)cap);
    for (char *q = out; *q; q++) if (*q == ' ') *q = '_';
}

static void location_save(void) {
    const struct vol *v = chosen_vol();
    const char *f = chosen_folder();
    strlcpy(g_prefs.location, f ? f : v ? v->point : "", sizeof g_prefs.location);
    for (int i = 0; i < FOLDERS_MAX; i++) {
        const char *o = uui_droprows_own(&g_locs, i);
        strlcpy(g_prefs.folder[i], o ? o : "", sizeof g_prefs.folder[i]);
    }
    prefs_save();
}

// The remembered folders that still pass, oldest first so the newest
// ends on top; then the remembered location, else the root volume.
static void location_load(void) {
    uui_droprows_init(&g_locs, &g_vol, FOLDERS_MAX, "Choose folder...");
    uui_droprows_set_fixed(&g_locs, g_vol_items, g_nvols);
    int dropped = 0;
    for (int i = FOLDERS_MAX - 1; i >= 0; i--) {
        char why[DIR_MAX + 64];
        if (!g_prefs.folder[i][0]) continue;
        if (folder_usable(g_prefs.folder[i], why, sizeof why)) {
            uui_droprows_add_own(&g_locs, g_prefs.folder[i]);
        } else {
            ulogf("diskmark: folder dropped: %s\n", why);
            dropped = 1;
        }
    }
    int at = uui_droprows_find_own(&g_locs, g_prefs.location);
    if (at >= 0) {
        uui_droprows_select(&g_locs, UUI_DROPROW_OWN, at);
    } else {
        int pick = -1;
        for (int i = 0; i < g_nvols; i++) {
            if (!strcmp(g_vols[i].point, g_prefs.location)) pick = i;
            if (pick < 0 && g_vols[i].root) pick = i;
        }
        if (pick < 0 && g_nvols) pick = 0;
        if (pick >= 0) uui_droprows_select(&g_locs, UUI_DROPROW_FIXED, pick);
    }
    if (dropped) location_save();
}

// --- widgets ----------------------------------------------------------

#define ID_MENU    1
#define ID_TOOLBAR 2
#define ID_SIZE    3
#define ID_VOL     4
#define ID_HIST    5
#define ID_STATUS  6
#define ID_DIALOG  7
#define ID_SIZEFIELD 8

#define CMD_RUN     1
#define CMD_STOP    2
#define CMD_COPY    3
#define CMD_HISTORY 4
#define CMD_EXIT    5
#define CMD_SIZE_0  10   // ..14, the presets
#define CMD_CUSTOM  20
#define CMD_FOLDER  21
#define DLG_OK      1
#define DLG_CANCEL  2

static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static struct uui_dropdown g_size;
static struct uui_dialog g_dlg;
static struct uui_sizefield g_sf;
static struct uui_item g_sf_item = { .ops = &uui_sizefield_ops, .widget = &g_sf,
                                     .id = ID_SIZEFIELD, .name = "customsize" };
static struct uui_meter g_meter[PROFILES];
static struct uui_chart g_chart;
static struct uui_table g_hist;
static struct uui_statusbar g_sb;
static int g_show_hist;

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Copy results", CMD_COPY, "Ctrl-C"),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static const struct uui_menu_item test_items[] = {
    UUI_MENU("Run", CMD_RUN, "F5"),
    UUI_MENU("Stop", CMD_STOP, "Esc"),
    UUI_MENU_SEP,
    UUI_MENU("16 MiB",  CMD_SIZE_0,     0),
    UUI_MENU("64 MiB",  CMD_SIZE_0 + 1, 0),
    UUI_MENU("256 MiB", CMD_SIZE_0 + 2, 0),
    UUI_MENU("512 MiB", CMD_SIZE_0 + 3, 0),
    UUI_MENU("1 GiB",   CMD_SIZE_0 + 4, 0),
    UUI_MENU("Custom size...", CMD_CUSTOM, 0),
    UUI_MENU_SEP,
    UUI_MENU("Choose folder...", CMD_FOLDER, 0),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU("History", CMD_HISTORY, "F9"),
};
static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Test", test_items),
    UUI_SUBMENU("View", view_items),
};

// Run and Stop at the left, labelled; the size and the volume sit
// between (placed by layout()); Copy and History at the right.
static const struct uui_toolbar_item tb_items[] = {
    { "tb-play",    "Run (F5)",          CMD_RUN,     "Run",  0,          "F5",     UTHEME_ACT_CREATE },
    { "tb-stop",    "Stop (Esc)",        CMD_STOP,    "Stop", 0,          "Esc",    UTHEME_ACT_DANGER },
    { "tb-copy",    "Copy the results",  CMD_COPY,    0,      UUI_TB_END, "Ctrl-C", UTHEME_ACT_EDIT },
    { "tb-history", "History (F9)",      CMD_HISTORY, 0,      0,          "F9",     UTHEME_ACT_VIEW },
};
#define TB_STOP 1
#define TB_COPY 2

static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,     .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,       .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_table_ops,     .widget = &g_hist,     .id = ID_HIST,    .name = "history", .hidden = 1 },
    { .ops = &uui_statusbar_ops, .widget = &g_sb,       .id = ID_STATUS,  .name = "status" },
    // AFTER everything they can open over: their popups are drawn last.
    { .ops = &uui_dropdown_ops,  .widget = &g_size,     .id = ID_SIZE,    .name = "size" },
    { .ops = &uui_dropdown_ops,  .widget = &g_vol,      .id = ID_VOL,     .name = "location" },
    // A container, drawn in the items pass: last, so it is on top.
    { .ops = &uui_dialog_ops,    .widget = &g_dlg,      .id = ID_DIALOG,  .name = "dialog" },
};

// --- the size -----------------------------------------------------------
//
// The dropdown's rows: the presets, the custom size when there is one,
// then "Custom...". The size itself is g_size_mib; the rows only show it.
static int g_size_mib = 16;
static int g_custom_mib;              // 0: no custom row
static struct uui_droprows g_sizes;

// "1 GiB" when it is a whole number of them, else "1536 MiB".
static void fmt_size(char *out, int cap, int mib) {
    if (mib >= 1024 && mib % 1024 == 0) snprintf(out, (unsigned)cap, "%d GiB", mib / 1024);
    else snprintf(out, (unsigned)cap, "%d MiB", mib);
}

static int preset_of(int mib) {
    for (int i = 0; i < PRESETS; i++) if (PRESET_MIB[i] == mib) return i;
    return -1;
}

static int size_mib(void) { return g_size_mib; }

static void size_rows_init(void) {
    static char labels[PRESETS][16];
    static const char *rows[PRESETS];
    for (int i = 0; i < PRESETS; i++) {
        fmt_size(labels[i], sizeof labels[i], PRESET_MIB[i]);
        rows[i] = labels[i];
    }
    uui_droprows_init(&g_sizes, &g_size, 1, "Custom...");
    uui_droprows_set_fixed(&g_sizes, rows, PRESETS);
}

static void set_size(int mib) {
    if (mib < 1) mib = 1;
    if (mib > CUSTOM_MAX_MIB) mib = CUSTOM_MAX_MIB;
    g_size_mib = mib;
    int p = preset_of(mib);
    if (p < 0) {
        char label[16];
        g_custom_mib = mib;
        fmt_size(label, sizeof label, mib);
        uui_droprows_add_own(&g_sizes, label);   // the one custom row, replaced
    } else {
        uui_droprows_select(&g_sizes, UUI_DROPROW_FIXED, p);
    }
    if (g_prefs.size_mib != mib) {
        g_prefs.size_mib = mib;
        prefs_save();
    }
}

// --- history ----------------------------------------------------------
//
// One line per finished run, newest LAST in the file and first on
// screen: `epoch MiB point device sr sw rr rw rri rwi rrl rwl` -- the
// four milli-MB/s, then the random IOPS and latencies.
#define HIST_MAX 50
struct run { long long when; int mib; char point[32]; char dev[16];
             unsigned long long mb[PROFILES]; unsigned long long rri, rwi, rrl, rwl; };
static struct run g_runs[HIST_MAX];
static int g_nruns;

static void history_load(void) {
    g_nruns = 0;
    FILE *f = fopen(HISTORY_PATH, "r");
    if (!f) { uui_table_set_rows(&g_hist, 0); return; }
    static struct run all[HIST_MAX];   // static: the ring-3 frame budget is 2 KiB
    int n = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        struct run r;
        memset(&r, 0, sizeof r);
        if (sscanf(line, "%lld %d %31s %15s %llu %llu %llu %llu %llu %llu %llu %llu",
                   &r.when, &r.mib, r.point, r.dev, &r.mb[0], &r.mb[1], &r.mb[2], &r.mb[3],
                   &r.rri, &r.rwi, &r.rrl, &r.rwl) != 12)
            continue;
        if (n == HIST_MAX) { memmove(all, all + 1, sizeof all[0] * (HIST_MAX - 1)); n--; }
        all[n++] = r;
    }
    fclose(f);
    for (int i = 0; i < n; i++) g_runs[i] = all[n - 1 - i];   // newest first
    g_nruns = n;
    uui_table_set_rows(&g_hist, g_nruns);
}

static void history_append(void) {
    mkdir(HISTORY_DIR, 0755);   // first use; an existing one is fine
    FILE *f = fopen(HISTORY_PATH, "a");
    if (!f) return;
    fprintf(f, "%lld %d %s %s %llu %llu %llu %llu %llu %llu %llu %llu\n",
            (long long)g_run_when, g_run_mib, g_run_vol[0] ? g_run_vol : "/",
            g_run_dev[0] ? g_run_dev : "-",
            (unsigned long long)g_result[0], (unsigned long long)g_result[1],
            (unsigned long long)g_result[2], (unsigned long long)g_result[3],
            (unsigned long long)g_iops[P_RND_READ], (unsigned long long)g_iops[P_RND_WRITE],
            (unsigned long long)g_lat_us[P_RND_READ], (unsigned long long)g_lat_us[P_RND_WRITE]);
    fclose(f);
}

static const struct uui_table_column HIST_COLS[] = {
    { "When",     0,  UUI_TALIGN_LEFT },
    { "Volume",   11, UUI_TALIGN_LEFT },
    { "Size",     8,  UUI_TALIGN_RIGHT },
    { "SEQ R",    10, UUI_TALIGN_RIGHT },
    { "SEQ W",    10, UUI_TALIGN_RIGHT },
    { "RND4K R",  10, UUI_TALIGN_RIGHT },
    { "RND4K W",  10, UUI_TALIGN_RIGHT },
};

static void fmt_mb(char *out, int cap, unsigned long long milli) {
    snprintf(out, (unsigned)cap, "%llu.%llu", milli / 1000, (milli % 1000) / 100);
    unum_localize(out, (unsigned)cap, 0);
}

// A reading against the run BEFORE it on the same volume and size -- a
// comparison across sizes or disks says nothing.
static void hist_cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    const struct run *r = &g_runs[row];
    switch (col) {
    case 0: {
        time_t t = (time_t)r->when;
        struct tm tm;
        localtime_r(&t, &tm);
        udate_format_tm(out, (unsigned long)cap, &tm, UDATE_DATE | UDATE_TIME);
        return;
    }
    case 1: snprintf(out, (unsigned)cap, "%s  %s", r->point, r->dev); return;
    case 2: fmt_size(out, cap, r->mib); return;
    default: {
        int p = col == 3 ? P_SEQ_READ : col == 4 ? P_SEQ_WRITE : col == 5 ? P_RND_READ : P_RND_WRITE;
        char v[16];
        fmt_mb(v, sizeof v, r->mb[p]);
        const struct run *prev = NULL;
        for (int k = row + 1; k < g_nruns; k++)
            if (g_runs[k].mib == r->mib && !strcmp(g_runs[k].point, r->point)) { prev = &g_runs[k]; break; }
        if (prev && prev->mb[p]) {
            long long d = ((long long)r->mb[p] - (long long)prev->mb[p]) * 100 / (long long)prev->mb[p];
            snprintf(out, (unsigned)cap, "%s %+lld%%", v, d);
        } else {
            snprintf(out, (unsigned)cap, "%s", v);
        }
        return;
    }
    }
}

// --- readings ---------------------------------------------------------

static void set_status(const char *s) { strlcpy(g_status, s, sizeof g_status); }

// --- will it fit, and how long -----------------------------------------

// The largest size the chosen volume has room for, in MiB, or -1.
// uvolinfo_fits() is the test diskbench applies too.
static long long room_mib(void) {
    struct uvolinfo vi;
    return chosen_volinfo(&vi) ? (long long)(uvolinfo_room(&vi) >> 20) : -1;
}

static int size_fits(int mib) {
    long long room = room_mib();
    return room < 0 || mib <= room;
}

// From the newest run on this volume, at whatever size it was: each
// profile's MB/s over what it would move at `mib` -- the random ones an
// eighth of the file (bin/diskbench.c). Seconds, or -1 with nothing to
// go by.
static long long estimate_s(int mib) {
    if (!work_path()[0]) return -1;
    char point[32];
    chosen_point(point, sizeof point);
    for (int i = 0; i < g_nruns; i++) {
        const struct run *r = &g_runs[i];
        if (strcmp(r->point, point)) continue;
        uint64_t bytes = (uint64_t)mib << 20, us = 0;
        for (int p = 0; p < PROFILES; p++) {
            if (!r->mb[p]) return -1;
            uint64_t moved = (p == P_RND_READ || p == P_RND_WRITE) ? bytes / 8 : bytes;
            us += moved * 1000 / r->mb[p];   // milli-MB/s: bytes*1000/milli is microseconds
        }
        return (long long)(us / 1000000);
    }
    return -1;
}

static void fmt_duration(char *out, int cap, long long s) {
    if (s < 60) { snprintf(out, (unsigned)cap, "under a minute"); return; }
    long long m = (s + 30) / 60;
    if (m < 90) snprintf(out, (unsigned)cap, "about %lld min", m);
    else snprintf(out, (unsigned)cap, "about %lld h %lld min", m / 60, m % 60);
}

// The idle status line for the size and location chosen: refused when
// it does not fit, else how long it should take, when a run says -- and
// that a share is a share, since its numbers are the network's.
static void size_status(void) {
    if (g_pid > 0 || !work_path()[0]) return;
    char sz[16];
    fmt_size(sz, sizeof sz, g_size_mib);
    struct uvolinfo vi;
    if (!size_fits(g_size_mib) && chosen_volinfo(&vi)) {
        char f[24];
        human_size_iec(f, sizeof f, uvolinfo_free(&vi));
        snprintf(g_status, sizeof g_status, "%s does not fit: %s has %s free.", sz, vi.point, f);
        return;
    }
    const char *share = chosen_is_share() ? "a network share" : NULL;
    long long est = estimate_s(g_size_mib);
    char d[32];
    if (est >= 0) fmt_duration(d, sizeof d, est);
    // The size is in the pane beside it.
    if (est < 0) snprintf(g_status, sizeof g_status, "Ready%s%s.", share ? " -- " : "", share ? share : "");
    else snprintf(g_status, sizeof g_status, "Ready -- %s%s%s.", share ? share : "", share ? ", " : "", d);
}

// --- Custom size... -------------------------------------------------------

static char g_dlg_line[2][96];
static const char *g_dlg_rows[2] = { g_dlg_line[0], g_dlg_line[1] };

static void dialog_lines(void) {
    struct uvolinfo vi;
    g_dlg_line[0][0] = g_dlg_line[1][0] = 0;
    if (chosen_volinfo(&vi)) {
        char f[24];
        human_size_iec(f, sizeof f, uvolinfo_free(&vi));
        snprintf(g_dlg_line[0], sizeof g_dlg_line[0], "%s on %s has %s free.", vi.point,
                 vi.device[0] ? vi.device : vi.remote ? vi.server : vi.fs, f);
    }
    long long est = estimate_s((int)uui_sizefield_mib(&g_sf));
    if (est >= 0) {
        char d[32];
        fmt_duration(d, sizeof d, est);
        snprintf(g_dlg_line[1], sizeof g_dlg_line[1], "%c%s, from the last run on this volume.",
                 d[0] - 'a' + 'A', d + 1);
    } else {
        snprintf(g_dlg_line[1], sizeof g_dlg_line[1], "No earlier run here to estimate the time from.");
    }
}

static void open_custom(struct uapp *a) {
    if (g_pid > 0) return;
    long long room = room_mib();
    uint64_t max = room > 0 && room < CUSTOM_MAX_MIB ? (uint64_t)room : CUSTOM_MAX_MIB;
    uui_sizefield_init(&g_sf, (uint64_t)g_size_mib, 1, max);
    static const struct uui_dialog_button btns[] = {
        { "Cancel", DLG_CANCEL, 0 }, { "OK", DLG_OK, UUI_DLG_PRIMARY },
    };
    dialog_lines();
    uui_dialog_set_bounds(&g_dlg, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_dlg, "Test size", g_dlg_rows, 2, btns, 2, 1, DLG_CANCEL);
    int bw, bh;
    uui_sizefield_natural_size(&g_sf, &bw, &bh);
    uui_dialog_set_body(&g_dlg, &g_sf_item, bw, bh);
    struct uui_item *tabs[] = { &g_sf_item };
    uui_dialog_set_tabs(&g_dlg, tabs, 1);
    uui_dialog_focus(&g_dlg, &g_sf_item);
    ulogf("diskmark: custom size dialog, room %lld MiB\n", room);
}

static void custom_answer(int code) {
    uui_dialog_close(&g_dlg);
    uui_dialog_set_body(&g_dlg, 0, 0, 0);
    if (code == DLG_OK) {
        uui_sizefield_commit(&g_sf);
        set_size((int)uui_sizefield_mib(&g_sf));
        ulogf("diskmark: size %d MiB\n", g_size_mib);
    } else {
        uui_droprows_reselect(&g_sizes);   // the dropdown back on the size it had
    }
    size_status();
}

// --- Choose folder... -------------------------------------------------------

static void folder_chosen(void *ctx, const char *path) {
    struct uapp *a = ctx;
    char why[DIR_MAX + 64];
    if (!path) {
        ulogf("diskmark: folder chooser cancelled\n");
    } else if (g_pid > 0) {
        // started from the keyboard while the chooser was up: keep the run's
    } else if (!folder_usable(path, why, sizeof why)) {
        set_status(why);
        ulogf("diskmark: folder refused: %s\n", why);
    } else {
        int vol = -1;
        for (int i = 0; i < g_nvols; i++) if (!strcmp(g_vols[i].point, path)) vol = i;
        if (vol >= 0) uui_droprows_select(&g_locs, UUI_DROPROW_FIXED, vol);   // a volume's top is its row
        else uui_droprows_add_own(&g_locs, path);
        location_save();
        ulogf("diskmark: location %s\n", g_prefs.location);
        size_status();
    }
    uapp_redraw(a);
}

// Opens at the folder in force, else the person's Documents.
static void open_chooser(struct uapp *a) {
    if (g_pid > 0 || uui_filedialog_is_open(&g_chooser)) return;
    const char *start = chosen_folder();
    if (!start) start = udir_path(UDIR_DOCUMENTS);
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_FOLDER,
        .title = "Choose Test Folder",
        .start_dir = start,
    };
    if (!uui_filedialog_open(a, &g_chooser, &o, folder_chosen, a)) {
        set_status("Could not open the folder chooser");
        return;
    }
    ulogf("diskmark: folder chooser open at %s\n", start ? start : "/");
}

// Each bar against the FASTEST of this run: a ceiling picked for an SSD
// leaves every bar here empty, and one picked for an emulator lies.
static void rescale_bars(void) {
    uint64_t best = 0;
    for (int i = 0; i < PROFILES; i++) if (g_result[i] > best) best = g_result[i];
    for (int i = 0; i < PROFILES; i++)
        uui_meter_set_fill(&g_meter[i], best ? (int)((g_result[i] * 1000ull) / best) : 0);
}

static int wire_index(const char *name, int len) {
    for (int i = 0; i < PROFILES; i++) {
        int n = (int)strlen(WIRE_NAME[i]);
        if (n == len && !strncmp(name, WIRE_NAME[i], (unsigned)n)) return i;
    }
    return -1;
}

// THE CHART: one series for reads and one for writes, a mark where each
// phase begins. A phase too short to be sampled still gets its mark and
// its result as one point.
static int g_phase_seen[PROFILES];
static int g_phase_samples[PROFILES];
static int g_last_p = -1;
static unsigned long long g_last_moved, g_last_us;
static char g_chart_value[40];

static void phase_begin(int p) {
    if (g_phase_seen[p]) return;
    g_phase_seen[p] = 1;
    uui_chart_set_series(&g_chart, is_write(p) ? 1 : 0);
    uui_chart_add_mark(&g_chart, PHASE_LABEL[p]);
    g_last_p = p;
    g_last_moved = g_last_us = 0;
}

static void sample(int p, unsigned long long moved, unsigned long long us) {
    phase_begin(p);
    if (p != g_last_p) { g_last_p = p; g_last_moved = g_last_us = 0; }
    if (us <= g_last_us || moved <= g_last_moved) return;
    // Bytes per microsecond is MB/s; in milli-MiB/s, the results' unit.
    unsigned long long dm = moved - g_last_moved, du = us - g_last_us;
    unsigned long long milli = (dm * 1000ull / 1048576ull) * 1000000ull / du;
    uui_chart_push(&g_chart, (uint32_t)milli);
    g_phase_samples[p]++;
    g_last_moved = moved;
    g_last_us = us;
    char v[16];
    fmt_mb(v, sizeof v, milli);
    snprintf(g_chart_value, sizeof g_chart_value, "%s  %s MB/s", PHASE_LABEL[p], v);
}

static void show_result(int p, uint64_t milli, uint64_t iops, uint64_t us) {
    // THE DRAIN IS IDEMPOTENT, SO THIS MUST BE: re-LOGGING a seen line
    // every tick floods dmesg.
    if (g_result[p] == milli) return;
    // A PHASE TOO SHORT TO SAMPLE is still drawn: its result as a flat
    // run of two points rather than a dot nobody sees.
    phase_begin(p);
    if (p == g_last_p)
        while (g_phase_samples[p] < 2) { uui_chart_push(&g_chart, (uint32_t)milli); g_phase_samples[p]++; }
    g_result[p] = milli;
    g_iops[p] = iops;
    g_lat_us[p] = us;
    fmt_mb(g_value[p], sizeof g_value[p], milli);
    // IOPS and latency on the random profiles only: at a 256 KiB request
    // the sequential op count is the byte count again.
    if (p == P_RND_READ || p == P_RND_WRITE) {
        char ms[16];
        snprintf(ms, sizeof ms, "%llu.%llu", (unsigned long long)(us / 1000),
                 (unsigned long long)((us % 1000) / 100));
        unum_localize(ms, sizeof ms, 0);
        snprintf(g_detail[p], sizeof g_detail[p], "%llu IOPS, %s ms avg", (unsigned long long)iops, ms);
        unum_localize(g_detail[p], sizeof g_detail[p], 0);
    } else {
        g_detail[p][0] = 0;
    }
    uui_meter_set(&g_meter[p], CAPTION[p], g_value[p], "MB/s", g_detail[p][0] ? g_detail[p] : NULL);
    ulogf("diskmark: result %s = %s MB/s %s\n", PROFILE_NAME[p], g_value[p],
          g_detail[p][0] ? g_detail[p] : "-");
}

// Re-reads the child's report whole and applies every line. Idempotent:
// re-applying what was seen costs a few compares and no read offset.
static void drain_output(void) {
    int fd = sys_open(RESULT_PATH, 0);
    if (fd < 0) return;
    int64_t n = sys_read(fd, g_out, sizeof g_out - 1);
    sys_close(fd);
    if (n <= 0) return;
    g_out[n] = 0;

    for (char *line = g_out; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        const char *p = strstr(line, "diskbench: ");
        if (p) {
            p += 11;
            if (!strncmp(p, "result ", 7)) {
                p += 7;
                const char *sp = strchr(p, ' ');
                unsigned long long milli = 0, iops = 0, us = 0;
                int idx = sp ? wire_index(p, (int)(sp - p)) : -1;
                if (idx >= 0 && sscanf(sp + 1, "%llu %llu %llu", &milli, &iops, &us) == 3)
                    show_result(idx, milli, iops, us);
            } else if (!strncmp(p, "progress ", 9)) {
                p += 9;
                const char *sp = strchr(p, ' ');
                int idx = sp ? wire_index(p, (int)(sp - p)) : -1;
                int pct = 0;
                unsigned long long moved = 0, us = 0;
                int got = idx >= 0 ? sscanf(sp + 1, "%d %llu %llu", &pct, &moved, &us) : 0;
                if (got >= 1 && !g_result[idx]) {
                    g_running_p = idx;
                    snprintf(g_status, sizeof g_status, "%s: %d%%", PHASE_LABEL[idx], pct);
                    snprintf(g_detail[idx], sizeof g_detail[idx], "running, %d%%", pct);
                    uui_meter_set(&g_meter[idx], CAPTION[idx], "--", NULL, g_detail[idx]);
                    uui_meter_set_fill(&g_meter[idx], pct * 10);
                }
                if (got == 3) sample(idx, moved, us);
            } else if (!strncmp(p, "error ", 6)) {
                snprintf(g_status, sizeof g_status, "diskbench failed: %s", p + 6);
                g_state = ST_FAILED;
            }
        }
        line = nl ? nl + 1 : 0;
    }
}

static void cleanup(void) {
    sys_unlink(RESULT_PATH);
    if (g_work[0]) sys_unlink(g_work);
}

static void reset_readings(void) {
    for (int i = 0; i < PROFILES; i++) {
        g_value[i][0] = g_detail[i][0] = 0;
        g_result[i] = g_iops[i] = g_lat_us[i] = 0;
        g_phase_seen[i] = 0;
        g_phase_samples[i] = 0;
        uui_meter_set(&g_meter[i], CAPTION[i], "--", NULL, "waiting");
        uui_meter_set_fill(&g_meter[i], 0);
        g_meter[i].active = 0;
    }
    g_running_p = -1;
    g_last_p = -1;
    g_chart_value[0] = 0;
    uui_chart_set_fit(&g_chart, 1);
}

static void begin_run(struct uapp *a) {
    if (g_pid > 0) return;   // running, or a stopped child not yet reaped
    const struct vol *v = chosen_vol();
    const char *folder = chosen_folder();
    char why[DIR_MAX + 64];
    if (!work_path()[0] || (folder && !folder_usable(folder, why, sizeof why))) {
        // A folder can go between choosing it and running: an unplugged
        // disk, a share gone, the folder deleted in the File Manager.
        set_status(folder ? why : "no writable disk volume is mounted -- RAM would measure memcpy");
        ulogf("diskmark: run refused: %s\n", g_status);
        g_state = ST_FAILED;
        uapp_redraw(a);
        return;
    }
    if (!size_fits(size_mib())) {
        size_status();   // says why, and what the volume has
        uapp_redraw(a);
        return;
    }
    reset_readings();
    strlcpy(g_work, work_path(), sizeof g_work);
    cleanup();

    // THE CHILD WRITES THE REPORT ITSELF (`--out`) with no stdout: a pipe
    // would couple the two (a pipe holds 4 KiB), so a window
    // slow to drain would block the benchmark it is timing.
    // A VECTOR, not sys_spawn()'s string: that splits on whitespace, and
    // a chosen folder may have a space in its name.
    char size[16];
    snprintf(size, sizeof size, "%d", size_mib());
    char *const argv[] = { BENCH_PATH, "--size", size, "--path", g_work,
                           "--out", (char *)RESULT_PATH, NULL };
    g_pid = sys_spawn_argv(BENCH_PATH, argv);
    if (g_pid < 0) {
        set_status("could not start " BENCH_PATH);
        g_state = ST_FAILED;
        cleanup();
    } else {
        g_state = ST_RUNNING;
        g_run_mib = size_mib();
        // As the history will read it back: one word, its field's width.
        // A share has no device; its filesystem stands in ("smb").
        struct uvolinfo vi;
        chosen_point(g_run_vol, sizeof g_run_vol);
        strlcpy(g_run_dev, v ? v->dev : "-", sizeof g_run_dev);
        if (!v && uvolinfo_of(g_work, &vi))
            strlcpy(g_run_dev, vi.device[0] ? vi.device : vi.fs[0] ? vi.fs : "-", sizeof g_run_dev);
        g_run_when = time(NULL);
        char sz[16];
        fmt_size(sz, sizeof sz, size_mib());
        if (v) snprintf(g_status, sizeof g_status, "Running %s on %s...", sz, v->point);
        else snprintf(g_status, sizeof g_status, "Running %s in %s%s...", sz, folder,
                      chosen_is_share() ? " (a network share)" : "");
        ulogf("diskmark: spawned %s as pid %d, file %s\n", BENCH_PATH, g_pid, g_work);
    }
    uapp_redraw(a);
}

static void stop_run(struct uapp *a) {
    if (g_pid <= 0 || g_state != ST_RUNNING) return;
    // KILLED, and reaped on the next ticks; diskbench has no handler to
    // remove its scratch file, so cleanup() does once it is gone.
    sys_kill(g_pid, 9);
    g_state = ST_STOPPING;
    set_status("Stopping...");
    uapp_redraw(a);
}

static void copy_results(void) {
    char text[512];
    char sz[16];
    fmt_size(sz, sizeof sz, g_run_mib ? g_run_mib : size_mib());
    int n = snprintf(text, sizeof text, "Disk Mark -- %s, %s, Q1T1, %u KiB per call\n",
                     g_run_vol[0] ? g_run_vol : "/", sz, (unsigned)(SYS_WRITE_MAX / 1024));
    for (int p = 0; p < PROFILES && n < (int)sizeof text; p++) {
        char v[16];
        fmt_mb(v, sizeof v, g_result[p]);
        n += snprintf(text + n, sizeof text - (unsigned)n, "%-18s %8s MB/s%s%s\n", PROFILE_NAME[p],
                      g_result[p] ? v : "--", g_detail[p][0] && g_result[p] ? "  " : "",
                      g_result[p] ? g_detail[p] : "");
    }
    if (n > (int)sizeof text - 1) n = (int)sizeof text - 1;
    set_status(uclip_set_text(text, n) ? "Copied the results" : "The clipboard refused it");
}

static unsigned item_flags(int code) {
    int busy = g_pid > 0;
    switch (code) {
    case CMD_RUN:     return busy || !size_fits(size_mib()) ? UUI_MI_DISABLED : 0;
    case CMD_CUSTOM:  return (preset_of(g_size_mib) < 0 ? UUI_MI_CHECKED : 0) | (busy ? UUI_MI_DISABLED : 0);
    case CMD_FOLDER:  return (chosen_folder() ? UUI_MI_CHECKED : 0) | (busy ? UUI_MI_DISABLED : 0);
    case CMD_STOP:    return g_state == ST_RUNNING ? 0 : UUI_MI_DISABLED;
    case CMD_COPY:    return (g_result[0] || g_result[3]) ? 0 : UUI_MI_DISABLED;
    case CMD_HISTORY: return g_show_hist ? UUI_MI_CHECKED : 0;
    default:
        if (code >= CMD_SIZE_0 && code < CMD_SIZE_0 + PRESETS)
            return (g_size_mib == PRESET_MIB[code - CMD_SIZE_0] ? UUI_MI_CHECKED : 0) |
                   (busy ? UUI_MI_DISABLED : 0);
        return 0;
    }
}

static int on_close(struct uapp *a);

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_RUN:     begin_run(a); break;
    case CMD_STOP:    stop_run(a); break;
    case CMD_COPY:    copy_results(); break;
    case CMD_HISTORY: g_show_hist = !g_show_hist; if (g_show_hist) history_load(); break;
    case CMD_EXIT:    on_close(a); uapp_quit(a, 0); break;
    case CMD_CUSTOM:  open_custom(a); break;
    case CMD_FOLDER:  open_chooser(a); break;
    default:
        if (code >= CMD_SIZE_0 && code < CMD_SIZE_0 + PRESETS && g_pid <= 0) {
            set_size(PRESET_MIB[code - CMD_SIZE_0]);
            size_status();
        }
        break;
    }
    uapp_redraw(a);
}

// --- layout -----------------------------------------------------------

static int pad(void) { return ugfx_char_h() * 3 / 4; }
static int card_min_w(void) { return 17 * ugfx_char_advance('n'); }

static void layout(int cw, int ch) {
    int y = 0;
    int mh = uui_menubar_height(&g_menu);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mh);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    y += mh;
    int th = uui_toolbar_height(&g_tb);
    uui_toolbar_ops.set_geometry(&g_tb, 0, y, cw, th);

    // The size and the volume BETWEEN Stop and the right-hand group.
    int sx, sy, sw, sh, rx, ry, rw, rh;
    uui_toolbar_item_rect(&g_tb, TB_STOP, &sx, &sy, &sw, &sh);
    if (!uui_toolbar_item_rect(&g_tb, TB_COPY, &rx, &ry, &rw, &rh) || rw == 0) rx = cw;
    int gap = ugfx_char_h() / 2;
    // THE BUTTONS' HEIGHT, not the dropdown's own: its natural height is
    // a form row's, taller than the bar, and centred it stuck out of both
    // edges. Placed on Stop's rect, the boxes line up with the buttons.
    int bh = sh > 0 ? sh : th, by = sh > 0 ? sy : y;
    int segw, segh;
    uui_dropdown_natural_size(&g_size, &segw, &segh);
    if (segh < bh) { by += (bh - segh) / 2; bh = segh; }
    int x = sx + sw + gap * 2;
    uui_dropdown_set_geometry(&g_size, x, by, segw, bh);
    x += segw + gap;
    int dw, dh;
    uui_dropdown_natural_size(&g_vol, &dw, &dh);
    if (dw > rx - gap * 2 - x) dw = rx - gap * 2 - x;
    if (dw < ugfx_char_h() * 3) dw = ugfx_char_h() * 3;
    uui_dropdown_set_geometry(&g_vol, x, by, dw, bh);
    y += th + 1;

    uui_dialog_set_bounds(&g_dlg, 0, 0, cw, ch);
    int sbh = uui_statusbar_height(&g_sb);
    uui_statusbar_set_geometry(&g_sb, 0, ch - sbh, cw, sbh);

    // Four cards in a row, or two by two when the window is narrow.
    int p = pad();
    int mw, mhh;
    uui_meter_natural_size(&g_meter[0], &mw, &mhh);
    // A FIXED floor, not the natural width -- that moves with the number
    // on the card, and the grid would reflow as a run filled it in.
    mw = card_min_w();
    int cols = (cw - 5 * p) / 4 >= mw ? 4 : 2;
    int rows = PROFILES / cols;
    int cardw = (cw - (cols + 1) * p) / cols;
    y += p;
    for (int i = 0; i < PROFILES; i++) {
        int c = i % cols, r = i / cols;
        uui_meter_ops.set_geometry(&g_meter[i], p + c * (cardw + p), y + r * (mhh + p), cardw, mhh);
    }
    y += rows * (mhh + p);

    int bottom_h = ch - sbh - p - y;
    if (bottom_h < 1) bottom_h = 1;
    uui_chart_set_geometry(&g_chart, p, y, cw - 2 * p, bottom_h);
    uui_table_ops.set_geometry(&g_hist, p, y, cw - 2 * p, bottom_h);
    // By id, not by index: an index silently moves to the next widget
    // when one is added or taken out above it.
    for (unsigned i = 0; i < sizeof g_widgets / sizeof g_widgets[0]; i++)
        if (g_widgets[i].id == ID_HIST) g_widgets[i].hidden = !g_show_hist;
}

static void log_layout(struct uapp *a) {
    int x, y, w, h;
    // What tools/diskmark_test.py clicks and compares.
    if (uui_toolbar_item_rect(&g_tb, 0, &x, &y, &w, &h))
        uapp_logf_layout("diskmark: layout run %d %d %d %d\n", x, y, w, h);
    for (int i = 0; i < PROFILES; i++)
        uapp_logf_layout("diskmark: layout tile %d %d %d %d %d\n", i,
                         g_meter[i].x, g_meter[i].y, g_meter[i].w, g_meter[i].h);
    if (!g_show_hist)
        uapp_logf_layout("diskmark: layout chart %d %d %d %d samples %d marks %d\n",
                         g_chart.x, g_chart.y, g_chart.w, g_chart.h, g_chart.count,
                         g_chart.mark_n);
    uapp_log_layout(a, "diskmark");
}

static void update_panes(void) {
    fmt_size(g_size_pane, sizeof g_size_pane, g_run_mib ? g_run_mib : size_mib());
    snprintf(g_call_pane, sizeof g_call_pane, "%u KiB per call", (unsigned)(SYS_WRITE_MAX / 1024));
    struct uvolinfo vi;
    g_free_pane[0] = 0;
    if (chosen_volinfo(&vi)) {
        char f[16];
        human_size_iec(f, sizeof f, uvolinfo_free(&vi));
        snprintf(g_free_pane, sizeof g_free_pane, "%s free", f);
    }
    if (g_run_when) {
        struct tm tm;
        time_t t = g_run_when;
        localtime_r(&t, &tm);
        udate_format_tm(g_when_pane, sizeof g_when_pane, &tm, UDATE_TIME);
    } else {
        strlcpy(g_when_pane, "-", sizeof g_when_pane);
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    layout(s->w, s->h);
    update_panes();
    g_vol.disabled = g_size.disabled = g_pid > 0;   // fixed until the run ends
    for (int i = 0; i < PROFILES; i++) {
        g_meter[i].active = (g_state == ST_RUNNING && i == g_running_p);
        uui_meter_draw(s, &g_meter[i]);
    }
    if (!g_show_hist) {
        uui_chart_set_value(&g_chart, g_chart_value[0] ? g_chart_value : NULL);
        uui_chart_draw(s, &g_chart);
    }
    log_layout(a);
}

static int on_tick(struct uapp *a) {
    int redraw = uui_toolbar_tick(&g_tb);
    if (g_pid <= 0) return redraw;
    if (g_state == ST_RUNNING) drain_output();

    // POLLED WHATEVER THE STATE: a failed or stopped child still has to
    // be reaped, or it stays a zombie and its files stay behind.
    int code = 0;
    if (sys_waitpid_nohang(g_pid, &code) == g_pid) {
        g_pid = -1;
        if (g_state == ST_RUNNING) {
            drain_output();   // its last lines can land between the read and its exit
            rescale_bars();
            g_state = ST_DONE;
            if (code) snprintf(g_status, sizeof g_status, "diskbench exited %d", code);
            else {
                set_status("Done.");
                g_chart_value[0] = 0;
                ulog("diskmark: all four passes complete\n");
                history_append();
                if (g_show_hist) history_load();
            }
        } else if (g_state == ST_STOPPING) {
            g_state = ST_IDLE;
            set_status("Stopped.");
            ulog("diskmark: stopped\n");
        }
        g_running_p = -1;
        cleanup();
    }
    uapp_redraw(a);
    return 1;
}

static void on_widget(struct uapp *a, int id, int reason) {
    if (id == ID_MENU) {
        int code = uui_menubar_take_code(&g_menu);
        if (code > 0) do_command(a, code);
    } else if (id == ID_TOOLBAR) {
        int code = uui_toolbar_take_code(&g_tb);
        if (code > 0) do_command(a, code);
    } else if (id == ID_SIZE) {
        int i, kind = uui_droprows_picked(&g_sizes, &i);
        if (g_size.open || kind == UUI_DROPROW_NONE) {
            // still choosing
        } else if (g_pid > 0) {
            uui_droprows_reselect(&g_sizes);   // a run's size is the one it started with
        } else if (kind == UUI_DROPROW_MORE) {
            uui_droprows_reselect(&g_sizes);   // the size in force, not "Custom...", behind the dialog
            open_custom(a);
        } else if (kind == UUI_DROPROW_FIXED) {
            if (PRESET_MIB[i] != g_size_mib) { set_size(PRESET_MIB[i]); size_status(); }
        } else if (g_custom_mib && g_custom_mib != g_size_mib) {
            set_size(g_custom_mib);
            size_status();
        }
    } else if (id == ID_VOL) {
        int i, kind = uui_droprows_picked(&g_locs, &i);
        if (g_vol.open || kind == UUI_DROPROW_NONE) {
            // still choosing
        } else if (g_pid > 0) {
            uui_droprows_reselect(&g_locs);
        } else if (kind == UUI_DROPROW_MORE) {
            uui_droprows_reselect(&g_locs);
            open_chooser(a);
        } else if (kind != g_locs.sel_kind || i != g_locs.sel_index) {
            uui_droprows_select(&g_locs, kind, i);
            location_save();
            ulogf("diskmark: location %s\n", g_prefs.location);
            size_status();
        }
    } else if (id == ID_DIALOG) {
        int code = uui_dialog_take_code(&g_dlg);
        if (code > 0) custom_answer(code);
        else dialog_lines();
    } else if (id == ID_SIZEFIELD) {
        if (reason == UUI_REASON_PRESS) uui_dialog_focus(&g_dlg, &g_sf_item);
        dialog_lines();
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int code;
    if (uui_dialog_is_open(&g_dlg)) return;   // a shortcut must not run past the modal
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        uapp_redraw(a);
        return;
    }
    switch (key) {
    case KEY_F5: do_command(a, CMD_RUN); break;
    case KEY_F9: do_command(a, CMD_HISTORY); break;
    case 0x1B:   do_command(a, CMD_STOP); break;   // Esc stops a run; it closes nothing
    case 0x03:   do_command(a, CMD_COPY); break;   // Ctrl-C
    default: break;
    }
}

static int on_close(struct uapp *a) {
    (void)a;
    // A run cut off by closing the window would otherwise leave its
    // scratch file AND an orphan still writing to the disk.
    if (g_pid > 0) sys_kill(g_pid, 9);
    cleanup();
    if (uui_filedialog_is_open(&g_chooser)) uui_filedialog_close_window(&g_chooser);
    return 1;
}

static void default_size(int *w, int *h);

static void on_open(struct uapp *a) {
    // A REMEMBERED SIZE FROM THE OLD LAYOUT is smaller than this one
    // needs; grow to fit, never shrink what the person chose.
    int dw, dh;
    default_size(&dw, &dh);
    if (uapp_width(a) < dw || uapp_height(a) < dh)
        uapp_resize(a, uapp_width(a) > dw ? uapp_width(a) : dw,
                    uapp_height(a) > dh ? uapp_height(a) : dh);
    if (!work_path()[0]) set_status("no writable disk volume is mounted -- RAM would measure memcpy");
    else size_status();
    layout(uapp_width(a), uapp_height(a));
    log_layout(a);
    ulogf("diskmark: ready, size %d MiB\n", g_size_mib);
}

static void load_big_font(void) {
    int px = ugfx_char_h() * 2;
    if (px < 16) px = 16;
    unsigned long need = ugfx_font_arena_size(px);
    void *arena = malloc(need);
    if (!arena) return;
    g_big_ok = ugfx_font_load("/usr/share/fonts/dejavu-sans-mono-bold.ttf",
                              px, 1, &g_big, arena, need);
    if (!g_big_ok) free(arena);
}

// From the font: four cards across and a chart under them.
static void default_size(int *w, int *h) {
    int mw, mh;
    uui_meter_natural_size(&g_meter[0], &mw, &mh);
    mw = card_min_w();
    int p = pad();
    int want = 4 * mw + 5 * p;
    int floor = 76 * ugfx_char_advance('n');
    *w = want > floor ? want : floor;
    *h = uui_menubar_height(&g_menu) + uui_toolbar_height(&g_tb) + 1 + p + mh + p +
         16 * ugfx_char_h() + p + uui_statusbar_height(&g_sb);
}

int main(void) {
    if (!ugfx_font_init()) return 2;   // every metric below comes from it
    setlocale(LC_ALL, "");             // the history's dates and the numbers
    load_big_font();
    scan_volumes();

    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    g_tb.accent_latch = 1;
    // 16 MiB by default: an emulated disk makes 64 MiB a multi-minute run.
    uprefs_load(&PREFS, &g_prefs);
    g_prefs_was = g_prefs;
    uui_dropdown_init(&g_size, 0, 0, 0, 0, NULL, 0);
    size_rows_init();
    set_size(g_prefs.size_mib);
    uui_dialog_init(&g_dlg);
    uui_dropdown_init(&g_vol, 0, 0, 0, 0, NULL, 0);
    location_load();

    for (int i = 0; i < PROFILES; i++) {
        uui_meter_init(&g_meter[i]);
        if (g_big_ok) g_meter[i].value_font = &g_big;
        uint32_t c = is_write(i) ? UUI_ROLE(action[UTHEME_ACT_ARRANGE]) : UUI_ROLE(action[UTHEME_ACT_NAV]);
        g_meter[i].value_fg = c;
        g_meter[i].accent = c;
        uui_meter_set(&g_meter[i], CAPTION[i], "--", NULL, NULL);
    }
    uui_chart_init(&g_chart, "THROUGHPUT DURING THE RUN");
    uui_chart_set_scale(&g_chart, 0);
    uui_chart_set_axis(&g_chart, "MB/s", 1000);   // samples are milli-MB/s
    uui_chart_set_fit(&g_chart, 1);
    g_chart.series_col[0] = UUI_ROLE(action[UTHEME_ACT_NAV]);
    g_chart.series_col[1] = UUI_ROLE(action[UTHEME_ACT_ARRANGE]);

    uui_table_init(&g_hist, 0, 0, 0, 0, HIST_COLS,
                   (int)(sizeof HIST_COLS / sizeof HIST_COLS[0]), hist_cell, NULL);
    history_load();
    uui_table_set_rows(&g_hist, g_nruns);

    uui_statusbar_init(&g_sb);
    g_sb.count = 6;
    g_sb.panes[0].text = g_status;    g_sb.panes[0].chars = 0;
    g_sb.panes[1].text = g_size_pane; g_sb.panes[1].chars = 8;
    g_sb.panes[2].text = g_call_pane; g_sb.panes[2].chars = 14;
    g_sb.panes[3].text = "Q1T1";      g_sb.panes[3].chars = 5;
    g_sb.panes[4].text = g_free_pane; g_sb.panes[4].chars = 13;
    g_sb.panes[5].text = g_when_pane; g_sb.panes[5].chars = 6;

    struct uapp_desc desc = {
        .title        = "Disk Mark",
        .app_id       = "diskmark",
        .x            = 200,
        .y            = 90,
        .on_size      = default_size,
        .flags        = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE,
        .min_w        = 360,
        .min_h        = 300,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        // POLLING A CHILD, NOT ANIMATING: 500 ms is the rate ui/uapp.h
        // names for a process list, and diskbench reports every quarter
        // second, so each tick has a fresh sample for the chart.
        .tick_ms      = 500,
        .on_tick      = on_tick,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_close     = on_close,
    };
    return uapp_run(&desc);
}
