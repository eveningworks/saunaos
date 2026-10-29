// Disks -- every drive, its partitions as a map, and what is on them:
// GNOME Disks' shape (chosen from mockups on 2026-10-09, D1). A list of
// drives, and for the one selected its facts, a MAP of its partitions
// and free space (ui/uui_segbar.h), and the selected volume's facts.
//
// It CHANGES disks, AND THE CHANGES QUEUE (lib/udiskops.h): a new
// partition (an inline form on free space), Format, Resize and Delete
// each add a step to a pending list under the map, and the map and facts
// show the disk as the steps will leave it -- GParted's and KDE Partition
// Manager's shape (chosen 2026-10-29, A). Apply confirms the whole list
// and runs it in order, stopping at the first step that fails. Check and
// Mount/Unmount change no layout, so they run at once. THE SYSTEM DISK IS
// REFUSED for anything but Check and its facts -- its table only takes
// effect at the next boot, and a format there is the running system. A
// new partition also needs nothing mounted from the disk, since it
// rewrites the whole table (SYS_MKPART); Delete edits one entry
// (SYS_PARTEDIT) and needs only that partition unmounted, as Resize does
// (the table, then SYS_FS_RESIZE for the filesystem). Everything goes
// through the same calls `mkpart`, `part`, `mkfs`, `fsck` and `mount` make.
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "partition_abi.h"
#include "mount_abi.h"
#include "lib/human.h"
#include "lib/udiskscan.h"
#include "lib/udiskops.h"
#include "lib/ugrow.h"
#include "lib/icon_cache.h"
#include "lib/uimg.h"
#include "ui/uapp.h"
#include "ui/uui_anim.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui.h"
#include "ui/uui_button.h"
#include "ui/uui_dialog.h"
#include "ui/uui_focus.h"
#include "ui/uui_grid.h"
#include "ui/uui_menubar.h"
#include "ui/uui_primitives.h"
#include "ui/uui_segbar.h"
#include "ui/uui_diskplan.h"   // uui_diskplan_colour()
#include "ui/uui_layout.h"
#include "ui/uui_resizebar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_sizefield.h"
#include "ui/uui_stages.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_steplist.h"
#include "ui/uui_textbox.h"
#include "ui/uui_toolbar.h"
#include "keyboard.h"

#define MIB       UDISK_MIB
#define GPT_TAIL  UDISK_GPT_TAIL

static struct udisk_scan g_scan;   // ~10 KB: never on the stack
static int g_cur;                  // the drives list's row: a disk, then the RAM volumes

// THE PENDING CHANGES, every disk's, and the selected disk AS THEY LEAVE
// IT: everything that draws or decides reads g_view (cur_disk()), so the
// map, the facts and what may be done next all follow the queue.
static struct udiskops g_q;
static struct udisk g_view;
static int g_view_marks[UDISK_PARTS_MAX];   // UDISKOP_MARK_* per g_view partition
static struct uui_step *g_steps;            // g_q as the pending list's rows
static int g_steps_cap;
static struct uui_steplist g_pending;
static struct uui_button g_clear;
static char g_pending_head[64];

// The map of the current drive: a segment per partition and per gap.
static struct uui_segbar_seg g_segs[UUI_SEGBAR_MAX];
static int g_seg_part[UUI_SEGBAR_MAX];       // a part index, -1 for free space
static uint64_t g_seg_start[UUI_SEGBAR_MAX], g_seg_count[UUI_SEGBAR_MAX];
static char g_seg_title[UUI_SEGBAR_MAX][64], g_seg_corner[UUI_SEGBAR_MAX][24], g_seg_detail[UUI_SEGBAR_MAX][96];
static int g_nsegs;

enum {
    ID_MENU = 1, ID_TOOLBAR, ID_DRIVES, ID_MAP, ID_SIZE, ID_NAME, ID_FS, ID_CREATE, ID_STATUS, ID_DIALOG,
    ID_RZ_BAR, ID_RZ_SIZE, ID_RZ_AFTER,   // the Resize dialog's body
    ID_PENDING, ID_CLEAR, ID_RUN,         // the pending list, its Clear all, Apply's list
};
enum {
    CMD_REFRESH = 1, CMD_NEW, CMD_FORMAT, CMD_CHECK, CMD_MOUNT, CMD_DELETE, CMD_EXIT, CMD_RESIZE,
    CMD_UNDO, CMD_CLEAR, CMD_APPLY,
    // dialog answers
    ASK_CANCEL = 100, ASK_FORMAT_TFS3, ASK_FORMAT_FAT, ASK_REPAIR, ASK_OK,
    ASK_STOP, ASK_BUSY, ASK_RESIZE, ASK_APPLY,
};

static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
static struct uui_grid g_drives;
static struct uui_segbar g_map;
static struct uui_sizefield g_size;
static struct uui_textbox g_name;
static struct uui_segmented g_fs;
static struct uui_button g_create;
static struct uui_statusbar g_sb;
static struct uui_dialog g_ask;
static struct uui_focus g_focus;
static struct uui_focusable g_ring[4];
enum { RING_DRIVES, RING_MAP, RING_SIZE, RING_NAME };
static char g_ask_line[3][160];
static const char *g_ask_rows[3];
static char g_note[160], g_st_count[48], g_st_sel[48], g_st_free[48];
static const char *const FS_CHOICES[] = { "tfs3", "FAT32", "Leave empty" };
static int g_form_x, g_form_y;     // where the form's labels go

// --- reading the machine ---------------------------------------------------

static void project(void);
static void steps_sync(void);

static void scan(void) {
    udisk_scan(&g_scan);
    int rows = g_scan.ndisks + g_scan.nram;
    uui_grid_set(&g_drives, rows, g_drives.cell, 0);
    if (g_cur >= rows) g_cur = rows - 1;
    if (g_cur < 0) g_cur = 0;
    uui_grid_select(&g_drives, rows ? g_cur : -1);
    int nparts = 0;
    for (int i = 0; i < g_scan.ndisks; i++) nparts += g_scan.disks[i].nparts;
    snprintf(g_st_count, sizeof g_st_count, "%d drive%s, %d partition%s", g_scan.ndisks, g_scan.ndisks == 1 ? "" : "s",
             nparts, nparts == 1 ? "" : "s");
    ulogf("disks: %d drives, %d partitions, %d in memory\n", g_scan.ndisks, nparts, g_scan.nram);
    int gone = udiskops_revalidate(&g_q, &g_scan);
    if (gone) ulogf("disks: %d pending step%s no longer possible, removed\n", gone, gone == 1 ? "" : "s");
    steps_sync();
    project();
}

static struct udisk *cur_disk(void) { return g_cur >= 0 && g_cur < g_scan.ndisks ? &g_view : 0; }
static struct udisk *cur_real(void) { return g_cur >= 0 && g_cur < g_scan.ndisks ? &g_scan.disks[g_cur] : 0; }
static struct udisk_ram *cur_ram(void) { return g_cur >= g_scan.ndisks && g_cur - g_scan.ndisks < g_scan.nram ? &g_scan.ram[g_cur - g_scan.ndisks] : 0; }

static struct udisk_part *cur_part(void) {
    struct udisk *d = cur_disk();
    int s = g_map.selected;
    if (!d || s < 0 || s >= g_nsegs || g_seg_part[s] < 0) return 0;
    return &d->parts[g_seg_part[s]];
}

static int cur_is_free(void) {
    int s = g_map.selected;
    return cur_disk() && s >= 0 && s < g_nsegs && g_seg_part[s] < 0;
}

static int cur_mark(void) {
    int s = g_map.selected;
    return cur_part() ? g_view_marks[g_seg_part[s]] : 0;
}

// The pending list's rows, from the queue.
static void steps_sync(void) {
    if (!UGROW(g_steps, g_steps_cap, g_q.count)) {
        ulogf("disks: no memory for %d pending rows\n", g_q.count);
        return;
    }
    for (int i = 0; i < g_q.count; i++)
        g_steps[i] = (struct uui_step){ udiskop_verb(g_q.ops[i].kind), g_q.ops[i].what, UUI_STEP_WAITING, -1 };
    uui_steplist_set(&g_pending, g_steps, g_q.count);
    snprintf(g_pending_head, sizeof g_pending_head, "Pending changes (%d)", g_q.count);
}

// The selected disk as the queue leaves it; build_map() asks it of every
// selection, the rows above follow only the queue's own changes.
static void project(void) {
    struct udisk *real = cur_real();
    if (real) udiskops_project(&g_q, real, &g_view, g_view_marks);
}

// --- the map -----------------------------------------------------------------

static void add_seg(int part, uint64_t start, uint64_t count) {
    if (g_nsegs >= UUI_SEGBAR_MAX) return;
    int i = g_nsegs++;
    g_seg_part[i] = part;
    g_seg_start[i] = start;
    g_seg_count[i] = count;
    struct uui_segbar_seg *s = &g_segs[i];
    memset(s, 0, sizeof *s);
    s->size = count;
    s->used_pct = -1;
    char size[24];
    human_size_iec(size, sizeof size, count * 512);
    if (part < 0) {
        snprintf(g_seg_title[i], sizeof g_seg_title[i], "Free space");
        snprintf(g_seg_detail[i], sizeof g_seg_detail[i], "%s", size);
        s->empty = 1;
    } else {
        const struct udisk_part *p = &cur_disk()->parts[part];
        udisk_part_title(p, g_seg_title[i], sizeof g_seg_title[i]);
        // A labelled volume is titled by its label, so the table's name
        // moves down beside what it holds: "7452.0 GiB NTFS · Basic data".
        char name[40] = "";
        if (p->label[0] && (p->name[0] || udisk_role_name(p))) udisk_part_name(p, name, sizeof name);
        const char *fs = udisk_part_fs(p);
        snprintf(g_seg_detail[i], sizeof g_seg_detail[i], "%s%s%s%s%s", size, fs[0] ? " " : "", fs,
                 name[0] ? " \xb7 " : "", name);
        int mark = g_view_marks[part];
        // A partition a pending step makes or changes says so where a
        // mounted one shows its mount point -- it is never mounted.
        snprintf(g_seg_corner[i], sizeof g_seg_corner[i], "%s",
                 mark & UDISKOP_MARK_NEW ? "New" : mark ? "Changed" : p->point);
        if (p->total) s->used_pct = (int)(p->used * 100 / p->total);
        // Colour by what it IS: the firmware's in amber, data in the accent's tint.
        uui_diskplan_colour(s, p->role, p->point[0] != 0);
        if (mark) s->edge = UUI_ROLE(accent);
        s->corner = g_seg_corner[i][0] ? g_seg_corner[i] : 0;
    }
    s->title = g_seg_title[i];
    s->detail = g_seg_detail[i];
}

// Partitions in disk order, and every gap of a megabyte or more between
// the table's first usable sector and its last as free space.
static void build_map(void) {
    g_nsegs = 0;
    project();
    struct udisk *d = cur_disk();
    if (!d) { uui_segbar_set(&g_map, g_segs, 0); return; }
    udisk_sort(d);
    // A disk with no table gets a GPT, whose backup takes the last sectors.
    uint64_t at = MIB, end = d->kind == QUERY_PART_MBR ? d->sectors : d->sectors - GPT_TAIL;
    if (!d->kind && d->nparts) at = 0;   // a whole-disk volume
    for (int i = 0; i < d->nparts; i++) {
        const struct udisk_part *p = &d->parts[i];
        if (p->start > at && p->start - at >= MIB) add_seg(-1, at, p->start - at);
        add_seg(i, p->start, p->count);
        if (p->start + p->count > at) at = p->start + p->count;
    }
    if (end > at && end - at >= MIB && d->persistent) add_seg(-1, at, end - at);
    int keep = g_map.selected;
    uui_segbar_set(&g_map, g_segs, g_nsegs);
    g_map.selected = keep >= 0 && keep < g_nsegs ? keep : (g_nsegs ? 0 : -1);
}

// --- what may be done --------------------------------------------------------

// Why `cmd` is refused for the selection, or NULL when it may go ahead.
static int check_running(void);
static const char *job_wait_note(void);

// The step `cmd` would queue for the selection: what refusal() asks the
// queue's own check (udiskops_check()) and what the command then adds.
static void probe(int cmd, struct udiskop *op) {
    memset(op, 0, sizeof *op);
    struct udisk *d = cur_disk();
    struct udisk_part *p = cur_part();
    int s = g_map.selected;
    switch (cmd) {
    case CMD_NEW:
        op->kind = UDISKOP_CREATE;
        op->start = (g_seg_start[s] + MIB - 1) / MIB * MIB;   // aligned to a megabyte
        op->sectors = g_seg_start[s] + g_seg_count[s] > op->start ? g_seg_start[s] + g_seg_count[s] - op->start : 0;
        break;
    case CMD_DELETE: op->kind = UDISKOP_DELETE; break;
    case CMD_RESIZE: op->kind = UDISKOP_RESIZE; break;
    case CMD_FORMAT: op->kind = UDISKOP_FORMAT; snprintf(op->fs, sizeof op->fs, "tfs3"); break;
    default: break;
    }
    if (p) {
        op->start = p->start;
        op->sectors = p->count;
    }
    snprintf(op->disk, sizeof op->disk, "%s", d ? d->name : "");
}

static int pending_create_on(const char *disk) {
    for (int i = 0; i < g_q.count; i++)
        if (g_q.ops[i].kind == UDISKOP_CREATE && !strcmp(g_q.ops[i].disk, disk)) return 1;
    return 0;
}

static const char *refusal(int cmd) {
    struct udisk *d = cur_disk();
    struct udisk_part *p = cur_part();
    struct udiskop op;
    // Refresh too: reading a volume's usage waits behind the check's lock.
    if (check_running() && cmd != CMD_EXIT) return job_wait_note();
    switch (cmd) {
    case CMD_UNDO: case CMD_CLEAR: case CMD_APPLY:
        return g_q.count ? 0 : "No changes are waiting";
    default: break;
    }
    if (!d) return "Volumes in memory cannot be changed";
    switch (cmd) {
    case CMD_NEW:
        if (!cur_is_free()) {
            if (d->system) return "The system disk's partitions cannot be changed while it runs";
            return "Select free space";
        }
        probe(cmd, &op);
        return udiskops_check(d, g_view_marks, &op);
    // ONE ENTRY EDITED IN PLACE (SYS_PARTEDIT), so the rest of the table
    // -- any type, any number of entries, a mounted sibling -- is no
    // obstacle; only the partition itself must be free.
    case CMD_DELETE:
    // The table side; what the FILESYSTEM allows the dialog asks the
    // kernel when it opens (FSRESIZE_INFO), and says.
    case CMD_RESIZE:
    case CMD_FORMAT:
        if (!p || !p->number) return "Select a partition";
        if (d->system && cmd == CMD_FORMAT) return "The system disk cannot be formatted while it runs";
        probe(cmd, &op);
        return udiskops_check(d, g_view_marks, &op);
    case CMD_CHECK:
        if (!p || !p->point[0]) return "Check needs a mounted volume";
        return 0;
    case CMD_MOUNT:
        if (!p) return "Select a partition";
        if (d->system) return "The system disk's volumes stay as they are";
        if (!p->point[0] && cur_mark()) return "Changes to it are waiting: apply or remove them first";
        // Mounted, it would make the waiting rewrite of the table fail.
        if (!p->point[0] && pending_create_on(d->name)) return "A new partition waits to rewrite this disk's table";
        if (!p->point[0] && (p->role != UDISK_ROLE_DATA || !p->dev[0])) return "Nothing here can be mounted";
        if (!p->point[0] && p->type[0] && !p->readable) {
            static char why[64];
            snprintf(why, sizeof why, "SaunaOS cannot read %s", udisk_fs_title(p->type));
            return why;
        }
        return 0;
    default:
        return 0;
    }
}

static unsigned item_flags(int code) {
    if (code == CMD_EXIT || (code == CMD_REFRESH && !check_running())) return 0;
    return refusal(code) ? UUI_MI_DISABLED : 0;
}

// --- actions ---------------------------------------------------------------

static void say(const char *fmt, const char *a, const char *b) {
    snprintf(g_note, sizeof g_note, fmt, a, b);
    ulogf("disks: %s\n", g_note);
}

static void ask(struct uapp *a, const char *title, int rows, const struct uui_dialog_button *btns, int n,
                int def) {
    for (int i = 0; i < rows; i++) g_ask_rows[i] = g_ask_line[i];
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_ask, title, g_ask_rows, rows, btns, n, def, ASK_CANCEL);
    ulogf("disks: ask %s\n", title);
}

// Queue `op` on the selected disk; the status line says what happened.
static int queue(struct udiskop *op) {
    struct udisk *real = cur_real();
    if (!real) return -1;
    int lost;
    const char *why = udiskops_add(&g_q, real, op, &lost);
    if (why) { say("%s%s", why, ""); return -1; }
    steps_sync();
    ulogf("disks: queued %s %s (%d pending, %d dropped)\n", udiskop_verb(op->kind), op->what, g_q.count, lost);
    // Steps it made impossible went with it: say so, never silently.
    if (lost) snprintf(g_note, sizeof g_note, "Queued: %s %s -- and removed %d step%s that no longer fit",
                       udiskop_verb(op->kind), op->what, lost, lost == 1 ? "" : "s");
    else say("Queued: %s %s", udiskop_verb(op->kind), op->what);
    return 0;
}

// The form's partition, and its format when one is chosen: two steps.
static void do_create(void) {
    struct udisk *d = cur_disk();
    if (refusal(CMD_NEW) || !d) return;
    struct udiskop op;
    probe(CMD_NEW, &op);
    uint64_t spb = d->block_size / 512 ? d->block_size / 512 : 1;
    uui_sizefield_commit(&g_size);
    uint64_t mb = uui_sizefield_mib(&g_size), want = mb ? mb * MIB : op.sectors;
    if (want < op.sectors) op.sectors = want;
    op.sectors -= op.sectors % spb;
    snprintf(op.name, sizeof op.name, "%s", uui_textbox_text(&g_name));
    if (queue(&op) < 0) return;
    uui_textbox_set_text(&g_name, "");   // the next free space starts a fresh form
    int fs = g_fs.selected;
    if (fs == 2) return;
    struct udiskop f;
    memset(&f, 0, sizeof f);
    f.kind = UDISKOP_FORMAT;
    f.start = op.start;
    snprintf(f.fs, sizeof f.fs, "%s", fs == 0 ? "tfs3" : "fat32");
    queue(&f);
}

static void do_mount(void) {
    struct udisk_part *p = cur_part();
    if (!p || refusal(CMD_MOUNT)) return;
    int rc;
    if (p->point[0]) {
        char point[64];
        snprintf(point, sizeof point, "%s", p->point);
        rc = sys_umount(point) < 0 ? -errno : 0;
        if (rc < 0) { say("Could not unmount %s: %s", point, strerror(-rc)); return; }
        // A directory Disks made for it goes with it.
        if (!strncmp(point, "/mnt/", 5)) sys_unlink(point);
        say("Unmounted %s%s", point, "");
    } else {
        struct mount_request m;
        memset(&m, 0, sizeof m);
        snprintf(m.source, sizeof m.source, "%s", p->dev);
        snprintf(m.point, sizeof m.point, "/mnt/%s", p->dev);
        sys_mkdir(m.point);
        rc = sys_mount(&m) < 0 ? -errno : 0;
        if (rc < 0) {
            sys_unlink(m.point);
            say("Could not mount %s: %s", p->dev, rc == -ENODEV ? "no filesystem this system reads" : strerror(-rc));
            return;
        }
        say("Mounted %s at %s", p->dev, m.point);
    }
    scan();
}

// --- Check and Apply, on a worker -----------------------------------------------
//
// SYS_FS_CHECK, SYS_MKFS AND SYS_FS_RESIZE BLOCK FOR THE WHOLE PASS, so
// each runs on a worker and this thread reads how far it has got
// (FSCK_PROGRESS, MKFS_PROGRESS, FSRESIZE_PROGRESS, none taking a lock
// the pass holds) on every tick. The worker touches nothing but the
// job's answers and posts them.

#define SHOW_AFTER_MS 300   // a check quicker than this never shows a dialog...
#define SHOW_FOR_MS   500   // ...and one that does stays up this long, so it never flashes

// The worker's messages: a step began (a1 its index, its device written
// first), a step ended, and the whole job ended.
enum { EV_CHECK_DONE = 1, EV_STEP, EV_STEP_DONE };

static struct fs_check_result g_check;
static char g_check_point[64], g_check_title[32], g_job_title[48];
enum { JOB_CHECK, JOB_APPLY };
static struct {
    int kind;                      // JOB_*
    int running, repair, shown, stopping, finished;
    unsigned ms;                   // how long the pass took, to the worker's answer
    unsigned long long started_ns, shown_ns;
    int rc, err;                   // the worker's answer, read after EV_CHECK_DONE
} g_job;
static struct uui_stages g_stages;
static struct uui_item g_stages_item = { .ops = &uui_stages_ops, .widget = &g_stages, .name = "stages" };
static struct uapp *g_app;

// APPLY: THE WORKER'S OWN COPY OF THE STEPS, so nothing the window does
// reaches what it runs; the queue keeps its steps until the job ends,
// and then loses the ones that were done. Each array has a row per step.
static struct {
    struct udiskop *ops;
    int count, cur, failed;        // cur: the step running; failed: -1, or the one that failed
    int (*rc);
    char (*dev)[16];               // written before EV_STEP, read after it
    char (*why)[FS_RESIZE_WHY_MAX + 48];
    struct uui_step *rows;         // Apply's list, with each step's state
} g_apply;
static struct uui_steplist g_run;
static struct uui_item g_run_item = { .ops = &uui_table_ops, .widget = &g_run.table, .id = ID_RUN, .name = "applysteps" };
static char g_apply_note[160];

static int check_running(void) { return g_job.running; }
static const char *job_wait_note(void) {
    return g_job.kind == JOB_APPLY ? "Wait for the changes to finish" : "Wait for the check to finish";
}

static void step_started(void *ctx, const char *dev) {
    int i = (int)(intptr_t)ctx;
    snprintf(g_apply.dev[i], sizeof g_apply.dev[i], "%s", dev);
    uapp_post(g_app, EV_STEP, i);
}

static void *apply_worker(void *arg) {
    (void)arg;
    int rc = 0;
    for (int i = 0; i < g_apply.count && !rc; i++) {
        rc = udiskops_run(&g_apply.ops[i], g_apply.why[i], sizeof g_apply.why[i], step_started, (void *)(intptr_t)i);
        g_apply.rc[i] = rc;
        uapp_post(g_app, EV_STEP_DONE, i);
    }
    g_job.rc = rc;
    g_job.err = rc < 0 ? -rc : 0;
    uapp_post(g_app, EV_CHECK_DONE, 0);
    return 0;
}

static void *check_worker(void *arg) {
    unsigned flags = (unsigned)(uintptr_t)arg;
    int rc = sys_fs_check(g_check_point, flags, &g_check);
    g_job.err = rc < 0 ? errno : 0;
    g_job.rc = rc;
    uapp_post(g_app, EV_CHECK_DONE, 0);
    return 0;
}

// The progress dialog: Stop for a check (Escape too), nothing for a
// repair or Apply -- and reopened if anything closes it while the pass runs.
static void open_progress(struct uapp *a) {
    static struct uui_dialog_button stop[1];
    stop[0] = (struct uui_dialog_button){ g_job.stopping ? "Stopping..." : "Stop", ASK_STOP, 0 };
    int n = g_job.repair || g_job.kind != JOB_CHECK || g_job.stopping || g_job.finished ? 0 : 1;
    int sw, sh;
    if (g_job.kind == JOB_APPLY) {
        int rows = g_apply.count < 8 ? g_apply.count : 8;
        uui_dialog_set_body(&g_ask, &g_run_item, ugfx_char_w() * 72, uui_steplist_height(&g_run, rows));
    } else {
        uui_stages_natural_size(&g_stages, &sw, &sh);
        uui_dialog_set_body(&g_ask, &g_stages_item, sw, sh);
    }
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_ask, g_job_title, 0, 0, stop, n, n ? 0 : -1, n ? ASK_STOP : ASK_BUSY);
    if (!g_job.shown) g_job.shown_ns = uui_anim_now_ns();
    g_job.shown = 1;
}

// 1 while the kernel reports the pass running -- not g_job.running,
// which stays set until the worker's message is read, after the pass.
static int poll_progress(void) {
    struct fs_check_progress p;
    if (g_job.kind == JOB_APPLY) {
        int i = g_apply.cur;
        if (i < 0 || i >= g_apply.count || g_apply.rows[i].state != UUI_STEP_RUNNING) return 0;
        int k = g_apply.ops[i].kind;
        int rc = k == UDISKOP_FORMAT ? sys_mkfs_progress(g_apply.dev[i], &p)
               : k == UDISKOP_RESIZE ? sys_fs_resize_progress(g_apply.dev[i], &p) : -1;
        if (rc < 0 || !p.running || !p.total) return 0;
        // Every stage an equal share of the step.
        uint32_t stages = p.stages ? p.stages : 1, done = p.done > p.total ? p.total : p.done;
        g_apply.rows[i].permille = (int)(((uint64_t)p.stage * 1000 + (uint64_t)done * 1000 / p.total) / stages);
        if (g_apply.rows[i].permille > 1000) g_apply.rows[i].permille = 1000;
        return 1;
    }
    int rc = sys_fs_check_progress(g_check_point, &p);
    if (rc < 0 || !p.running) return 0;
    if (p.stages && (int)p.stages != g_stages.count) {
        const char *names[FSCK_STAGES_MAX];
        for (int i = 0; i < FSCK_STAGES_MAX; i++) names[i] = p.names[i];
        uui_stages_set_names(&g_stages, names, (int)p.stages);
    }
    int st = p.stage < FSCK_STAGES_MAX ? (int)p.stage : 0;
    uint32_t done = p.done > p.total ? p.total : p.done;
    uui_stages_set(&g_stages, st, p.total ? (int)((uint64_t)done * 1000 / p.total) : -1);
    if (p.total) snprintf(g_stages.line, sizeof g_stages.line, "%u of %u %s", done, p.total, p.units[st]);
    else snprintf(g_stages.line, sizeof g_stages.line, "%s", "Starting");
    snprintf(g_stages.detail[0], sizeof g_stages.detail[0], "%u files, %u blocks", p.records, p.blocks);
    if (st > 0)
        snprintf(g_stages.detail[st], sizeof g_stages.detail[st], p.problems == 1 ? "%u problem so far"
                                                                                  : "%u problems so far", p.problems);
    return 1;
}

// THE PASS HOLDS THE VOLUME'S LOCK, AND A PAGE OF libuapp.so NOT YET READ
// IN IS READ FROM THAT VOLUME -- so the first progress dialog of a boot
// would wait, undrawn, for the very pass it reports. Drawing it once
// into a scratch surface first brings those pages in while the volume
// is free -- and asking both progress calls once, with nothing running
// (a moved wrapper was the whole dialog lost, unseen).
static void warm_progress(void) {
    struct fs_check_progress none;
    sys_fs_check_progress("/", &none);
    sys_mkfs_progress("", &none);
    sys_fs_resize_progress("", &none);
    static const char *const names[] = { "Walk", "Compare", "Count" };
    struct uui_stages st;
    uui_stages_init(&st);
    uui_stages_set_names(&st, names, 3);
    uui_stages_set(&st, 1, 500);
    int w, h;
    uui_stages_natural_size(&st, &w, &h);
    struct ugfx_surface surf;
    memset(&surf, 0, sizeof surf);
    surf.w = w + 64;
    surf.h = h + 128;
    surf.pixels = malloc((size_t)surf.w * (size_t)surf.h * 4);
    if (!surf.pixels) return;
    st.x = st.y = 8; st.w = w; st.h = h;
    uui_stages_draw(&surf, &st);
    struct uui_dialog d;
    struct uui_dialog_button b = { "Stop", ASK_STOP, 0 };
    uui_dialog_init(&d);
    uui_dialog_set_bounds(&d, 0, 0, surf.w, surf.h);
    uui_dialog_open(&d, "Checking", 0, 0, &b, 1, 0, ASK_STOP);
    uui_dialog_draw(&surf, &d);
    free(surf.pixels);
}

static void start_check(struct uapp *a, unsigned flags) {
    g_app = a;
    warm_progress();
    memset(&g_check, 0, sizeof g_check);
    memset(&g_job, 0, sizeof g_job);
    g_job.repair = flags & FSCK_REPAIR ? 1 : 0;
    g_job.started_ns = uui_anim_now_ns();
    snprintf(g_job_title, sizeof g_job_title, "%s %s", g_job.repair ? "Repairing" : "Checking", g_check_title);
    uui_stages_set_names(&g_stages, 0, 0);
    uui_stages_set(&g_stages, 0, -1);
    snprintf(g_stages.line, sizeof g_stages.line, "%s", "Starting");
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    g_job.running = 1;
    if (pthread_create(&th, &at, check_worker, (void *)(uintptr_t)flags) != 0) {
        g_job.running = 0;
        say("Could not start the check of %s: %s", g_check_point, "no thread");
        return;
    }
    ulogf("disks: check %s %s started\n", g_check_point, g_job.repair ? "repair" : "read-only");
    say("%s%s", g_job_title, "...");
}

static void apply_free(void) {
    free(g_apply.ops);
    free(g_apply.rc);
    free(g_apply.dev);
    free(g_apply.why);
    free(g_apply.rows);
    memset(&g_apply, 0, sizeof g_apply);
}

// Apply's list: every step, each Waiting.
static int apply_load(void) {
    apply_free();
    int n = g_q.count;
    g_apply.ops = malloc((size_t)n * sizeof *g_apply.ops);
    g_apply.rc = calloc((size_t)n, sizeof *g_apply.rc);
    g_apply.dev = calloc((size_t)n, sizeof *g_apply.dev);
    g_apply.why = calloc((size_t)n, sizeof *g_apply.why);
    g_apply.rows = calloc((size_t)n, sizeof *g_apply.rows);
    if (!g_apply.ops || !g_apply.rc || !g_apply.dev || !g_apply.why || !g_apply.rows) {
        apply_free();
        return -1;
    }
    memcpy(g_apply.ops, g_q.ops, (size_t)n * sizeof *g_apply.ops);
    g_apply.count = n;
    g_apply.cur = g_apply.failed = -1;
    for (int i = 0; i < n; i++)
        g_apply.rows[i] = (struct uui_step){ udiskop_verb(g_apply.ops[i].kind), g_apply.ops[i].what, UUI_STEP_WAITING, -1 };
    uui_steplist_set(&g_run, g_apply.rows, n);
    return 0;
}

// The confirm: every step in its order, and what a step cut short costs.
static void confirm_apply(struct uapp *a) {
    if (apply_load() < 0) { say("Out of memory%s%s", "", ""); return; }
    int n = g_apply.count, unsafe = 0;
    for (int i = 0; i < n; i++) unsafe |= g_apply.ops[i].kind == UDISKOP_RESIZE && g_apply.ops[i].unsafe;
    static char title[48];
    snprintf(title, sizeof title, n == 1 ? "Apply %d change?" : "Apply %d changes?", n);
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s", "They run in this order. A partition being changed stays unmounted until they finish.");
    snprintf(g_ask_line[1], sizeof g_ask_line[1], "%s", "If one fails, the ones after it are not run and stay in the list.");
    static struct uui_dialog_button btns[2];
    btns[0] = (struct uui_dialog_button){ "Apply", ASK_APPLY, UUI_DLG_PRIMARY };
    btns[1] = (struct uui_dialog_button){ "Cancel", ASK_CANCEL, 0 };
    int rows = n < 8 ? n : 8;
    uui_dialog_set_body(&g_ask, &g_run_item, ugfx_char_w() * 72, uui_steplist_height(&g_run, rows));
    ask(a, title, 2, btns, 2, 1);
    g_ask.body_full = 1;
    if (unsafe) {
        snprintf(g_apply_note, sizeof g_apply_note, "%s",
                 "FAT32 has no journal: if its resize is cut short while its data moves, the volume is lost. Back it up first.");
        uui_dialog_set_note(&g_ask, g_apply_note);
        ulogf("disks: apply note %s\n", g_apply_note);
    }
}

// Every step on the worker, in order, in the steps' own dialog.
static void start_apply(struct uapp *a) {
    g_app = a;
    warm_progress();
    memset(&g_job, 0, sizeof g_job);
    g_job.kind = JOB_APPLY;
    g_job.started_ns = uui_anim_now_ns();
    snprintf(g_job_title, sizeof g_job_title, g_apply.count == 1 ? "Applying %d change" : "Applying %d changes",
             g_apply.count);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    g_job.running = 1;
    if (pthread_create(&th, &at, apply_worker, 0) != 0) {
        g_job.running = 0;
        say("Could not apply the changes: %s%s", "no thread", "");
        return;
    }
    ulogf("disks: apply %d steps started\n", g_apply.count);
    say("%s%s", g_job_title, "...");
    open_progress(a);
}

// A step began or ended (the worker's EV_STEP and EV_STEP_DONE).
static void apply_step(int ev, int i) {
    if (i < 0 || i >= g_apply.count) return;
    if (ev == EV_STEP) {
        g_apply.cur = i;
        g_apply.rows[i].state = UUI_STEP_RUNNING;
        ulogf("disks: apply step %d %s %s on %s\n", i + 1, udiskop_verb(g_apply.ops[i].kind), g_apply.ops[i].what,
              g_apply.dev[i][0] ? g_apply.dev[i] : g_apply.ops[i].disk);
        return;
    }
    int rc = g_apply.rc[i];
    ulogf("disks: apply step %d -> %d %s\n", i + 1, rc, g_apply.why[i]);
    g_apply.rows[i].state = rc < 0 ? UUI_STEP_FAILED : UUI_STEP_DONE;
    g_apply.rows[i].permille = -1;
    if (rc < 0) {
        g_apply.failed = i;
        for (int k = i + 1; k < g_apply.count; k++) g_apply.rows[k].state = UUI_STEP_NOT_RUN;
    }
}

// The job ended: the steps that were done leave the queue, and a step
// that failed is shown with why and what was not run.
static void apply_done(struct uapp *a) {
    int done = 0;
    while (done < g_apply.count && g_apply.rows[done].state == UUI_STEP_DONE) done++;
    // Off the queue WITHOUT a replay against the old scan, which would
    // judge what is left against the disk as it was before they ran.
    udiskops_ran(&g_q, done);
    ulogf("disks: apply %d of %d done in %u ms\n", done, g_apply.count, g_job.ms);
    scan();
    if (g_apply.failed < 0) {
        snprintf(g_note, sizeof g_note, done == 1 ? "Applied %d change" : "Applied %d changes", done);
        return;
    }
    int f = g_apply.failed;
    static char title[48];
    snprintf(title, sizeof title, "Step %d of %d failed", f + 1, g_apply.count);
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s.", g_apply.why[f][0] ? g_apply.why[f] : strerror(-g_apply.rc[f]));
    // The rescan drops what the failure made impossible (a partition
    // mounted behind Disks' back), so say what is really left.
    int left = g_apply.count - done;
    snprintf(g_ask_line[1], sizeof g_ask_line[1], "%s%s",
             done ? "The steps before it are done. " : "Nothing was changed. ",
             g_q.count == left ? "It and the ones after it are still in the pending list."
             : g_q.count ? "Those that can still be done are in the pending list."
                         : "None of the rest can be done now, so the pending list is empty.");
    static struct uui_dialog_button ok[1];
    ok[0] = (struct uui_dialog_button){ "Close", ASK_OK, UUI_DLG_PRIMARY };
    int rows = g_apply.count < 8 ? g_apply.count : 8;
    uui_dialog_set_body(&g_ask, &g_run_item, ugfx_char_w() * 72, uui_steplist_height(&g_run, rows));
    ask(a, title, 2, ok, 1, 0);
    g_ask.body_full = 1;
    snprintf(g_note, sizeof g_note, "Step %d failed: %s", f + 1, g_ask_line[0]);
}

static void do_check(struct uapp *a, unsigned flags) {
    struct udisk_part *p = cur_part();
    if (!flags) {
        if (!p || refusal(CMD_CHECK)) return;
        snprintf(g_check_point, sizeof g_check_point, "%s", p->point);
        udisk_part_title(p, g_check_title, sizeof g_check_title);
    }
    start_check(a, flags);
}

// The worker is done: say what it found, as the dialog always did.
static void check_done(struct uapp *a) {
    unsigned flags = g_job.repair ? FSCK_REPAIR : 0;
    g_job.running = 0;
    if (uui_dialog_is_open(&g_ask)) uui_dialog_close(&g_ask);
    uui_dialog_set_body(&g_ask, 0, 0, 0);
    unsigned ms = g_job.ms;
    if (g_job.kind == JOB_APPLY) { apply_done(a); return; }
    if (g_job.rc < 0 && g_job.err == ECANCELED) {
        ulogf("disks: check %s stopped after %u ms\n", g_check_point, ms);
        say("Stopped checking %s%s", g_check_point, "");
        return;
    }
    if (g_job.rc < 0) { say("Could not check %s: %s", g_check_point, strerror(g_job.err)); return; }
    unsigned bad = fs_check_problems(&g_check);
    ulogf("disks: check %s %s -> %u problems in %u ms\n", g_check_point, flags ? "repair" : "read-only", bad, ms);
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s on %s: %u records, %u blocks in use.", g_check.fstype,
             g_check_point, g_check.records_used, g_check.blocks_referenced);
    static struct uui_dialog_button btns[2];
    if (!bad) {
        snprintf(g_ask_line[1], sizeof g_ask_line[1], "%s", flags ? "Repaired: no problems remain." : "No problems found.");
        btns[0] = (struct uui_dialog_button){ "OK", ASK_OK, 0 };
        ask(a, "Check", 2, btns, 1, 0);
        say("%s: no problems%s", g_check_point, "");
        return;
    }
    snprintf(g_ask_line[1], sizeof g_ask_line[1], "%u leaked, %u in use but free, %u twice used, %u out of range.",
             g_check.leaked, g_check.referenced_but_free, g_check.double_allocated, g_check.out_of_range);
    int rows = 2;
    if (g_check.counts_wrong + g_check.links_wrong + g_check.bad_names + g_check.dir_errors)
        snprintf(g_ask_line[rows++], sizeof g_ask_line[2],
                 "%u free counts wrong, %u link counts wrong, %u damaged names, %u directory errors.",
                 g_check.counts_wrong, g_check.links_wrong, g_check.bad_names, g_check.dir_errors);
    btns[0] = (struct uui_dialog_button){ "Repair", ASK_REPAIR, UUI_DLG_PRIMARY };
    btns[1] = (struct uui_dialog_button){ "Close", ASK_CANCEL, 0 };
    ask(a, "Check found problems", rows, btns, 2, 0);
}

// Every tick while a pass runs: read it, show the dialog once it has
// taken long enough to be worth one, and keep asking a stop that may
// have arrived before the pass began (FSCK_STOP is then -ESRCH).
static int check_tick(struct uapp *a) {
    if (!g_job.running) return 0;
    if (g_job.finished) {
        if (uui_anim_now_ns() - g_job.shown_ns < SHOW_FOR_MS * 1000000ull) return 0;
        check_done(a);
        build_map();
        return 1;
    }
    if (g_job.stopping) {
        static int last = 1;
        int rc = sys_fs_check_stop(g_check_point) ? -errno : 0;
        if (rc != last) ulogf("disks: check %s stop asked again -> %d\n", g_check_point, rc);
        last = rc;
    }
    int live = poll_progress();
    if (!g_job.shown && live && uui_anim_now_ns() - g_job.started_ns >= SHOW_AFTER_MS * 1000000ull) {
        ulogf("disks: check progress shown\n");
        open_progress(a);
    }
    uui_stages_tick(&g_stages);
    return g_job.shown;
}

// --- Resize: R1, the partition's end dragged on a small map --------------------
//
// THE BAR IS THE VALUE (ui/uui_resizebar.h); "New size" and "Free space
// after" echo it, and typing in either moves it. What the FILESYSTEM
// allows is the kernel's answer (FSRESIZE_INFO): its size, what is in
// use, how far it grows and why it cannot shrink. A filesystem nothing
// resizes gets a message saying so, never a bar to drag.
static struct uui_resizebar g_rz_bar;
static struct uui_sizefield g_rz_size, g_rz_after;
static struct uui_custom g_rz_cap, g_rz_lsize, g_rz_lafter;
static struct uui_item g_rz_size_it[2], g_rz_after_it[2], g_rz_col_it[4], g_rz_body;
static struct uui_layout g_rz_size_row, g_rz_after_row, g_rz_col;
static struct {
    char dev[16];
    int has_fs, unsafe;
    uint64_t start;              // the partition's first sector: what the queued step names
    uint64_t base;               // where the partition starts on the bar (the neighbour before it)
    uint64_t now, least, most;   // the partition's length now and its limits, sectors
    uint64_t now_mib, least_mib, most_mib;
    uint64_t synced_end, synced_size, synced_after;   // what the bar and fields last agreed on
    char smallest[48], largest[48], label[40], line[160], note[FS_RESIZE_WHY_MAX + 40];
} g_rz;

static void rz_text(struct ugfx_surface *s, const struct uui_custom *cu) {
    ugfx_draw_string_clipped(s, cu->x, cu->y + (cu->h - ugfx_char_h()) / 2, cu->w, (const char *)cu->state,
                             UTHEME_TEXT, UGFX_TRANSPARENT);
}

// "Smallest ..." at the left, "Largest ..." at the right, dim.
static void rz_caption(struct ugfx_surface *s, const struct uui_custom *cu) {
    uint32_t ink = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    int lw = ugfx_text_width(g_rz.largest), y = cu->y + (cu->h - ugfx_char_h()) / 2;
    if (lw > cu->w / 2) lw = cu->w / 2;
    ugfx_draw_string_clipped(s, cu->x, y, cu->w - lw - ugfx_char_w(), g_rz.smallest, ink, UGFX_TRANSPARENT);
    ugfx_draw_string_clipped(s, cu->x + cu->w - lw, y, lw, g_rz.largest, ink, UGFX_TRANSPARENT);
}

// A size the fields show, in MiB, as sectors: the limits and the size now
// exactly, so neither rounding drops a sector nor adds one.
static uint64_t rz_sectors_of(uint64_t mib) {
    if (mib >= g_rz.most_mib) return g_rz.most;
    if (mib <= g_rz.least_mib) return g_rz.least;
    if (mib == g_rz.now_mib) return g_rz.now;
    return mib * MIB;
}

static uint64_t rz_mib_of(uint64_t sectors) {
    if (sectors >= g_rz.most) return g_rz.most_mib;
    if (sectors <= g_rz.least) return g_rz.least_mib;
    if (sectors == g_rz.now) return g_rz.now_mib;
    return sectors / MIB;
}

// The bar, the size and the room after it, made to agree with whichever
// moved last.
static void rz_follow(void) {
    uint64_t len = g_rz_bar.end - g_rz.base;
    if (len == g_rz.synced_end) {
        uint64_t sz = uui_sizefield_mib(&g_rz_size), af = uui_sizefield_mib(&g_rz_after);
        if (sz != g_rz.synced_size) len = rz_sectors_of(sz);
        else if (af != g_rz.synced_after) len = af == 0 ? g_rz.most : rz_sectors_of(g_rz.most_mib - af);
        else return;
        uui_resizebar_set_end(&g_rz_bar, g_rz.base + len);
        len = g_rz_bar.end - g_rz.base;
    }
    g_rz.synced_end = len;
    g_rz.synced_size = rz_mib_of(len);
    g_rz.synced_after = (g_rz.most - len) / MIB;
    uui_sizefield_set_mib(&g_rz_size, g_rz.synced_size);
    uui_sizefield_set_mib(&g_rz_after, g_rz.synced_after);
    g_rz.synced_size = uui_sizefield_mib(&g_rz_size);
    g_rz.synced_after = uui_sizefield_mib(&g_rz_after);
}

static int rz_is_open(void) { return uui_dialog_is_open(&g_ask) && g_ask.body == &g_rz_body; }

static void rz_message(struct uapp *a, const char *title) {
    static struct uui_dialog_button ok[1];
    ok[0] = (struct uui_dialog_button){ "OK", ASK_OK, 0 };
    ask(a, title, 2, ok, 1, 0);
}

static void rz_open(struct uapp *a) {
    struct udisk *d = cur_disk();
    struct udisk_part *p = cur_part();
    if (!d || !p || refusal(CMD_RESIZE)) return;
    struct fsresize_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.source, sizeof req.source, "%s", p->dev);
    req.flags = FSRESIZE_INFO;
    int rc = sys_fs_resize(&req) < 0 ? -errno : 0;
    const struct fs_resize_info *fi = &req.info;
    char title[32], size[24];
    udisk_part_title(p, title, sizeof title);
    const char *fs = fi->fstype[0] ? udisk_fs_title(fi->fstype) : "";
    ulogf("disks: resize info %s -> %d fs=%s flags=%u size=%llu used=%llu max=%llu why=%s\n", p->dev, rc, fi->fstype,
          fi->flags, (unsigned long long)fi->size_bytes, (unsigned long long)fi->used_bytes,
          (unsigned long long)fi->max_bytes, fi->why);
    if (rc < 0 || !(fi->flags & (FS_RESIZE_CAN_GROW | FS_RESIZE_CAN_SHRINK))) {
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s (%s%s%s) cannot be resized: %s.", p->dev, title,
                 fs[0] ? ", " : "", fs, fi->why[0] ? fi->why : strerror(-rc));
        snprintf(g_ask_line[1], sizeof g_ask_line[1], "%s", "tfs3 and FAT32 volumes can grow; shrinking any filesystem is not built yet.");
        rz_message(a, "Resize partition");
        return;
    }
    uint64_t spb = d->block_size > 512 ? d->block_size / 512 : 1;
    uint64_t room = udisk_room_after(d, p), most = p->count + room;
    if (fs[0] && fi->max_bytes / 512 < most) most = fi->max_bytes / 512;
    most -= most % spb;
    if (most < p->count) most = p->count;
    uint64_t least = p->count;
    if (fi->flags & FS_RESIZE_CAN_SHRINK) least = fs[0] ? (fi->min_bytes + 511) / 512 : MIB;
    if (least > p->count) least = p->count;
    if (most == p->count && least == p->count) {
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "There is no free space right after %s for it to grow into.", p->dev);
        snprintf(g_ask_line[1], sizeof g_ask_line[1], "%s", fi->why[0] ? fi->why : "");
        rz_message(a, "Resize partition");
        return;
    }
    snprintf(g_rz.dev, sizeof g_rz.dev, "%s", p->dev);
    g_rz.has_fs = fs[0] != 0;
    g_rz.unsafe = g_rz.has_fs && (fi->flags & FS_RESIZE_UNSAFE);
    g_rz.start = p->start;
    g_rz.now = p->count;
    g_rz.least = least;
    g_rz.most = most;
    g_rz.now_mib = (p->count + MIB / 2) / MIB;
    g_rz.least_mib = (least + MIB - 1) / MIB;
    g_rz.most_mib = most / MIB;
    if (g_rz.least_mib > g_rz.now_mib) g_rz.least_mib = g_rz.now_mib;
    if (g_rz.most_mib < g_rz.now_mib) g_rz.most_mib = g_rz.now_mib;

    // The map: a glimpse of the neighbour before it, the partition, the
    // room it may take, and a glimpse of what follows -- each neighbour
    // at most a quarter of the run, so a big one does not squeeze the
    // part being dragged to a sliver.
    int s = g_map.selected;
    uint64_t before = s > 0 ? g_seg_count[s - 1] : 0, after = 0;
    int next = s + 1 < g_nsegs && g_seg_part[s + 1] < 0 ? s + 2 : s + 1;
    if (next < g_nsegs && most == p->count + room) after = g_seg_count[next];
    if (before > most / 4) before = most / 4;
    if (after > most / 4) after = most / 4;
    g_rz.base = before;
    uint64_t used = g_rz.has_fs ? fi->used_bytes / 512 : 0;
    uui_resizebar_init(&g_rz_bar);
    g_rz_bar.step = MIB;
    uui_resizebar_set(&g_rz_bar, before + most + after, before, before + p->count, before + least, before + most, used);
    if (g_rz.has_fs) {
        human_size_iec(size, sizeof size, fi->used_bytes);
        snprintf(g_rz.label, sizeof g_rz.label, "used %s", size);
        g_rz_bar.label = g_rz.label;
    }
    human_size_iec(size, sizeof size, least * 512);
    snprintf(g_rz.smallest, sizeof g_rz.smallest, "Smallest %s", size);
    human_size_iec(size, sizeof size, most * 512);
    snprintf(g_rz.largest, sizeof g_rz.largest, "Largest %s", size);

    uui_sizefield_init(&g_rz_size, g_rz.now_mib, g_rz.least_mib, g_rz.most_mib);
    uui_sizefield_init(&g_rz_after, (most - p->count) / MIB, 0, (most - least) / MIB);
    g_rz.synced_end = ~0ull;
    rz_follow();

    int lw = ugfx_text_width("Free space after") + ugfx_char_w() * 2, ch = utheme_control_h();
    g_rz_cap = (struct uui_custom){ .w = ugfx_char_w() * 40, .h = ugfx_char_h() + 2, .draw = rz_caption };
    g_rz_lsize = (struct uui_custom){ .w = lw, .h = ch, .draw = rz_text, .state = (void *)"New size" };
    g_rz_lafter = (struct uui_custom){ .w = lw, .h = ch, .draw = rz_text, .state = (void *)"Free space after" };
    g_rz_size_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_rz_lsize };
    g_rz_size_it[1] = (struct uui_item){ .ops = &uui_sizefield_ops, .widget = &g_rz_size, .id = ID_RZ_SIZE, .name = "rzsize" };
    g_rz_after_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_rz_lafter };
    g_rz_after_it[1] = (struct uui_item){ .ops = &uui_sizefield_ops, .widget = &g_rz_after, .id = ID_RZ_AFTER, .name = "rzafter" };
    g_rz_size_row = (struct uui_layout){ .dir = UUI_ROW, .items = g_rz_size_it, .count = 2 };
    g_rz_after_row = (struct uui_layout){ .dir = UUI_ROW, .items = g_rz_after_it, .count = 2 };
    g_rz_col_it[0] = (struct uui_item){ .ops = &uui_resizebar_ops, .widget = &g_rz_bar, .id = ID_RZ_BAR,
                                        .flags = UUI_FILL_W, .name = "rzbar" };
    g_rz_col_it[1] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_rz_cap, .flags = UUI_FILL_W, .name = "rzcaption" };
    g_rz_col_it[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_rz_size_row };
    g_rz_col_it[3] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_rz_after_row };
    g_rz_col = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_rz_col_it, .count = 4 };
    g_rz_body = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_rz_col, .name = "rzbody" };
    int bw, bh;
    uui_layout_ops.natural_size(&g_rz_col, &bw, &bh);
    if (bw < ugfx_char_w() * 52) bw = ugfx_char_w() * 52;

    snprintf(g_rz.line, sizeof g_rz.line, "%s",
             g_rz.has_fs ? "Its filesystem grows with the partition when you apply the change."
                         : "Nothing SaunaOS recognises is on it: only the table changes.");
    g_ask_line[0][0] = 0;
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s", g_rz.line);
    g_rz.note[0] = 0;
    // A grow nothing makes atomic (FAT32) outranks why it cannot shrink.
    if (g_rz.has_fs && (fi->flags & FS_RESIZE_UNSAFE))
        snprintf(g_rz.note, sizeof g_rz.note,
                 "%s has no journal: if this is cut short while its data moves, the volume is lost. Back it up first.",
                 fs);
    else if (g_rz.has_fs && !(fi->flags & FS_RESIZE_CAN_SHRINK) && fi->why[0])
        snprintf(g_rz.note, sizeof g_rz.note, "%s.", fi->why);
    char heading[96];
    static char title_buf[96];
    snprintf(heading, sizeof heading, "Resize %s (%s%s%s)", p->dev, title, fs[0] ? ", " : "", fs);
    snprintf(title_buf, sizeof title_buf, "%s", heading);
    static struct uui_dialog_button btns[2];
    btns[0] = (struct uui_dialog_button){ "Resize", ASK_RESIZE, UUI_DLG_PRIMARY };
    btns[1] = (struct uui_dialog_button){ "Cancel", ASK_CANCEL, 0 };
    uui_dialog_set_body(&g_ask, &g_rz_body, bw, bh);
    ask(a, title_buf, 1, btns, 2, 0);
    g_ask.body_full = 1;
    if (g_rz.note[0]) {
        uui_dialog_set_note(&g_ask, g_rz.note);
        ulogf("disks: resize note %s\n", g_rz.note);
    }
    struct uui_item *tabs[] = { &g_rz_col_it[0], &g_rz_size_it[1], &g_rz_after_it[1] };
    uui_dialog_set_tabs(&g_ask, tabs, 3);
    uui_dialog_focus(&g_ask, &g_rz_size_it[1]);
}

// Resize was pressed: whatever was typed last wins, then the step queues.
static void rz_commit(void) {
    uui_sizefield_commit(&g_rz_size);
    uui_sizefield_commit(&g_rz_after);
    rz_follow();
    uint64_t len = g_rz_bar.end - g_rz.base;
    uui_dialog_set_body(&g_ask, 0, 0, 0);
    if (len == g_rz.now) { say("%s keeps its size%s", g_rz.dev, ""); return; }
    struct udiskop op;
    memset(&op, 0, sizeof op);
    op.kind = UDISKOP_RESIZE;
    op.start = g_rz.start;
    op.sectors = len;
    op.has_fs = g_rz.has_fs;
    op.unsafe = g_rz.unsafe;
    queue(&op);
}

static void command(struct uapp *a, int cmd) {
    struct udisk *d = cur_disk();
    struct udisk_part *p = cur_part();
    char size[24], title[32];
    const char *why = refusal(cmd);
    if (why && cmd != CMD_EXIT && (cmd != CMD_REFRESH || check_running())) { say("%s%s", why, ""); return; }
    static struct uui_dialog_button btns[3];
    switch (cmd) {
    case CMD_REFRESH: scan(); build_map(); say("Refreshed%s%s", "", ""); break;
    case CMD_NEW:     uui_focus_set(&g_focus, RING_SIZE); break;
    case CMD_FORMAT:
        human_size_iec(size, sizeof size, p->count * 512);
        udisk_part_title(p, title, sizeof title);
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "Format %s (%s, %s) on %s?", p->dev[0] ? p->dev : "the new partition",
                 title, size, d->name);
        snprintf(g_ask_line[1], sizeof g_ask_line[1], "Everything on it is erased when you apply the change.");
        btns[0] = (struct uui_dialog_button){ "Format as tfs3", ASK_FORMAT_TFS3, UUI_DLG_PRIMARY };
        btns[1] = (struct uui_dialog_button){ "Format as FAT32", ASK_FORMAT_FAT, 0 };
        btns[2] = (struct uui_dialog_button){ "Cancel", ASK_CANCEL, 0 };
        ask(a, "Format partition", 2, btns, 3, 2);
        break;
    case CMD_CHECK:   do_check(a, 0); break;
    case CMD_MOUNT:   do_mount(); break;
    case CMD_RESIZE:  rz_open(a); break;
    // Queued at once, as GParted does: Undo takes it back, and Apply's
    // confirm is where it is agreed to.
    case CMD_DELETE: {
        struct udiskop op;
        probe(CMD_DELETE, &op);
        queue(&op);
        break;
    }
    case CMD_UNDO: {
        char what[sizeof g_q.ops[0].what];
        snprintf(what, sizeof what, "%s %s", udiskop_verb(g_q.ops[g_q.count - 1].kind), g_q.ops[g_q.count - 1].what);
        int more = udiskops_remove(&g_q, &g_scan, g_q.count - 1);
        steps_sync();
        ulogf("disks: undo %s (+%d)\n", what, more);
        say("Removed: %s%s", what, more ? ", and the steps that needed it" : "");
        break;
    }
    case CMD_CLEAR:
        udiskops_clear(&g_q);
        steps_sync();
        ulogf("disks: pending changes cleared\n");
        say("Removed every pending change%s%s", "", "");
        break;
    case CMD_APPLY: confirm_apply(a); break;
    case CMD_EXIT: uapp_quit(a, 0); break;
    default: break;
    }
    build_map();   // the queue may have changed: the map shows its end state
}

static void answer(struct uapp *a, int code) {
    struct udisk_part *p = cur_part();
    // A body is the Resize dialog's, or Apply's list, until an answer closes it.
    if (g_ask.body == &g_rz_body && code != ASK_RESIZE) uui_dialog_set_body(&g_ask, 0, 0, 0);
    if (g_ask.body == &g_run_item) uui_dialog_set_body(&g_ask, 0, 0, 0);
    switch (code) {
    case ASK_FORMAT_TFS3:
    case ASK_FORMAT_FAT:
        if (!p || refusal(CMD_FORMAT)) break;
        {
            struct udiskop op;
            probe(CMD_FORMAT, &op);
            snprintf(op.fs, sizeof op.fs, "%s", code == ASK_FORMAT_TFS3 ? "tfs3" : "fat32");
            queue(&op);
        }
        break;
    case ASK_RESIZE: rz_commit(); break;
    case ASK_APPLY:  if (!check_running() && g_q.count) start_apply(a); break;
    case ASK_REPAIR: do_check(a, FSCK_REPAIR); break;
    case ASK_STOP:
        if (!g_job.running || g_job.repair) break;
        g_job.stopping = 1;
        ulogf("disks: check %s stop asked -> %d\n", g_check_point, sys_fs_check_stop(g_check_point) ? -errno : 0);
        break;
    default: break;
    }
    // Nothing closes the progress of a pass still running.
    if (g_job.running && g_job.shown && !uui_dialog_is_open(&g_ask)) open_progress(a);
    build_map();
}

// --- drawing ----------------------------------------------------------------

static void drive_card(struct ugfx_surface *s, void *ctx, int index, int x, int y, int w, int h, int state) {
    (void)ctx;
    int pad = ugfx_char_w(), lh = ugfx_char_h(), sel = state & UUI_GRID_SELECTED;
    uint32_t ink = sel ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, ground = sel ? UTHEME_ACCENT : uui_grid_c_bg(&g_drives);
    char line1[48], line2[64], size[24];
    const char *icon = "drive";
    if (index < g_scan.ndisks) {
        const struct udisk *d = &g_scan.disks[index];
        human_size_iec(size, sizeof size, d->sectors * 512);
        snprintf(line1, sizeof line1, "%s", d->name);
        // Changes waiting outrank the table's kind: the card is narrow.
        int waiting = udiskops_count_on(&g_q, d->name);
        if (waiting) snprintf(line2, sizeof line2, "%s  %d pending", size, waiting);
        else snprintf(line2, sizeof line2, "%s  %s%s", size,
                      d->kind == QUERY_PART_GPT ? "GPT" : d->kind == QUERY_PART_MBR ? "MBR" : "no table",
                      d->system ? "  system" : "");
        if (!d->persistent) icon = "drive-ram";
    } else {
        const struct udisk_ram *r = &g_scan.ram[index - g_scan.ndisks];
        human_size_iec(size, sizeof size, r->total);
        snprintf(line1, sizeof line1, "%s", r->point);
        snprintf(line2, sizeof line2, "%s  %s", size, r->fs);
        icon = "drive-ram";
    }
    int px = lh * 2;
    const struct uimg *ico = icon_get(icon, px);
    if (ico) ugfx_blit_alpha(s, x + pad, y + (h - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
    int tx = x + pad * 2 + px, room = w - (tx - x) - pad, ty = y + (h - lh * 2 - 2) / 2;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, tx, ty, room, line1, ink, ground);
    ugfx_set_font(was);
    ugfx_draw_string_clipped(s, tx, ty + lh + 2, room, line2, sel ? ink : uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED),
                             ground);
}

static void fact(struct ugfx_surface *s, int x, int *y, int vx, int right, const char *k, const char *v, uint32_t col) {
    ugfx_draw_string(s, x, *y, k, uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), UTHEME_WHITE);
    ugfx_draw_string_clipped(s, vx, *y, right - vx, v, col, UTHEME_WHITE);
    *y += ugfx_char_h() + 4;
}

static void guid_text(const uint8_t *g, char *out, size_t cap) {
    // GPT's mixed endianness: the first three groups are little-endian.
    snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1], g[0],
             g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int g_pane_x, g_pane_y, g_facts_y;

static void draw_details(struct ugfx_surface *s, int cw) {
    int pad = ugfx_char_w(), lh = ugfx_char_h(), x = g_pane_x + pad * 2, right = cw - pad * 2;
    int vx = x + ugfx_text_width("Partition table") + pad * 2, y = g_pane_y + pad * 2;
    char a[96], b[64], c[64];
    struct udisk *d = cur_disk();
    struct udisk_ram *r = cur_ram();
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string(s, x, y, d ? d->name : r ? r->point : "No drives", UTHEME_TEXT, UTHEME_WHITE);
    int nw = ugfx_text_width(d ? d->name : r ? r->point : "No drives");
    ugfx_set_font(was);
    if (d) {
        human_size_iec(b, sizeof b, d->sectors * 512);
        snprintf(a, sizeof a, "%s %s disk, %llu-byte sectors%s%s", b, d->driver[0] ? d->driver : "",
                 (unsigned long long)d->block_size, d->model[0] ? " -- " : "", d->model);
        ugfx_draw_string_clipped(s, x + nw + pad, y, right - x - nw - pad, a, UTHEME_TEXT, UTHEME_WHITE);
        y += lh + pad;
        snprintf(a, sizeof a, "%s, %d partition%s", d->kind == QUERY_PART_GPT ? "GPT" : d->kind == QUERY_PART_MBR ? "MBR" : "None",
                 d->kind ? d->nparts : 0, d->nparts == 1 ? "" : "s");
        fact(s, x, &y, vx, right, "Partition table", a, UTHEME_TEXT);
        static const uint8_t none[16];
        if (d->kind == QUERY_PART_GPT && memcmp(d->guid, none, 16)) {   // a GPT a step makes has none yet
            guid_text(d->guid, a, sizeof a);
            fact(s, x, &y, vx, right, "Disk GUID", a, UTHEME_TEXT);
        }
    } else if (r) {
        y += lh + pad;
        human_size_iec(b, sizeof b, r->used);
        human_size_iec(c, sizeof c, r->total);
        snprintf(a, sizeof a, "%s in memory, mounted at %s", r->fs, r->point);
        fact(s, x, &y, vx, right, "Contents", a, UTHEME_TEXT);
        snprintf(a, sizeof a, "%s of %s; gone at a restart", b, c);
        fact(s, x, &y, vx, right, "Used", a, UTHEME_TEXT);
        return;
    }
    if (!d) return;
    y += pad / 2;
    ugfx_draw_string(s, x, y, "Volumes", utheme_action(UTHEME_ACT_NAV), UTHEME_WHITE);
    g_facts_y = g_map.y + g_map.h + pad * 2;
    y = g_facts_y;
    ugfx_fill_rect(s, x, y - pad, right - x, 1, UTHEME_SEPARATOR);
    int sidx = g_map.selected;
    struct udisk_part *p = cur_part();
    if (sidx >= 0 && sidx < g_nsegs && !p) {
        human_size_iec(b, sizeof b, g_seg_count[sidx] * 512);
        snprintf(a, sizeof a, "%s (%llu sectors), from LBA %llu", b, (unsigned long long)g_seg_count[sidx],
                 (unsigned long long)g_seg_start[sidx]);
        fact(s, x, &y, vx, right, "Free space", a, UTHEME_TEXT);
        const char *why = refusal(CMD_NEW);
        g_form_x = x;
        g_form_y = y + pad;
        if (why) fact(s, x, &y, vx, right, "Note", why, utheme_action(UTHEME_ACT_DANGER));
        else {
            int fy = g_form_y + (g_size.h - lh) / 2;
            ugfx_draw_string(s, x, fy, "New partition", UTHEME_TEXT, UTHEME_WHITE);
        }
        return;
    }
    if (!p) return;
    char t[40];
    udisk_part_name(p, t, sizeof t);
    if (p->number) snprintf(a, sizeof a, "%d of %d, %s", p->number, d->nparts, t);
    else snprintf(a, sizeof a, "The whole disk, no partition table");
    fact(s, x, &y, vx, right, "Partition", a, UTHEME_TEXT);
    int mark = cur_mark();
    if (mark) {
        if (mark & UDISKOP_MARK_NEW)
            snprintf(a, sizeof a, "Created when you apply the changes%s", mark & UDISKOP_MARK_FORMAT ? ", then formatted" : "");
        else
            snprintf(a, sizeof a, "%s%s%s when you apply the changes", mark & UDISKOP_MARK_RESIZED ? "Resized" : "",
                     (mark & UDISKOP_MARK_RESIZED) && (mark & UDISKOP_MARK_FORMAT) ? " and " : "",
                     mark & UDISKOP_MARK_FORMAT ? "formatted" : "");
        fact(s, x, &y, vx, right, "Pending", a, utheme_action(UTHEME_ACT_NAV));
    }
    human_size_iec(b, sizeof b, p->count * 512);
    snprintf(a, sizeof a, "%s (%llu sectors), from LBA %llu", b, (unsigned long long)p->count, (unsigned long long)p->start);
    fact(s, x, &y, vx, right, "Size", a, UTHEME_TEXT);
    if (p->point[0]) snprintf(a, sizeof a, "%s, mounted at %s", p->fs, p->point);
    else if (p->type[0])
        snprintf(a, sizeof a, "%s (not mounted%s)", udisk_fs_title(p->type), p->readable ? "" : "; SaunaOS cannot read it");
    else snprintf(a, sizeof a, "%s", p->role == UDISK_ROLE_BIOS ? "A boot loader, no filesystem" : "Not mounted");
    fact(s, x, &y, vx, right, "Contents", a, UTHEME_TEXT);
    if (p->total) {
        human_size_iec(b, sizeof b, p->used);
        human_size_iec(c, sizeof c, p->total);
        char f[24];
        human_size_iec(f, sizeof f, p->total - p->used);
        snprintf(a, sizeof a, "%s of %s, %s free", b, c, f);
        fact(s, x, &y, vx, right, "Used", a, UTHEME_TEXT);
    }
    if (d->kind == QUERY_PART_GPT) {
        const char *rn = p->role == UDISK_ROLE_DATA ? "Basic data" : p->role == UDISK_ROLE_ESP ? "EFI System" :
                         p->role == UDISK_ROLE_BIOS ? "BIOS boot" : 0;
        guid_text(p->type_guid, b, sizeof b);
        if (mark & UDISKOP_MARK_NEW) b[0] = 0;   // its GUIDs are written when the step runs
        snprintf(a, sizeof a, "%s%s%s", rn ? rn : "", rn && b[0] ? "  " : "", b);
        fact(s, x, &y, vx, right, "Type", a, UTHEME_TEXT);
    } else if (d->kind == QUERY_PART_MBR) {
        snprintf(a, sizeof a, "0x%02x", p->mbr_type);
        fact(s, x, &y, vx, right, "Type", a, UTHEME_TEXT);
    }
    if (p->dev[0]) fact(s, x, &y, vx, right, "Device", p->dev, UTHEME_TEXT);
    if (p->root) fact(s, x, &y, vx, right, "Note", "The system runs from this volume: it cannot be unmounted or formatted.",
                      utheme_action(UTHEME_ACT_DANGER));
    else if (d->system) fact(s, x, &y, vx, right, "Note", "On the system disk: Disks only checks it.",
                             utheme_action(UTHEME_ACT_DANGER));
}

// --- layout -------------------------------------------------------------------

static struct uui_menu_item file_items[] = {
    UUI_MENU("Refresh", CMD_REFRESH, "F5"),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static struct uui_menu_item edit_items[] = {
    UUI_MENU("Undo", CMD_UNDO, "Ctrl+Z"),
    UUI_MENU("Clear all changes", CMD_CLEAR, 0),
    UUI_MENU_SEP,
    UUI_MENU("Apply changes...", CMD_APPLY, 0),
};
static struct uui_menu_item part_items[] = {
    UUI_MENU("New partition", CMD_NEW, "Ctrl+N"),
    UUI_MENU("Format...", CMD_FORMAT, 0),
    UUI_MENU("Check", CMD_CHECK, 0),
    UUI_MENU("Mount or unmount", CMD_MOUNT, 0),
    UUI_MENU("Resize...", CMD_RESIZE, 0),
    UUI_MENU_SEP,
    UUI_MENU("Delete...", CMD_DELETE, "Del"),
};
static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Edit", edit_items),
    UUI_SUBMENU("Partition", part_items),
};

static struct uui_toolbar_item tb_items[] = {   // not const: Mount's label follows the selection
    { "tb-refresh", "Refresh (F5)",                CMD_REFRESH, 0,               0, "F5",  UTHEME_ACT_VIEW },
    UUI_TOOLBAR_SEP,
    { "tb-new",     "New partition in free space", CMD_NEW,     "New partition", 0, 0,     UTHEME_ACT_CREATE },
    { "tb-format",  "Format the partition",        CMD_FORMAT,  "Format...",     0, 0,     UTHEME_ACT_EDIT },
    { "tb-check",   "Check the filesystem",        CMD_CHECK,   "Check",         0, 0,     UTHEME_ACT_ARRANGE },
    { "tb-eject",   "Mount or unmount",            CMD_MOUNT,   "Mount",         0, 0,     UTHEME_ACT_NAV },
    { "tb-fit",     "Grow or shrink the partition", CMD_RESIZE, "Resize...",     0, 0,     UTHEME_ACT_EDIT },
    { "tb-delete",  "Delete the partition (Del)",  CMD_DELETE,  0,               0, "Del", UTHEME_ACT_DANGER },
    UUI_TOOLBAR_SEP,
    { "tb-undo",    "Undo the last change (Ctrl+Z)", CMD_UNDO,  "Undo",          0, "Ctrl+Z", UTHEME_ACT_NAV },
    { "tb-save",    "Apply the pending changes",   CMD_APPLY,   "Apply",         0, 0,     UTHEME_ACT_CREATE },
};
#define TB_MOUNT 5
#define TB_APPLY 10
static char g_apply_label[32];

static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,   .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,     .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_grid_ops,      .widget = &g_drives, .id = ID_DRIVES,  .name = "drives" },
    { .ops = &uui_segbar_ops,    .widget = &g_map,    .id = ID_MAP,     .name = "map" },
    { .ops = &uui_sizefield_ops, .widget = &g_size,   .id = ID_SIZE,    .name = "size",   .hidden = 1 },
    { .ops = &uui_textbox_ops,   .widget = &g_name,   .id = ID_NAME,    .name = "name",   .hidden = 1 },
    { .ops = &uui_segmented_ops, .widget = &g_fs,     .id = ID_FS,      .name = "fs",     .hidden = 1 },
    { .ops = &uui_button_ops,    .widget = &g_create, .id = ID_CREATE,  .name = "create", .hidden = 1 },
    { .ops = &uui_table_ops,     .widget = &g_pending.table, .id = ID_PENDING, .name = "pending", .hidden = 1 },
    { .ops = &uui_button_ops,    .widget = &g_clear,  .id = ID_CLEAR,   .name = "clearall", .hidden = 1 },
    { .ops = &uui_statusbar_ops, .widget = &g_sb,     .id = ID_STATUS,  .name = "status" },
    { .ops = &uui_dialog_ops,    .widget = &g_ask,    .id = ID_DIALOG,  .name = "dialog" },
};
#define W_FORM 4   // ..7
#define W_PENDING 8
#define W_CLEAR 9
#define PENDING_ROWS 4   // shown before the list scrolls
static int g_panel_y;    // the pending panel's top, 0 when there is none

static void layout(int cw, int ch) {
    int pad = ugfx_char_w(), lh = ugfx_char_h(), bh = lh + pad;
    int mh = uui_menubar_height(&g_menu);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mh);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    int th = uui_toolbar_height(&g_tb);
    uui_toolbar_ops.set_geometry(&g_tb, 0, mh, cw, th);
    int sbh = uui_statusbar_height(&g_sb);
    uui_statusbar_set_geometry(&g_sb, 0, ch - sbh, cw, sbh);
    int top = mh + th + 1, listw = pad * 18;
    g_drives.cell_w = listw - pad;
    g_drives.cell_h = lh * 2 + pad * 2;
    uui_grid_ops.set_geometry(&g_drives, 0, top, listw, ch - sbh - top);
    g_pane_x = listw + 1;
    g_pane_y = top;
    struct udisk *d = cur_disk();
    int x = g_pane_x + pad * 2, right = cw - pad * 2;
    // Under the drive's heading and its two or three facts.
    int my = top + pad * 2 + lh + pad + (lh + 4) * (d && d->kind == QUERY_PART_GPT ? 2 : 1) + pad / 2 + lh + 4;
    uui_segbar_ops.set_geometry(&g_map, x, my, right - x, d ? lh * 3 + pad : 0);
    // The pending changes, along the foot of the pane: a heading row with
    // Clear all, then the steps.
    int panel = g_q.count > 0;
    g_widgets[W_PENDING].hidden = g_widgets[W_CLEAR].hidden = !panel;
    g_panel_y = 0;
    if (panel) {
        int rows = g_q.count < PENDING_ROWS ? g_q.count : PENDING_ROWS;
        int lh2 = uui_steplist_height(&g_pending, rows), head = bh + pad;
        g_panel_y = ch - sbh - lh2 - head - pad;
        int cw2 = ugfx_text_width("Clear all") + pad * 4;
        uui_button_set_geometry(&g_clear, cw - pad - cw2, g_panel_y + (head - bh) / 2, cw2, bh);
        uui_table_ops.set_geometry(&g_pending.table, g_pane_x + pad, g_panel_y + head, cw - g_pane_x - pad * 2, lh2);
    }
    g_widgets[3].hidden = !d;
    int form = d && cur_is_free() && !refusal(CMD_NEW);
    for (int i = 0; i < 4; i++) g_widgets[W_FORM + i].hidden = !form;
    if (form) {
        // The facts' first row is the free space; the form is the next.
        int fy = g_map.y + g_map.h + pad * 2 + lh + 4 + pad;
        int fx = x + ugfx_text_width("New partition") + pad * 2;
        int zw, zh;
        uui_sizefield_natural_size(&g_size, &zw, &zh);
        uui_sizefield_set_geometry(&g_size, fx, fy, zw, bh);
        int nx = fx + zw + pad * 2;
        uui_textbox_set_geometry(&g_name, nx, fy, pad * 14, bh);
        int sw, sh;
        uui_segmented_ops.natural_size(&g_fs, &sw, &sh);
        uui_segmented_ops.set_geometry(&g_fs, fx, fy + bh + pad, sw, sh);
        int b = ugfx_text_width("Create") + pad * 4;
        uui_button_set_geometry(&g_create, fx, fy + bh + pad + sh + pad, b, bh);
    }
}

static void status(void) {
    struct udisk_part *p = cur_part();
    char t[32];
    if (p) {
        udisk_part_title(p, t, sizeof t);
        snprintf(g_st_sel, sizeof g_st_sel, "%s selected", t);
    } else {
        snprintf(g_st_sel, sizeof g_st_sel, "%s", cur_is_free() ? "Free space selected" : "");
    }
    uint64_t free = 0;
    for (int i = 0; i < g_scan.ndisks; i++)
        for (int k = 0; k < g_scan.disks[i].nparts; k++)
            if (g_scan.disks[i].parts[k].total) free += g_scan.disks[i].parts[k].total - g_scan.disks[i].parts[k].used;
    char f[24];
    human_size_iec(f, sizeof f, free);
    snprintf(g_st_free, sizeof g_st_free, "%s free in all", f);
}

static void on_draw(struct uapp *a, struct uapp_draw *dr) {
    struct ugfx_surface *s = dr->surface;
    layout(s->w, s->h);
    status();
    // The Mount button says which way it goes.
    struct udisk_part *p = cur_part();
    tb_items[TB_MOUNT].label = p && p->point[0] ? "Unmount" : "Mount";
    if (g_q.count) snprintf(g_apply_label, sizeof g_apply_label, g_q.count == 1 ? "Apply %d change" : "Apply %d changes",
                            g_q.count);
    tb_items[TB_APPLY].label = g_q.count ? g_apply_label : "Apply";
    uapp_log_layout(a, "disks");
    uapp_logf_layout("disks: layout state %d %d %d %d\n", g_cur, g_map.selected, g_nsegs, p ? p->number : -1);
    // The Resize dialog's body is a layout, which describes nothing of
    // its own: its parts by name, so a test drives and measures them.
    if (rz_is_open()) {
        int hx, hy, hw, hh;
        uui_resizebar_handle(&g_rz_bar, &hx, &hy, &hw, &hh);
        uapp_logf_layout("disks: layout rzbox %d %d %d %d\n", g_ask.x, g_ask.y, g_ask.w, g_ask.h);
        uapp_logf_layout("disks: layout rzbar %d %d %d %d\n", g_rz_bar.x, g_rz_bar.y, g_rz_bar.w, g_rz_bar.h);
        uapp_logf_layout("disks: layout rzhandle %d %d %d %d\n", hx, hy, hw, hh);
        uapp_logf_layout("disks: layout rzcaption %d %d %d %d\n", g_rz_cap.x, g_rz_cap.y, g_rz_cap.w, g_rz_cap.h);
        uapp_logf_layout("disks: layout rzlsize %d %d %d %d\n", g_rz_lsize.x, g_rz_lsize.y, g_rz_lsize.w, g_rz_lsize.h);
        uapp_logf_layout("disks: layout rzsize %d %d %d %d\n", g_rz_size.x, g_rz_size.y, g_rz_size.w, g_rz_size.h);
        uapp_logf_layout("disks: layout rzsizenum %d %d %d %d\n", g_rz_size.num.x, g_rz_size.num.y, g_rz_size.num.w,
                         g_rz_size.num.h);
        uapp_logf_layout("disks: layout rzlafter %d %d %d %d\n", g_rz_lafter.x, g_rz_lafter.y, g_rz_lafter.w, g_rz_lafter.h);
        uapp_logf_layout("disks: layout rzafter %d %d %d %d\n", g_rz_after.x, g_rz_after.y, g_rz_after.w, g_rz_after.h);
        uapp_logf_layout("disks: layout rzmib %llu\n", (unsigned long long)uui_sizefield_mib(&g_rz_size));
        uapp_logf_layout("disks: layout rzaftermib %llu\n", (unsigned long long)uui_sizefield_mib(&g_rz_after));
        uapp_logf_layout("disks: layout rzend %llu\n", (unsigned long long)((g_rz_bar.end - g_rz.base) / MIB));
    }
    ugfx_fill_rect(s, g_pane_x, g_pane_y, s->w - g_pane_x, g_sb.y - g_pane_y, UTHEME_WHITE);
    ugfx_fill_rect(s, g_pane_x - 1, g_pane_y, 1, g_sb.y - g_pane_y, UTHEME_SEPARATOR);
    ugfx_fill_rect(s, 0, g_pane_y - 1, s->w, 1, UTHEME_SEPARATOR);
    draw_details(s, s->w);
    if (g_panel_y) {
        int pad = ugfx_char_w(), y = g_panel_y;
        ugfx_fill_rect(s, g_pane_x, y, s->w - g_pane_x, g_sb.y - y, UTHEME_PANEL_BG);
        ugfx_fill_rect(s, g_pane_x, y, s->w - g_pane_x, 1, UTHEME_SEPARATOR);
        int ty = g_clear.y + (g_clear.h - ugfx_char_h()) / 2, tx = g_pane_x + pad * 2;
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, tx, ty, g_clear.x - tx, g_pending_head, UTHEME_TEXT, UTHEME_PANEL_BG);
        int hw = ugfx_text_width(g_pending_head) + pad * 2;
        ugfx_set_font(was);
        if (tx + hw < g_clear.x)
            ugfx_draw_string_clipped(s, tx + hw, ty, g_clear.x - tx - hw - pad, "nothing is written until you apply",
                                     uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), UTHEME_PANEL_BG);
    }
}

// --- input --------------------------------------------------------------------

// Free space opens with the whole gap offered, and nothing larger.
static void offer_whole_gap(void) {
    uint64_t mib = g_seg_count[g_map.selected] / MIB;
    uui_sizefield_init(&g_size, mib, 1, mib ? mib : 1);
}

static void select_drive(int i) {
    if (i == g_cur) return;
    g_cur = i;
    g_map.selected = 0;
    build_map();
    if (cur_is_free()) offer_whole_gap();
}

static void selection_changed(void) {
    if (cur_is_free()) offer_whole_gap();
}

static void on_widget(struct uapp *a, int id, int reason) {
    int code;
    switch (id) {
    case ID_MENU:    if ((code = uui_menubar_take_code(&g_menu)) > 0) command(a, code); break;
    case ID_TOOLBAR: if ((code = uui_toolbar_take_code(&g_tb)) > 0) command(a, code); break;
    case ID_DRIVES:  if (g_drives.selected >= 0) select_drive(g_drives.selected); break;
    case ID_MAP:     selection_changed(); break;
    case ID_PENDING: {
        int row = uui_steplist_take_remove(&g_pending);
        if (row >= 0 && row < g_q.count && !check_running()) {
            char what[sizeof g_q.ops[0].what + 8];
            snprintf(what, sizeof what, "%s %s", udiskop_verb(g_q.ops[row].kind), g_q.ops[row].what);
            int more = udiskops_remove(&g_q, &g_scan, row);
            steps_sync();
            ulogf("disks: removed step %d %s (+%d)\n", row + 1, what, more);
            say("Removed: %s%s", what, more ? ", and the steps that needed it" : "");
            build_map();
        }
        break;
    }
    case ID_DIALOG:
        if ((code = uui_dialog_take_code(&g_ask)) >= 0) answer(a, code);
        else if (rz_is_open()) rz_follow();   // a key typed into a field, or the bar's arrows
        break;
    case ID_RZ_BAR: case ID_RZ_SIZE: case ID_RZ_AFTER:
        if (reason == UUI_REASON_PRESS)
            uui_dialog_focus(&g_ask, id == ID_RZ_BAR ? &g_rz_col_it[0] : id == ID_RZ_SIZE ? &g_rz_size_it[1]
                                                                                      : &g_rz_after_it[1]);
        if (rz_is_open()) rz_follow();
        break;
    default: break;
    }
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    if (code == ID_CREATE && !refusal(CMD_NEW)) { do_create(); build_map(); }
    if (code == ID_CLEAR) command(a, CMD_CLEAR);
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
    case KEY_F5:          command(a, CMD_REFRESH); break;
    case 0x0E:            command(a, CMD_NEW); break;    // Ctrl+N
    case 0x1A:            command(a, CMD_UNDO); break;   // Ctrl+Z
    case KEY_DELETE:      command(a, CMD_DELETE); break;
    default: break;
    }
    uapp_redraw(a);
}

// `disks <point>` opens on the volume mounted there, the File Manager's
// "Open in Disks"; `disks --check <point>` also starts its read-only
// Check, once the window is up (on the first tick).
static int g_check_on_open;

static void select_point(const char *point) {
    for (int i = 0; i < g_scan.ndisks; i++)
        for (int j = 0; j < g_scan.disks[i].nparts; j++) {
            if (strcmp(g_scan.disks[i].parts[j].point, point)) continue;
            select_drive(i);
            for (int k = 0; k < g_nsegs; k++)
                if (g_seg_part[k] == j) g_map.selected = k;
            selection_changed();
            return;
        }
    for (int i = 0; i < g_scan.nram; i++)
        if (!strcmp(g_scan.ram[i].point, point)) { select_drive(g_scan.ndisks + i); return; }
    say("%s is not a volume Disks shows%s", point, "");
    g_check_on_open = 0;
}

static int on_tick(struct uapp *a) {
    if (g_check_on_open) {
        g_check_on_open = 0;
        const char *why = refusal(CMD_CHECK);
        if (why) say("%s%s", why, "");
        else do_check(a, 0);
        uapp_redraw(a);
    }
    int r = uui_toolbar_tick(&g_tb) | check_tick(a);
    if (r) uapp_redraw(a);
    return r;
}

// The worker's answer. A dialog already up is held to SHOW_FOR_MS, at
// 100%, before the result replaces it (check_tick() finishes the job).
static int on_user(struct uapp *a, int a0, int a1) {
    if (a0 == EV_STEP || a0 == EV_STEP_DONE) { apply_step(a0, a1); return 1; }
    if (a0 != EV_CHECK_DONE) return 0;
    g_job.finished = 1;
    g_job.ms = (unsigned)((uui_anim_now_ns() - g_job.started_ns) / 1000000ull);
    if (g_job.kind == JOB_APPLY) {
        // nothing to settle: each step's row already says how it ended
    } else if (g_job.shown && g_job.rc == 0) {
        unsigned bad = fs_check_problems(&g_check);
        uui_stages_set(&g_stages, g_stages.count, 1000);
        g_stages.line[0] = 0;
        if (g_stages.count)
            snprintf(g_stages.detail[g_stages.count - 1], sizeof g_stages.detail[0],
                     bad == 1 ? "%u problem" : "%u problems", bad);
        open_progress(a);    // without its Stop: there is nothing left to stop
    }
    if (!g_job.shown || uui_anim_now_ns() - g_job.shown_ns >= SHOW_FOR_MS * 1000000ull) check_done(a);
    build_map();
    return 1;
}

static void default_size(int *w, int *h) {
    *w = ugfx_char_w() * 76;
    *h = ugfx_char_h() * 36;
}

int main(int argc, char **argv) {
    if (!ugfx_font_init()) return 2;
    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = item_flags;
    g_tb.overflow = 1;    // a narrow window folds the right-hand buttons into a menu
    uui_grid_init(&g_drives, ugfx_char_w() * 16, ugfx_char_h() * 3);
    g_drives.bg = UUI_ROLE(panel_bg);
    g_drives.cell = drive_card;
    uui_segbar_init(&g_map);
    uui_sizefield_init(&g_size, 1, 1, 1);
    uui_textbox_init(&g_name, "");
    g_name.placeholder = "Name (optional)";
    uui_segmented_init(&g_fs, FS_CHOICES, 3, 0);
    uui_button_init(&g_create, 0, 0, 0, 0, "Create", UUI_ROLE(accent), UUI_ROLE(accent_text), ID_CREATE);
    uui_dialog_init(&g_ask);
    uui_stages_init(&g_stages);
    uui_steplist_init(&g_pending, UUI_STEPLIST_REMOVE);
    uui_steplist_init(&g_run, UUI_STEPLIST_STATE);
    uui_button_init(&g_clear, 0, 0, 0, 0, "Clear all", UUI_ROLE(control_bg), UUI_ROLE(text), ID_CLEAR);
    uui_statusbar_init(&g_sb);
    g_sb.count = 4;
    g_sb.panes[0].text = g_note;     g_sb.panes[0].chars = 0;
    g_sb.panes[1].text = g_st_count; g_sb.panes[1].chars = 22;
    g_sb.panes[2].text = g_st_sel;   g_sb.panes[2].chars = 22;
    g_sb.panes[3].text = g_st_free;  g_sb.panes[3].chars = 18;
    g_ring[RING_DRIVES] = (struct uui_focusable){ &g_drives, &uui_grid_ops };
    g_ring[RING_MAP] = (struct uui_focusable){ &g_map, &uui_segbar_ops };
    g_ring[RING_SIZE] = (struct uui_focusable){ &g_size, &uui_sizefield_ops };
    g_ring[RING_NAME] = (struct uui_focusable){ &g_name, &uui_textbox_focus_ops };
    uui_focus_init(&g_focus, g_ring, 4);
    scan();
    g_cur = -1;
    select_drive(0);
    if (argc >= 3 && !strcmp(argv[1], "--check")) { g_check_on_open = 1; select_point(argv[2]); }
    else if (argc >= 2) select_point(argv[1]);

    struct uapp_desc desc = {
        .title        = "Disks",
        .app_id       = "disks",
        .on_size      = default_size,
        .flags        = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE,
        .min_w        = 560,
        .min_h        = 380,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .focus        = &g_focus,
        .tick_ms      = 100,
        .on_tick      = on_tick,
        .on_user      = on_user,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
    };
    return uapp_run(&desc);
}
