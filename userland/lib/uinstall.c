// See uinstall.h.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "rt/sys.h"
#include "ulistdir.h"
#include "errno.h"          // EEXIST -- an existing directory is not a failure
#include "partition_abi.h"
#include "mount_abi.h"
#include "query_abi.h"
#include "lib/human.h"
#include "lib/ubootcfg.h"   // ubootcfg_onoff_in(), the target's menu timeout
#include "lib/ufbs.h"       // the target starts unmarked, with what the session answered
#include "lib/ufile.h"
#include "lib/ufileop.h"
#include "lib/uwalk.h"
#include "lib/dirsort.h"
#include "lib/udirs.h"
#include "lib/uinstall.h"

#define SECTOR_BYTES      512
#define BIOS_BOOT_SECTORS 2048     // 1 MiB, and where GRUB's core.img goes
#define FIRST_LBA         2048     // the 1 MiB alignment every modern tool uses
#define GPT_TAIL          33       // the backup header and entry array

// WHAT IS NOT COPIED. `/boot` is a mount point -- for the source's ESP on
// a disk boot, and for nothing at all on a live one -- and the target's
// is written from the payload instead; `/mnt` is where the target itself
// is mounted, so copying it would copy the target into itself; `/tmp` is
// scratch by definition. Each is recreated empty on the target, because
// a mount point has to exist.
static const char *const SKIP[] = { "boot", "mnt", "tmp" };

static const char *const STAGE_NAMES[UINSTALL_STAGES] = {
    "Partition", "Format", "Mount", "Copy files", "Bootloader",
};

const char *uinstall_stage_name(int stage) {
    return stage >= 0 && stage < UINSTALL_STAGES ? STAGE_NAMES[stage] : "Done";
}

int uinstall_boot_word(void) {
    static char line[1024];
    return ubootcfg_cmdline(line, sizeof line) < 0 ? 0 : ubootcfg_onoff_in(line, "installer=");
}

static int is_skipped(const char *name) {
    for (unsigned i = 0; i < sizeof SKIP / sizeof SKIP[0]; i++)
        if (strcmp(name, SKIP[i]) == 0) return 1;
    return 0;
}

// EVERY OTHER MOUNTED FILESYSTEM STAYS BEHIND, wherever it is mounted --
// the copy is of the system, not of what is plugged into it (`cp -x`).
// SKIP above names three places; a USB stick auto-mounts under /media
// and an SMB share wherever /etc/mounts.d says, and the first install
// after automount copied a whole stick onto the target. Read fresh per
// install: a stick plugged in between two runs is a mount the second
// must see.
static char (*g_mounts)[64];
static int g_nmounts;

static void load_mounts(void) {
    free(g_mounts);
    g_mounts = 0;
    g_nmounts = 0;
    struct query_fsinfo fs;
    for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
        if (!(fs.flags & QUERY_FS_MOUNTED) || strcmp(fs.point, "/") == 0) continue;
        char (*m)[64] = realloc(g_mounts, (size_t)(g_nmounts + 1) * sizeof *g_mounts);
        if (!m) return;
        g_mounts = m;
        snprintf(g_mounts[g_nmounts++], sizeof *g_mounts, "%s", fs.point);
    }
}

static int is_mount_point(const char *path) {
    for (int i = 0; i < g_nmounts; i++)
        if (strcmp(path, g_mounts[i]) == 0) return 1;
    return 0;
}

static int enter_dir(void *ctx, const char *src) {
    (void)ctx;
    return !is_mount_point(src);
}

// --- the plan ---------------------------------------------------------

int uinstall_root_disk(char *out, int cap) {
    struct query_blkdev b;
    QUERY_FOREACH(QUERY_BLKDEV, b, i) {
        if (!b.is_root) continue;
        snprintf(out, (size_t)cap, "%s", b.parent[0] ? b.parent : b.name);
        return 1;
    }
    return 0;
}

// The two TOP-LEVEL entries and the count, by one walk per directory.
struct measure { uint32_t files; uint64_t bytes; };

static int measure_one(void *ctx, const char *path, const struct sys_dirent *e) {
    struct measure *m = ctx;
    if (!e->is_dir) { m->files++; m->bytes += e->size; }
    return e->is_dir && is_mount_point(path) ? 0 : 1;
}

static struct uwalk g_walk;            // several KB: never on a 2 KiB frame
static struct sys_dirent *g_top;   // the last list_root() listing of "/"

// "/" WHOLE, replacing the last listing.
static int list_root(void) {
    free(g_top);
    g_top = 0;
    return ulistdir("/", &g_top);
}

int uinstall_measure(uint32_t *files, uint64_t *bytes) {
    struct measure m = { 0, 0 };
    int n = list_root();
    if (n < 0) return -1;
    load_mounts();
    for (int i = 0; i < n; i++) {
        if (is_skipped(g_top[i].name)) continue;
        char path[80];
        snprintf(path, sizeof path, "/%s", g_top[i].name);
        if (is_mount_point(path)) continue;
        if (!g_top[i].is_dir) { m.files++; m.bytes += g_top[i].size; continue; }
        if (!uwalk_begin(&g_walk, path)) continue;
        while (uwalk_step(&g_walk, measure_one, &m)) {}
    }
    *files = m.files;
    *bytes = m.bytes;
    return 0;
}

uint64_t uinstall_root_need(uint64_t bytes) {
    return bytes + bytes / 4 + 8ull * 1024 * 1024;
}

// The root's size under each layout, in sectors; 0 when the layout does
// not fit at all.
static uint64_t root_sectors(uint64_t sectors, uint64_t esp_mib, int mbr) {
    uint64_t esp = esp_mib * 1024 * 1024 / SECTOR_BYTES;
    uint64_t fixed = mbr ? FIRST_LBA + esp : FIRST_LBA + BIOS_BOOT_SECTORS + esp + GPT_TAIL;
    return sectors > fixed ? sectors - fixed : 0;
}

const char *uinstall_refusal(const struct udisk *d, uint64_t esp_mib, int mbr, uint64_t need) {
    static char why[96];
    char mine[16] = "";
    uinstall_root_disk(mine, sizeof mine);
    if (d->system || (mine[0] && !strcmp(mine, d->name))) return "This system is running from it";
    if (!d->persistent) return "It is in memory, and gone at power off";
    // What this installs is BIOS-booted GRUB, whose boot sector and
    // core.img blocklist are 512-byte formats -- the kernel refuses
    // SYS_INSTALL_BOOT on anything else.
    if (d->block_size != SECTOR_BYTES) {
        snprintf(why, sizeof why, "Its sectors are %llu bytes; a BIOS boot needs 512",
                 (unsigned long long)d->block_size);
        return why;
    }
    uint64_t root = root_sectors(d->sectors, esp_mib, mbr);
    if (!root) return "Too small for the boot partitions";
    if (need && root * SECTOR_BYTES < need) {
        char n[16];
        human_size_iec(n, sizeof n, need + esp_mib * 1024 * 1024 + 2ull * 1024 * 1024);
        snprintf(why, sizeof why, "Too small -- SaunaOS needs %s", n);
        return why;
    }
    return 0;
}

const char *uinstall_plan(struct uinstall_plan *p, struct udisk_scan *s, const char *disk,
                          uint64_t esp_mib, int mbr, uint64_t need) {
    memset(p, 0, sizeof *p);
    struct udisk *d = udisk_named(s, disk);
    if (!d) return "There is no such disk (a WHOLE disk, as `lsblk` names it)";
    if (esp_mib < UINSTALL_ESP_MIN) return "The boot partition must be at least 8 MiB";
    const char *why = uinstall_refusal(d, esp_mib, mbr, need);
    if (why) return why;

    snprintf(p->disk, sizeof p->disk, "%s", disk);
    p->sectors = d->sectors;
    p->esp_mib = esp_mib;
    p->mbr = mbr;
    uint64_t esp = esp_mib * 1024 * 1024 / SECTOR_BYTES;
    uint64_t at = FIRST_LBA;
    int n = 0;
    // MBR has no BIOS-boot type: core.img lives in the gap below
    // FIRST_LBA, which is why the boot partition is p1 there.
    if (!mbr) {
        p->part[n] = (struct uinstall_part){ at, BIOS_BOOT_SECTORS, MKPART_ROLE_BIOS_BOOT, "", 0,
                                             "GRUB's core image" };
        at += BIOS_BOOT_SECTORS;
        n++;
    }
    p->boot = n;
    p->part[n++] = (struct uinstall_part){ at, esp, MKPART_ROLE_ESP, "", "fat32", "/boot" };
    at += esp;
    p->root = n;
    p->part[n++] = (struct uinstall_part){ at, root_sectors(d->sectors, esp_mib, mbr),
                                           MKPART_ROLE_DATA, "", "tfs3", "the root" };
    p->nparts = n;
    for (int i = 0; i < n; i++)
        snprintf(p->part[i].dev, sizeof p->part[i].dev, "%sp%d", disk, i + 1);
    return 0;
}

// Do the staged core images read the flat layout -- kernel.bin and grub/
// at the boot volume's root? A core image whose prefix does not match the
// files on the disk is a machine at a GRUB rescue prompt, so a payload
// that cannot say (one staged before core.prefix existed) is refused.
static int payload_flat(void) {
    char buf[128] = "";
    int fd = open(UINSTALL_PAYLOAD "/core.prefix", O_RDONLY);
    long n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (n > 0) buf[n] = 0;
    return n > 0 && strstr(buf, ")/grub\n") && !strstr(buf, "/boot/grub");
}

const char *uinstall_payload_problem(int mbr) {
    static char why[160];
    const char *const NEED[] = {
        UINSTALL_PAYLOAD "/kernel.bin", UINSTALL_PAYLOAD "/grub.cfg",
        UINSTALL_PAYLOAD "/boot.img",
        // Checked BEFORE the disk is erased, and it is the image this
        // run will actually use: a build staged without the MBR one
        // must refuse MBR rather than partition and then discover it.
        mbr ? UINSTALL_PAYLOAD "/core-msdos.img" : UINSTALL_PAYLOAD "/core.img",
    };
    struct sys_stat st;
    for (unsigned i = 0; i < sizeof NEED / sizeof NEED[0]; i++) {
        if (sys_stat(NEED[i], &st) < 0 || st.is_dir || st.size == 0) {
            snprintf(why, sizeof why, "%s is missing -- this system carries no install payload "
                     "and cannot install itself", NEED[i]);
            return why;
        }
    }
    if (!payload_flat())
        return UINSTALL_PAYLOAD "/core.prefix does not say the staged GRUB reads /boot/grub -- "
               "refusing to write a bootloader that may not find its files";
    return 0;
}

// --- the run ------------------------------------------------------------

struct run {
    const struct uinstall_plan *p;
    struct uinstall_progress *prog;
    const struct uinstall_ops *ops;
    uint64_t file_bytes;           // the current file's size, once known
    char last[96];
};

static void line(struct run *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(struct run *r, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (r->ops && r->ops->on_line) r->ops->on_line(r->ops->ctx, buf);
}

static int fail(struct run *r, const char *what, int err) {
    snprintf(r->prog->error, sizeof r->prog->error, "%s: %s", what, sys_strerror(err));
    r->prog->errors++;
    line(r, "%s", r->prog->error);
    return 0;
}

static void stage(struct run *r, int s, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void stage(struct run *r, int s, const char *fmt, ...) {
    char detail[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    // The callback first: a reader polling `stage` finds the detail there.
    if (r->ops && r->ops->on_stage) r->ops->on_stage(r->ops->ctx, s, detail);
    r->prog->stage = s;
}

static int do_partition(struct run *r) {
    const struct uinstall_plan *p = r->p;
    struct mkpart_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.device, sizeof req.device, "%s", p->disk);
    req.flags = MKPART_CONFIRM;
    req.kind = p->mbr ? MKPART_KIND_MBR : MKPART_KIND_GPT;
    req.count = (uint32_t)p->nparts;
    static const char *const GPT_NAMES[] = { "BIOS boot", "EFI System", "saunaos" };
    static const char *const MBR_NAMES[] = { "boot", "saunaos" };
    for (int i = 0; i < p->nparts; i++) {
        req.entries[i].start_lba = p->part[i].start;
        req.entries[i].sectors = p->part[i].sectors;
        // THE BOOT PARTITION KEEPS THE ESP ROLE, and on MBR therefore type
        // 0xEF -- which is also what lets a UEFI boot BOOTX64.EFI off an MBR
        // disk. The type is what
        // `partition_is_firmware()` reads to keep it OUT of the root scan,
        // and typing it 0x0C made the boot scan mount /boot as the root --
        // measured. It is also what marks it ACTIVE.
        req.entries[i].role = p->part[i].role;
        snprintf(req.entries[i].name, sizeof req.entries[i].name, "%s",
                 p->mbr ? MBR_NAMES[i] : GPT_NAMES[i]);
    }
    if (sys_mkpart(&req) < 0) return fail(r, "partitioning", sys_errno());
    return 1;
}

static int format_one(struct run *r, const char *part, const char *type) {
    struct mkfs_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.source, sizeof req.source, "%s", part);
    snprintf(req.fstype, sizeof req.fstype, "%s", type);
    req.flags = MKFS_CONFIRM;
    if (sys_mkfs(&req) < 0) return fail(r, part, sys_errno());
    return 1;
}

static int mount_one(struct run *r, const char *part, const char *point) {
    struct mount_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.source, sizeof req.source, "%s", part);
    snprintf(req.point, sizeof req.point, "%s", point);
    if (sys_mount(&req) < 0) return fail(r, point, sys_errno());
    return 1;
}

// An error in one file does not abandon the other thousand -- ufileop's
// contract, and coreutils'. Each is reported, and the run still fails.
static void copy_error(void *ctx, const char *path, int err) {
    fail(ctx, path, err);
}

// Per buffer, so it only records: a new path is a new file.
static int copy_progress(void *ctx, const char *path, uint64_t done, uint64_t total) {
    struct run *r = ctx;
    struct uinstall_progress *g = r->prog;
    if (strcmp(path, r->last) != 0) {
        g->bytes_done += r->file_bytes;
        r->file_bytes = 0;
        snprintf(r->last, sizeof r->last, "%s", path);
        snprintf(g->path, sizeof g->path, "%s", path);
        if (g->files_done < g->files_total) g->files_done++;
    }
    // `total` is this file's; the running sum moves with it.
    if (total > r->file_bytes) r->file_bytes = total;
    (void)done;
    return 1;
}

static struct ufileop g_op;   // ~30 KB, like every ufileop caller's

// Everything at the root except the three mount points, then those three
// recreated empty. A directory at a time rather than one copy of "/",
// because "/" contains the target.
static int copy_system(struct run *r) {
    const struct ufileop_policy pol = {
        .ctx = r, .on_conflict = 0, .on_progress = copy_progress, .on_error = copy_error,
        .enter_dir = enter_dir,
    };
    int n = list_root();
    if (n < 0) return fail(r, "/", sys_errno());
    load_mounts();
    // Sorted so the progress is the same on two machines, which is what
    // makes a transcript of a failed install comparable.
    dirsort(g_top, n, DIRSORT_NAME, 0);
    int ok = 1;
    for (int i = 0; i < n; i++) {
        if (is_skipped(g_top[i].name)) continue;
        char src[80], dst[80];
        snprintf(src, sizeof src, "/%s", g_top[i].name);
        snprintf(dst, sizeof dst, UINSTALL_TARGET "/%s", g_top[i].name);
        if (is_mount_point(src)) { mkdir(dst, 0755); continue; }
        line(r, "%s", src);
        if (ufileop_copy(src, dst, &g_op, &pol) != UFILEOP_OK) ok = 0;
    }
    // An empty file reports no progress at all, so the counts are made
    // whole here rather than left a few short.
    r->prog->files_done = r->prog->files_total;
    r->prog->bytes_done = r->prog->bytes_total;
    r->file_bytes = 0;
    for (unsigned i = 0; i < sizeof SKIP / sizeof SKIP[0]; i++) {
        char dst[64];
        snprintf(dst, sizeof dst, UINSTALL_TARGET "/%s", SKIP[i]);
        mkdir(dst, 0755);   // already there is not an error
    }
    return ok;
}

// AN INSTALLED SYSTEM BOOTS WITH A MENU. A default build's grub.cfg has
// `set timeout=0`, and `update` installs no kernel without a menu to
// pick the previous one from (docs/commands/update.md) -- so a copy left
// that way could never be updated. A file that already shows a menu, or
// whose timeout this does not understand, is left as it is.
#define MENU_SECONDS 3
static void give_menu(struct run *r) {
    const char *path = UINSTALL_TARGET_BOOT "/grub/grub.cfg";
    const char *why = 0;
    struct ubootcfg *c = malloc(sizeof *c);
    if (!c) why = "out of memory";
    else if (ubootcfg_load(c, path) < 0) why = "it could not be read";
    else if (c->timeout == 0) {
        if (ubootcfg_set_timeout(c, MENU_SECONDS) < 0) why = c->why;
        else if (ubootcfg_save(c, path, &why) == 0) {
            // The .bak is the menu-less file; `bootcfg undo` must not bring it back.
            unlink(UINSTALL_TARGET_BOOT "/grub/grub.cfg.bak");
            line(r, "boot menu: %d seconds", MENU_SECONDS);
        }
    }
    free(c);
    if (why)
        line(r, "could not give %s a boot menu (%s) -- `update` will refuse until "
                "`bootcfg timeout %d`", path, why, MENU_SECONDS);
}

// The target's ESP: the kernel and grub.cfg, at the paths GRUB's prefix
// names -- the volume's root, which the target shows as /boot. Written
// from the payload rather than copied from `/boot`, which a live boot
// does not have.
//
// GRUB's MODULE DIRECTORY IS NOT COPIED: `core.img` already carries every
// module grub.cfg's `insmod` asks for. What the target cannot do is have
// a HOST `grub-install` run against it later without re-copying them.
static int copy_boot(struct run *r) {
    const struct ufileop_policy pol = { .ctx = r, .on_error = copy_error };
    if (mkdir(UINSTALL_TARGET_BOOT "/grub", 0755) < 0 && sys_errno() != EEXIST)
        return fail(r, UINSTALL_TARGET_BOOT "/grub", sys_errno());
    static const char *const FILES[][2] = {
        { UINSTALL_PAYLOAD "/kernel.bin", UINSTALL_TARGET_BOOT "/kernel.bin" },
        { UINSTALL_PAYLOAD "/grub.cfg",   UINSTALL_TARGET_BOOT "/grub/grub.cfg" },
    };
    for (unsigned i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
        line(r, "%s", FILES[i][0]);
        if (ufileop_copy(FILES[i][0], FILES[i][1], &g_op, &pol) != UFILEOP_OK) return 0;
    }
    const char *why = 0;
    int efi = uinstall_write_efi(UINSTALL_TARGET_BOOT, &why);
    if (efi < 0) { line(r, "%s", why); return 0; }
    line(r, efi ? "no UEFI loader in " UINSTALL_PAYLOAD " -- this disk boots BIOS/CSM only"
                : UINSTALL_PAYLOAD "/bootx64.efi -> EFI/BOOT/BOOTX64.EFI");
    give_menu(r);
    return 1;
}

int uinstall_write_efi(const char *boot, const char **why) {
    uint8_t *img = 0;
    size_t size = 0;
    enum ufile_result r = ufile_slurp(UINSTALL_PAYLOAD "/bootx64.efi", 0, &img, &size);
    if (r == UFILE_NOENT) return 1;
    if (r != UFILE_OK) { *why = UINSTALL_PAYLOAD "/bootx64.efi could not be read"; return -1; }
    char path[96];
    static const char *const DIRS[] = { "/EFI", "/EFI/BOOT" };
    for (unsigned i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++) {
        snprintf(path, sizeof path, "%s%s", boot, DIRS[i]);
        if (mkdir(path, 0755) < 0 && sys_errno() != EEXIST) {
            free(img);
            *why = "could not make EFI/BOOT on the boot volume";
            return -1;
        }
    }
    snprintf(path, sizeof path, "%s/EFI/BOOT/BOOTX64.EFI", boot);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t done = 0;
    while (fd >= 0 && done < size) {
        long n = write(fd, img + done, size - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    if (fd >= 0) close(fd);
    free(img);
    if (done != size) { *why = "could not write EFI/BOOT/BOOTX64.EFI"; return -1; }
    return 0;
}

// A whole file into a fresh buffer; the caller frees it. boot.img is 512
// bytes and core.img tens of KiB -- neither belongs on a 2 KiB frame.
static void *slurp(const char *path, uint64_t *out_size) {
    uint8_t *buf = 0;
    size_t len = 0;
    if (ufile_slurp(path, 0, &buf, &len) != UFILE_OK) return 0;
    *out_size = len;
    return buf;
}

const char *uinstall_write_bootloader(const char *disk, int mbr, int in_use) {
    static char why[96];
    struct install_boot_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.device, sizeof req.device, "%s", disk);
    if (in_use) req.flags = INSTALL_BOOT_CONFIRM;

    uint64_t boot_size = 0, core_size = 0;
    void *boot = slurp(UINSTALL_PAYLOAD "/boot.img", &boot_size);
    if (!boot) return UINSTALL_PAYLOAD "/boot.img could not be read";
    // The prefix baked into a core image names the table format AND the
    // partition number, so the layout decides which staged image can
    // find its grub.cfg.
    const char *core_path = mbr ? UINSTALL_PAYLOAD "/core-msdos.img" : UINSTALL_PAYLOAD "/core.img";
    void *core = slurp(core_path, &core_size);
    if (!core) {
        free(boot);
        snprintf(why, sizeof why, "%s could not be read", core_path);
        return why;
    }
    req.boot_img = (uint64_t)(uintptr_t)boot;
    req.boot_size = boot_size;
    req.core_img = (uint64_t)(uintptr_t)core;
    req.core_size = core_size;
    int rc = sys_install_boot(&req);
    int err = sys_errno();
    free(boot);
    free(core);
    if (rc < 0) {
        snprintf(why, sizeof why, "%s: %s", disk, sys_strerror(err));
        return why;
    }
    return 0;
}

// A PREVIOUS INSTALL'S MOUNTS, left by one that was stopped (Ctrl-C, a
// kill, a crash -- no handler of its own could unmount under its open
// files): /mnt/boot and /mnt still holding partitions of THIS disk, which
// the repartition would find busy. Unmounted first, the child before its
// parent. A mount there from any other disk is left alone. How many went.
static int drop_stale_mounts(struct run *r) {
    int dropped = 0;
    static const char *const AT[] = { UINSTALL_TARGET_BOOT, UINSTALL_TARGET };
    size_t dl = strlen(r->p->disk);
    for (int k = 0; k < 2; k++) {
        struct query_fsinfo fs;
        for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
            if (!(fs.flags & QUERY_FS_MOUNTED) || strcmp(fs.point, AT[k]) != 0) continue;
            if (strncmp(fs.device, r->p->disk, dl) != 0 || fs.device[dl] != 'p') break;
            if (sys_umount(AT[k]) == 0) {
                line(r, "unmounted %s from %s, left there by an install that was stopped", fs.device, AT[k]);
                dropped++;
            }
            break;
        }
    }
    return dropped;
}

static int run(struct run *r, unsigned flags) {
    const struct uinstall_plan *p = r->p;
    struct uinstall_progress *g = r->prog;
    const char *boot = p->part[p->boot].dev, *root = p->part[p->root].dev;
    char size[16];

    int stale = drop_stale_mounts(r);

    human_size_iec(size, sizeof size, p->part[p->root].sectors * SECTOR_BYTES);
    stage(r, UINSTALL_PARTITION, "%s, %d partitions, root %s%s", p->mbr ? "MBR" : "GPT", p->nparts, size,
          stale ? "; unmounted what a stopped install left" : "");
    if (!do_partition(r)) return 0;

    stage(r, UINSTALL_FORMAT, "FAT32 on %s, TFS3 on %s", boot, root);
    if (!format_one(r, boot, "fat32") || !format_one(r, root, "tfs3")) return 0;

    stage(r, UINSTALL_MOUNT, "%s at " UINSTALL_TARGET ", %s at " UINSTALL_TARGET_BOOT, root, boot);
    if (!mount_one(r, root, UINSTALL_TARGET)) return 0;
    // The mount point has to exist on the volume it is mounted onto,
    // which is the target's root -- so it is made after that is mounted.
    mkdir(UINSTALL_TARGET_BOOT, 0755);
    if (!mount_one(r, boot, UINSTALL_TARGET_BOOT)) { sys_umount(UINSTALL_TARGET); return 0; }

    char total[16];
    human_size_iec(total, sizeof total, g->bytes_total);
    stage(r, UINSTALL_COPY, "%u files, %s", (unsigned)g->files_total, total);
    int ok = copy_system(r);
    // A NEW MACHINE IS SET UP BY WHOEVER BOOTS IT: the copied marker
    // would hide First-boot setup there. What this session answered
    // travels with it, so setup asks only the rest.
    unlink(UINSTALL_TARGET UFBS_MARKER);
    unlink(UINSTALL_TARGET UFBS_ANSWERED);
    // Nor does the live session's "Install SaunaOS" launcher go with it.
    char launcher[128];
    snprintf(launcher, sizeof launcher, UINSTALL_TARGET "%s/" UINSTALL_LAUNCHER, udir_path(UDIR_DESKTOP));
    unlink(launcher);
    if (ok && !(flags & UINSTALL_ASK_ALL) &&
        ufbs_write_answered(UINSTALL_TARGET, ufbs_stored()) != 0)
        line(r, "could not write " UINSTALL_TARGET UFBS_ANSWERED " -- setup will ask every page");
    if (ok) ok = copy_boot(r);

    // UNMOUNT BEFORE THE BOOTLOADER, for two reasons: the ESP's
    // write-back cache is flushed by the unmount, so the kernel the disk
    // is about to boot is actually on it; and the kernel refuses a boot
    // sector on a disk something is mounted from without a confirm flag,
    // which is the right refusal to be subject to.
    sys_umount(UINSTALL_TARGET_BOOT);
    sys_umount(UINSTALL_TARGET);
    if (!ok) return 0;

    stage(r, UINSTALL_BOOTLOADER, "the boot sector and %s",
          p->mbr ? UINSTALL_PAYLOAD "/core-msdos.img" : UINSTALL_PAYLOAD "/core.img");
    const char *why = uinstall_write_bootloader(p->disk, p->mbr, 0);
    if (why) {
        snprintf(g->error, sizeof g->error, "%s", why);
        g->errors++;
        line(r, "%s", why);
        return 0;
    }
    return 1;
}

int uinstall_run(const struct uinstall_plan *p, unsigned flags, struct uinstall_progress *prog,
                 const struct uinstall_ops *ops) {
    static struct run r;
    memset(&r, 0, sizeof r);
    r.p = p;
    r.prog = prog;
    r.ops = ops;
    prog->finished = prog->ok = 0;
    prog->errors = 0;
    prog->error[0] = prog->path[0] = 0;
    prog->files_done = 0;
    prog->bytes_done = 0;
    uint32_t files = 0;
    uint64_t bytes = 0;
    if (uinstall_measure(&files, &bytes) == 0) {
        prog->files_total = files;
        prog->bytes_total = bytes;
    }
    int ok = run(&r, flags);
    prog->stage = UINSTALL_STAGES;
    prog->ok = ok;
    prog->finished = 1;
    return ok ? 0 : -1;
}
