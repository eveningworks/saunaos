// Disks -- every drive, its partitions as a map, and what is on them:
// GNOME Disks' shape (chosen from mockups on 2026-10-09, D1). A list of
// drives, and for the one selected its facts, a MAP of its partitions
// and free space (ui/uui_segbar.h), and the selected volume's facts.
//
// It CHANGES disks: a new partition (an inline form on free space),
// Format, Check, Mount/Unmount and Delete, each destructive one behind
// a confirm. THE SYSTEM DISK IS REFUSED for anything but Check and its
// facts -- its table only takes effect at the next boot, and a format
// there is the running system. A new partition also needs nothing
// mounted from the disk, since it rewrites the whole table (SYS_MKPART);
// Delete edits one entry (SYS_PARTEDIT) and needs only that partition
// unmounted. Everything goes through the same calls `mkpart`, `part`,
// `mkfs`, `fsck` and `mount` make.
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
#include "ui/uui_segmented.h"
#include "ui/uui_sizefield.h"
#include "ui/uui_stages.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_textbox.h"
#include "ui/uui_toolbar.h"
#include "keyboard.h"

#define MIB       UDISK_MIB
#define GPT_TAIL  UDISK_GPT_TAIL

static struct udisk_scan g_scan;   // ~10 KB: never on the stack
static int g_cur;                  // the drives list's row: a disk, then the RAM volumes

// The map of the current drive: a segment per partition and per gap.
static struct uui_segbar_seg g_segs[UUI_SEGBAR_MAX];
static int g_seg_part[UUI_SEGBAR_MAX];       // a part index, -1 for free space
static uint64_t g_seg_start[UUI_SEGBAR_MAX], g_seg_count[UUI_SEGBAR_MAX];
static char g_seg_title[UUI_SEGBAR_MAX][64], g_seg_corner[UUI_SEGBAR_MAX][24], g_seg_detail[UUI_SEGBAR_MAX][96];
static int g_nsegs;

enum {
    ID_MENU = 1, ID_TOOLBAR, ID_DRIVES, ID_MAP, ID_SIZE, ID_NAME, ID_FS, ID_CREATE, ID_STATUS, ID_DIALOG,
};
enum {
    CMD_REFRESH = 1, CMD_NEW, CMD_FORMAT, CMD_CHECK, CMD_MOUNT, CMD_DELETE, CMD_EXIT,
    // dialog answers
    ASK_CANCEL = 100, ASK_FORMAT_TFS3, ASK_FORMAT_FAT, ASK_DELETE, ASK_CREATE, ASK_REPAIR, ASK_OK,
    ASK_STOP, ASK_BUSY,
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
}

static struct udisk *cur_disk(void) { return g_cur < g_scan.ndisks ? &g_scan.disks[g_cur] : 0; }
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

static int disk_busy(const struct udisk *d) {
    for (int i = 0; i < d->nparts; i++) if (d->parts[i].point[0]) return 1;
    return 0;
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
        snprintf(g_seg_corner[i], sizeof g_seg_corner[i], "%s", p->point);
        if (p->total) s->used_pct = (int)(p->used * 100 / p->total);
        // Colour by what it IS: the firmware's in amber, data in the accent's tint.
        uui_diskplan_colour(s, p->role, p->point[0] != 0);
        s->corner = g_seg_corner[i][0] ? g_seg_corner[i] : 0;
    }
    s->title = g_seg_title[i];
    s->detail = g_seg_detail[i];
}

// Partitions in disk order, and every gap of a megabyte or more between
// the table's first usable sector and its last as free space.
static void build_map(void) {
    g_nsegs = 0;
    struct udisk *d = cur_disk();
    if (!d) { uui_segbar_set(&g_map, g_segs, 0); return; }
    udisk_sort(d);
    uint64_t at = MIB, end = d->kind == QUERY_PART_GPT ? d->sectors - GPT_TAIL : d->sectors;
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

static const char *refusal(int cmd) {
    struct udisk *d = cur_disk();
    struct udisk_part *p = cur_part();
    // Refresh too: reading a volume's usage waits behind the check's lock.
    if (check_running() && cmd != CMD_EXIT) return job_wait_note();
    if (!d) return "Volumes in memory cannot be changed";
    switch (cmd) {
    case CMD_NEW:
        if (d->system) return "The system disk's partitions cannot be changed while it runs";
        if (!d->persistent) return "This disk is in memory";
        if (disk_busy(d)) return "Unmount this disk's volumes first";
        for (int i = 0; i < d->nparts; i++)
            if (d->parts[i].role == UDISK_ROLE_OTHER)
                return "A partition here has a type Disks cannot keep -- use mkpart";
        if (!cur_is_free()) return "Select free space";
        if (d->nparts >= MKPART_MAX_ENTRIES) return "A table here holds at most four partitions";
        return 0;
    // ONE ENTRY EDITED IN PLACE (SYS_PARTEDIT), so the rest of the table
    // -- any type, any number of entries, a mounted sibling -- is no
    // obstacle; only the partition itself must be free.
    case CMD_DELETE:
        if (d->system) return "The system disk's partitions cannot be changed while it runs";
        if (!d->persistent) return "This disk is in memory";
        if (!p || !p->number) return "Select a partition";
        if (p->point[0]) return "Unmount it first";
        if (!p->dev[0]) return "This partition has no device until the next boot";
        return 0;
    case CMD_FORMAT:
        if (!p || !p->number) return "Select a partition";
        if (d->system) return "The system disk cannot be formatted while it runs";
        if (p->point[0]) return "Unmount it first";
        if (p->role != UDISK_ROLE_DATA) return "This partition holds the firmware's boot data";
        if (!p->dev[0]) return "This partition has no device until the next boot";
        return 0;
    case CMD_CHECK:
        if (!p || !p->point[0]) return "Check needs a mounted volume";
        return 0;
    case CMD_MOUNT:
        if (!p) return "Select a partition";
        if (d->system) return "The system disk's volumes stay as they are";
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

// The table as it would be with `add` placed: every other entry as it
// is, in disk order, its role and MBR type kept.
static int write_table(struct udisk *d, const struct mkpart_entry *add) {
    struct mkpart_request req;
    memset(&req, 0, sizeof req);
    req.kind = d->kind == QUERY_PART_MBR ? MKPART_KIND_MBR : MKPART_KIND_GPT;
    snprintf(req.device, sizeof req.device, "%s", d->name);
    for (int i = 0; i < d->nparts; i++) {
        const struct udisk_part *p = &d->parts[i];
        if (!p->number) continue;
        if (add && req.count < MKPART_MAX_ENTRIES && add->start_lba < p->start) req.entries[req.count++] = *add, add = 0;
        if (req.count >= MKPART_MAX_ENTRIES) return -EINVAL;
        struct mkpart_entry *e = &req.entries[req.count++];
        e->start_lba = p->start;
        e->sectors = p->count;
        e->mbr_type = (uint8_t)p->mbr_type;
        e->role = p->role == UDISK_ROLE_ESP ? MKPART_ROLE_ESP : p->role == UDISK_ROLE_BIOS ? MKPART_ROLE_BIOS_BOOT : MKPART_ROLE_DATA;
        snprintf(e->name, sizeof e->name, "%s", p->name);
    }
    if (add) {
        if (req.count >= MKPART_MAX_ENTRIES) return -EINVAL;
        req.entries[req.count++] = *add;
    }
    int rc = sys_mkpart(&req) < 0 ? -errno : 0;
    ulogf("disks: mkpart %s %u entries -> %d\n", d->name, req.count, rc);
    return rc;
}

static int format(const char *dev, const char *fs) {
    struct mkfs_request m;
    memset(&m, 0, sizeof m);
    snprintf(m.source, sizeof m.source, "%s", dev);
    snprintf(m.fstype, sizeof m.fstype, "%s", fs);
    m.flags = MKFS_CONFIRM;
    int rc = sys_mkfs(&m) < 0 ? -errno : 0;
    ulogf("disks: mkfs %s %s -> %d\n", dev, fs, rc);
    return rc;
}

static void start_format(struct uapp *a, const char *dev, const char *fs);

static void do_create(struct uapp *a) {
    struct udisk *d = cur_disk();
    int s = g_map.selected;
    if (refusal(CMD_NEW) || !d) return;
    uint64_t spb = d->block_size / 512 ? d->block_size / 512 : 1;
    uui_sizefield_commit(&g_size);
    uint64_t room = g_seg_count[s], mb = uui_sizefield_mib(&g_size);
    uint64_t want = mb ? mb * MIB : room;
    if (want > room) want = room;
    want -= want % spb;
    struct mkpart_entry e;
    memset(&e, 0, sizeof e);
    e.start_lba = (g_seg_start[s] + MIB - 1) / MIB * MIB;   // aligned to a megabyte
    if (e.start_lba + want > g_seg_start[s] + room) want = g_seg_start[s] + room - e.start_lba;
    e.sectors = want;
    e.role = MKPART_ROLE_DATA;
    snprintf(e.name, sizeof e.name, "%s", uui_textbox_text(&g_name));
    if (e.sectors < MIB) { say("Too small: %s%s", "a partition needs at least 1 MiB", ""); return; }
    int rc = write_table(d, &e);
    if (rc < 0) { say("Could not write the table: %s%s", strerror(-rc), ""); return; }
    char name[16];
    snprintf(name, sizeof name, "%s", d->name);
    scan();
    d = udisk_named(&g_scan, name);
    const char *dev = "";
    for (int i = 0; d && i < d->nparts; i++)
        if (d->parts[i].start == e.start_lba) dev = d->parts[i].dev;
    int fs = g_fs.selected;
    if (fs == 2 || !dev[0]) {
        say(dev[0] ? "Created %s%s" : "Created the partition%s%s; it gets a device at the next boot", dev, "");
    } else {
        start_format(a, dev, fs == 0 ? "tfs3" : "fat32");
    }
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

// --- Check, Repair and Format, on a worker ----------------------------------------
//
// SYS_FS_CHECK AND SYS_MKFS BLOCK FOR THE WHOLE PASS, so each runs on a
// worker and this thread reads how far it has got (FSCK_PROGRESS,
// MKFS_PROGRESS, neither taking a lock the pass holds) on every tick. The
// worker touches nothing but the job's answer and posts once.

#define SHOW_AFTER_MS 300   // a check quicker than this never shows a dialog...
#define SHOW_FOR_MS   500   // ...and one that does stays up this long, so it never flashes

enum { EV_CHECK_DONE = 1 };

static struct fs_check_result g_check;
static char g_check_point[64], g_check_title[32], g_job_title[48];
enum { JOB_CHECK, JOB_FORMAT };
static char g_fmt_dev[16], g_fmt_fs[8];
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

static int check_running(void) { return g_job.running; }
static const char *job_wait_note(void) {
    return g_job.kind == JOB_FORMAT ? "Wait for the format to finish" : "Wait for the check to finish";
}

static void *format_worker(void *arg) {
    (void)arg;
    g_job.rc = format(g_fmt_dev, g_fmt_fs);   // -errno already
    g_job.err = g_job.rc < 0 ? -g_job.rc : 0;
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
// repair -- and reopened if anything closes it while the pass runs.
static void open_progress(struct uapp *a) {
    static struct uui_dialog_button stop[1];
    stop[0] = (struct uui_dialog_button){ g_job.stopping ? "Stopping..." : "Stop", ASK_STOP, 0 };
    int n = g_job.repair || g_job.kind == JOB_FORMAT || g_job.stopping || g_job.finished ? 0 : 1;
    int sw, sh;
    uui_stages_natural_size(&g_stages, &sw, &sh);
    uui_dialog_set_body(&g_ask, &g_stages_item, sw, sh);
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_ask, g_job_title, 0, 0, stop, n, n ? 0 : -1, n ? ASK_STOP : ASK_BUSY);
    if (!g_job.shown) g_job.shown_ns = uui_anim_now_ns();
    g_job.shown = 1;
}

// 1 while the kernel reports the pass running -- not g_job.running,
// which stays set until the worker's message is read, after the pass.
static int poll_progress(void) {
    struct fs_check_progress p;
    int rc = g_job.kind == JOB_FORMAT ? sys_mkfs_progress(g_fmt_dev, &p) : sys_fs_check_progress(g_check_point, &p);
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
    if (g_job.kind == JOB_FORMAT) return 1;
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

// A format on the worker, reported in the same stage dialog: everything
// on the partition is replaced, so there is no Stop.
static void start_format(struct uapp *a, const char *dev, const char *fs) {
    g_app = a;
    warm_progress();
    memset(&g_job, 0, sizeof g_job);
    g_job.kind = JOB_FORMAT;
    snprintf(g_fmt_dev, sizeof g_fmt_dev, "%s", dev);
    snprintf(g_fmt_fs, sizeof g_fmt_fs, "%s", fs);
    g_job.started_ns = uui_anim_now_ns();
    snprintf(g_job_title, sizeof g_job_title, "Formatting %s as %s", dev, !strcmp(fs, "fat32") ? "FAT32" : fs);
    uui_stages_set_names(&g_stages, 0, 0);
    uui_stages_set(&g_stages, 0, -1);
    snprintf(g_stages.line, sizeof g_stages.line, "%s", "Starting");
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    g_job.running = 1;
    if (pthread_create(&th, &at, format_worker, 0) != 0) {
        g_job.running = 0;
        say("Could not start formatting %s: %s", dev, "no thread");
        return;
    }
    ulogf("disks: format %s %s started\n", dev, fs);
    say("%s%s", g_job_title, "...");
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
    if (g_job.kind == JOB_FORMAT) {
        const char *fs = !strcmp(g_fmt_fs, "fat32") ? "FAT32" : g_fmt_fs;
        ulogf("disks: format %s %s -> %d in %u ms\n", g_fmt_dev, g_fmt_fs, g_job.rc, ms);
        if (g_job.rc < 0) say("Could not format %s: %s", g_fmt_dev, strerror(g_job.err));
        else say("Formatted %s %s", g_fmt_dev, fs);
        scan();
        return;
    }
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
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "Format %s (%s, %s) on %s?", p->dev, title, size, d->name);
        snprintf(g_ask_line[1], sizeof g_ask_line[1], "Everything on it is erased.");
        btns[0] = (struct uui_dialog_button){ "Format as tfs3", ASK_FORMAT_TFS3, UUI_DLG_DANGER };
        btns[1] = (struct uui_dialog_button){ "Format as FAT32", ASK_FORMAT_FAT, UUI_DLG_DANGER };
        btns[2] = (struct uui_dialog_button){ "Cancel", ASK_CANCEL, 0 };
        ask(a, "Format partition", 2, btns, 3, 2);
        break;
    case CMD_CHECK:   do_check(a, 0); break;
    case CMD_MOUNT:   do_mount(); break;
    case CMD_DELETE:
        human_size_iec(size, sizeof size, p->count * 512);
        udisk_part_title(p, title, sizeof title);
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "Delete %s (%s, %s) from %s?", p->dev[0] ? p->dev : title, title,
                 size, d->name);
        snprintf(g_ask_line[1], sizeof g_ask_line[1], "Its data cannot be recovered with Disks.");
        btns[0] = (struct uui_dialog_button){ "Delete", ASK_DELETE, UUI_DLG_DANGER };
        btns[1] = (struct uui_dialog_button){ "Cancel", ASK_CANCEL, 0 };
        ask(a, "Delete partition", 2, btns, 2, 1);
        break;
    case CMD_EXIT: uapp_quit(a, 0); break;
    default: break;
    }
}

static void answer(struct uapp *a, int code) {
    struct udisk *d = cur_disk();
    struct udisk_part *p = cur_part();
    int rc;
    switch (code) {
    case ASK_FORMAT_TFS3:
    case ASK_FORMAT_FAT:
        if (!p || refusal(CMD_FORMAT)) break;
        start_format(a, p->dev, code == ASK_FORMAT_TFS3 ? "tfs3" : "fat32");
        break;
    case ASK_DELETE:
        if (!p || refusal(CMD_DELETE)) break;
        {
            struct partedit_request req;
            memset(&req, 0, sizeof req);
            req.op = PARTEDIT_DELETE;
            req.flags = PARTEDIT_CONFIRM;
            snprintf(req.device, sizeof req.device, "%s", p->dev);
            rc = sys_partedit(&req) < 0 ? -errno : 0;
            ulogf("disks: partedit delete %s -> %d %s\n", req.device, rc, req.why);
            if (rc < 0) say("Could not delete it: %s%s", req.why[0] ? req.why : strerror(-rc), "");
            else say("Deleted %s from %s", req.device, d->name);
        }
        scan();
        break;
    case ASK_CREATE: do_create(a); break;
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
        snprintf(line2, sizeof line2, "%s  %s%s", size,
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
        if (d->kind == QUERY_PART_GPT) {
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
        snprintf(a, sizeof a, "%s%s%s", rn ? rn : "", rn ? "  " : "", b);
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
static struct uui_menu_item part_items[] = {
    UUI_MENU("New partition", CMD_NEW, "Ctrl+N"),
    UUI_MENU("Format...", CMD_FORMAT, 0),
    UUI_MENU("Check", CMD_CHECK, 0),
    UUI_MENU("Mount or unmount", CMD_MOUNT, 0),
    UUI_MENU_SEP,
    UUI_MENU("Delete...", CMD_DELETE, "Del"),
};
static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Partition", part_items),
};

static struct uui_toolbar_item tb_items[] = {   // not const: Mount's label follows the selection
    { "tb-refresh", "Refresh (F5)",                CMD_REFRESH, 0,               0, "F5",  UTHEME_ACT_VIEW },
    UUI_TOOLBAR_SEP,
    { "tb-new",     "New partition in free space", CMD_NEW,     "New partition", 0, 0,     UTHEME_ACT_CREATE },
    { "tb-format",  "Format the partition",        CMD_FORMAT,  "Format...",     0, 0,     UTHEME_ACT_EDIT },
    { "tb-check",   "Check the filesystem",        CMD_CHECK,   "Check",         0, 0,     UTHEME_ACT_ARRANGE },
    { "tb-eject",   "Mount or unmount",            CMD_MOUNT,   "Mount",         0, 0,     UTHEME_ACT_NAV },
    { "tb-delete",  "Delete the partition (Del)",  CMD_DELETE,  0,               0, "Del", UTHEME_ACT_DANGER },
};
#define TB_MOUNT 5

static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,   .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,     .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_grid_ops,      .widget = &g_drives, .id = ID_DRIVES,  .name = "drives" },
    { .ops = &uui_segbar_ops,    .widget = &g_map,    .id = ID_MAP,     .name = "map" },
    { .ops = &uui_sizefield_ops, .widget = &g_size,   .id = ID_SIZE,    .name = "size",   .hidden = 1 },
    { .ops = &uui_textbox_ops,   .widget = &g_name,   .id = ID_NAME,    .name = "name",   .hidden = 1 },
    { .ops = &uui_segmented_ops, .widget = &g_fs,     .id = ID_FS,      .name = "fs",     .hidden = 1 },
    { .ops = &uui_button_ops,    .widget = &g_create, .id = ID_CREATE,  .name = "create", .hidden = 1 },
    { .ops = &uui_statusbar_ops, .widget = &g_sb,     .id = ID_STATUS,  .name = "status" },
    { .ops = &uui_dialog_ops,    .widget = &g_ask,    .id = ID_DIALOG,  .name = "dialog" },
};
#define W_FORM 4   // ..7

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
    uapp_log_layout(a, "disks");
    uapp_logf_layout("disks: layout state %d %d %d %d\n", g_cur, g_map.selected, g_nsegs, p ? p->number : -1);
    ugfx_fill_rect(s, g_pane_x, g_pane_y, s->w - g_pane_x, g_sb.y - g_pane_y, UTHEME_WHITE);
    ugfx_fill_rect(s, g_pane_x - 1, g_pane_y, 1, g_sb.y - g_pane_y, UTHEME_SEPARATOR);
    ugfx_fill_rect(s, 0, g_pane_y - 1, s->w, 1, UTHEME_SEPARATOR);
    draw_details(s, s->w);
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
    (void)reason;
    int code;
    switch (id) {
    case ID_MENU:    if ((code = uui_menubar_take_code(&g_menu)) > 0) command(a, code); break;
    case ID_TOOLBAR: if ((code = uui_toolbar_take_code(&g_tb)) > 0) command(a, code); break;
    case ID_DRIVES:  if (g_drives.selected >= 0) select_drive(g_drives.selected); break;
    case ID_MAP:     selection_changed(); break;
    case ID_DIALOG:  if ((code = uui_dialog_take_code(&g_ask)) >= 0) answer(a, code); break;
    default: break;
    }
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    if (code == ID_CREATE && !refusal(CMD_NEW)) {
        struct udisk *d = cur_disk();
        uui_sizefield_commit(&g_size);
        char mb[24];
        human_size_iec(mb, sizeof mb, uui_sizefield_mib(&g_size) << 20);
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "Create a %s partition on %s%s%s?", mb,
                 d->name, g_fs.selected == 2 ? "" : ", formatted ", g_fs.selected == 2 ? "" : FS_CHOICES[g_fs.selected]);
        snprintf(g_ask_line[1], sizeof g_ask_line[1], "%s", d->kind ? "The other partitions are kept as they are."
                                                                    : "The disk gets a new GPT partition table.");
        static struct uui_dialog_button btns[2];
        btns[0] = (struct uui_dialog_button){ "Create", ASK_CREATE, UUI_DLG_PRIMARY };
        btns[1] = (struct uui_dialog_button){ "Cancel", ASK_CANCEL, 0 };
        ask(a, "New partition", 2, btns, 2, 0);
    }
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
    (void)a1;
    if (a0 != EV_CHECK_DONE) return 0;
    g_job.finished = 1;
    g_job.ms = (unsigned)((uui_anim_now_ns() - g_job.started_ns) / 1000000ull);
    if (g_job.shown && g_job.rc == 0 && g_job.kind == JOB_FORMAT) {
        uui_stages_set(&g_stages, g_stages.count, 1000);
        g_stages.line[0] = 0;
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
    *h = ugfx_char_h() * 30;
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
