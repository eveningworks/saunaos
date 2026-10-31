// Implements fs.h's public API (kapi.h's stable filesystem surface --
// unchanged by this file's existence, and by design) by resolving every
// path to a MOUNT and forwarding the call to that mount's backend.
//
// This file used to hold the backend table and the boot policy as
// well. Those moved to kernel/fs/mount.c when `/boot` became a second
// filesystem -- see kernel/mount.h for the mount table's five rules and
// for what Linux and Windows do. The split is by concern: this half is
// "given a path, who answers and what do they get handed", and that
// half is "what is mounted where, and what is allowed to be".
//
// WHAT A BACKEND IS HANDED. Not the path the caller passed: the path
// with its mount point stripped, so a mount at /boot sees /grub/x for
// /boot/grub/x and / for /boot itself. A backend never learns where it
// is mounted, which is what lets one driver serve the root and a
// partition at the same time.
//
// TWO REFUSALS LIVE HERE and both are Unix's. A mutating call on a
// READ-ONLY mount fails rather than being quietly dropped, and an
// operation naming TWO paths (rename, link) that land on different
// mounts is refused rather than half-done -- EXDEV, and the reason
// `cp` exists.
#include "fs.h"
#include "fs_ops.h"
#include "errno.h"   // fs_chmod returns a negative errno
#include "mmap.h"   // imgcache_forget: a rewritten /lib file must not serve stale pages
#include "mount.h"
#include "tmppath.h"
#include "block.h"
#include "heap.h"   // wipe_foreign's zero page
#include "ata.h"
#include "kfmt.h"    // klog_printf
#include "klog.h"
#include "string.h"
#include "scheduler.h"
#include "kmutex.h"  // the step table's lock; the mounts' are in mount.h
#include "fs_ident.h" // what an unmountable volume holds, for a resize refusal
#include "tfs3.h"     // tfs3_threads_start()
#include "fswatch.h" // a mutation tells whoever watches its path
#include "cachesim.h" // what a page cache would have hit, when measuring
#include "initcall.h"
#include "fs_defer.h" // the fd layer's calls, settled at close
#include "timer.h"    // coarse_ticks(): how long a change has gone unsettled

// EVERY backend call runs inside its MOUNT's lock, and a contender SLEEPS.
//
// It was a blanket preemption guard until 2026-09-18, and the reason it
// stopped being one is measured: `tools/latency_under_io.py` put the
// compositor's loaded wake latency at 0.3-0.4 s under the trap gate
// purely because this held preemption off for a whole backend call --
// a process waiting on nothing at all still waited, because the
// machine was not rotating at all. A lock stops only the contexts that
// want the same thing. See docs/blocking-design.md.
//
// The backends are not re-entrant and never were: tfs3.c walks
// directories, inodes and file data through scratch buffers (per mount
// since docs/fslock-design.md's stage 2), and fat32.c walks a FAT chain
// through a per-mount sector buffer. That is fine for a filesystem only one thing
// at a time uses, and this kernel is not that -- the kernel context is
// a scheduler participant and a ring-3 process is preemptible inside a
// syscall, so the WM reading a file and an app reading a file interleave
// at any instruction. The app's read then overwrites the block the WM
// is parsing.
//
// It did not look like a filesystem bug from outside. The WM reported
// files that plainly exist as missing or unreadable, intermittently and
// with no error logged anywhere -- the desktop losing cursor shapes on
// roughly one boot in three under KVM, hidden behind the built-in
// fallback. vfs.c is the one place every caller passes through, so the
// guard goes here rather than being repeated (and eventually forgotten)
// in each backend.
//
// ONE LOCK PER MOUNT (mount.h's struct mount), since stage 2 of
// docs/fslock-design.md: each backend keeps its scratch, caches and
// journal in the mount's own state, so two volumes' calls can run at
// once and a /tmp call does not wait behind a disk wait on /. Inside
// one volume it is still one call at a time -- stages 3-4 go finer.
//
// This is NOT the same thing as the nested-read refusal that guarded
// the old fs_read()'s shared staging buffer -- that call and its buffer
// were deleted on 2026-09-03; this protects every backend's internal
// state for the whole call. Note it does not make a LIST
// CALLBACK safe to call fs_* on the SAME mount from -- that is direct
// recursion, not preemption, and the lock's depth cannot see it.

// **THE MOUNT IS RE-CHECKED ONCE ITS LOCK IS HELD.** It was resolved
// before the lock, and a caller can SLEEP waiting for the lock -- so an
// unmount can complete in between, and a new mount can take the slot.
// `gen` is the resolve-time generation (mount.h); a mismatch fails the
// call with 0, which every backend op already means as failure, and the
// caller sees a path that no longer resolves, which is what it now is.
// `op` is an fs_ops slot and the mount's own state goes in first, so a
// call cannot be handed another mount's state.
//
// **AND IT RUNS THE OP AGAIN WHEN THE OP ASKS.** A backend with its own
// per-object locks (tfs3, fslock stage 4) that finds one busy releases
// what it holds, waits with the volume lock DROPPED (mount_wait()), and
// returns asking to be re-run -- what it looked up may have changed.
// op_end releases whatever the op took, every time, still under the lock.
// A mount gone while the op waited ends the loop WITHOUT calling op_end:
// its state is freed.
static inline int fs_op_again(const struct mount *m, uint32_t gen) {
    if (!(m->used && m->gen == gen)) return 0;
    if (m->fs->op_end) m->fs->op_end(m->state);
    if (!m->restart) return 0;
    ((struct mount *)m)->restart = 0;
    return 1;
}

#define FS_OP(m, g, op, ...) ({                                     \
    const struct mount *_fs_m = (m);                                \
    mount_lock(_fs_m);                                              \
    __typeof__(_fs_m->fs->op(_fs_m->state, ##__VA_ARGS__)) _fs_r = 0; \
    while (_fs_m->used && _fs_m->gen == (g)) {                      \
        _fs_r = _fs_m->fs->op(_fs_m->state, ##__VA_ARGS__);         \
        if (!fs_op_again(_fs_m, (g))) break;                        \
    }                                                               \
    mount_unlock(_fs_m);                                            \
    _fs_r;                                                          \
})

#define FS_OP_VOID(m, g, op, ...) do {                              \
    const struct mount *_fs_m = (m);                                \
    mount_lock(_fs_m);                                              \
    while (_fs_m->used && _fs_m->gen == (g)) {                      \
        _fs_m->fs->op(_fs_m->state, ##__VA_ARGS__);                 \
        if (!fs_op_again(_fs_m, (g))) break;                        \
    }                                                               \
    mount_unlock(_fs_m);                                            \
} while (0)

// ---- resolution -----------------------------------------------------
//
// One buffer per call, on the stack: FS_PATH_MAX is 64, and a static
// one would be exactly the shared-scratch hazard the preemption guard
// above exists to contain.
// `sub` BORROWS from the `path` the caller passed in (mount.h), so a
// struct resolved is two pointers and must not outlive that argument.
// It held an FS_PATH_MAX array until paths grew to 4096, at which point
// one of these per fs_*() was a quarter of the kernel stack.
struct resolved {
    const struct mount *m;
    const char *sub;
    uint32_t gen;            // the mount's, when resolved -- see FS_OP
};

static int resolve(const char *path, struct resolved *r) {
    cachesim_lookup(path);
    r->m = mount_resolve(path, &r->sub);
    if (r->m) r->gen = __atomic_load_n(&r->m->gen, __ATOMIC_ACQUIRE);
    return r->m != NULL;
}

// A mutating call needs a mount that is not read-only. Returns 0 (and
// logs, once per refusal, since a silent no is what a write to /boot
// would otherwise look like) when it is.
static int writable(const struct resolved *r, const char *what, const char *path) {
    if (!(r->m->flags & MNT_RDONLY)) return 1;
    klog_printf(KLOG_ERR "fs: %s refused -- %s is mounted read-only (\"%s\")\n",
                what, r->m->point, path ? path : "");
    return 0;
}

// ---- the API --------------------------------------------------------

static void ensure_layout(void);
static void rename_from_toy_os(void);

void fs_init(void) {
    ata_init(); // the disk comes up once, here -- before any backend is probed
    mount_boot_root();
    ensure_layout();
    // /boot has to EXIST before anything can be mounted over it, which
    // is why this runs after the layout pass and not beside the root
    // mount. Rule 3 in kernel/mount.h.
    mount_boot_auto();
}
INITCALL(fs_init, INIT_FS);

int fs_is_persistent(void) {
    const struct mount *m = mount_root();
    return m ? m->persistent : 0;
}

// THE ROOT'S, not "the filesystem's" -- there are several now. Every
// caller of these predates mounts and means the root: `df`'s summary
// line, the About window, fsck. Per-mount answers come from the
// QUERY_FSINFO provider, which reports one record per mount.
const char *fs_backend_name(void) {
    const struct mount *m = mount_root();
    return (m && m->fs->name) ? m->fs->name : "none";
}

uint32_t fs_capabilities(void) {
    const struct mount *m = mount_root();
    return m ? m->fs->caps : 0;
}

int fs_has(uint32_t cap) {
    return (fs_capabilities() & cap) == cap;
}

// See fs.h. The whole of the difference from fs_format_backend() below
// is that this one disturbs NOTHING: no unmount, no re-probe, no change
// to what is mounted where. A target that would need any of that is
// refused instead, because an installer handed the running root is an
// installer being asked to saw off its own branch.
// FOREIGN SIGNATURES TOO: mount_wipe_others() knows only this kernel's
// backends, and tfs3 never writes its first 32 KiB -- so an NTFS or ext4
// volume formatted tfs3 still read as NTFS to fs_ident() and to Windows.
// The first 128 KiB holds every signature fs_ident.c reads (Btrfs's is the
// deepest, at 64 KiB); what the new format writes there lands after this.
// A PARTITION only: on a whole disk those sectors are the MBR and GPT.
#define WIPE_SECTORS 256u
static void wipe_foreign(const struct block_device *dev) {
    uint64_t base;
    if (!blk_part_parent(dev, &base) || blkdev_sector_count(dev) < WIPE_SECTORS) return;
    uint8_t *zero = kmalloc(4096);
    if (!zero) return;
    k_memset(zero, 0, 4096);
    for (uint64_t lba = 0; lba < WIPE_SECTORS; lba += 8)
        if (!blkdev_write_partial(dev, lba, 8, zero)) break;
    kfree(zero);
}

// One format at a time: the progress record is a single one, and two
// formats would only fight over the same disk's queue anyway.
static struct kmutex g_format_lock;
static struct fs_check_live g_format_live;
// THE VOLUME A FORMAT OR RESIZE HOLDS. The job takes no mount's lock
// (mount_scratch_own()), so what keeps a mount of that volume out is
// this, claimed under fs_exclusive and checked by mount_add() under it.
static const struct block_device *volatile g_format_dev;

int fs_volume_busy(const struct block_device *dev) {
    return dev && __atomic_load_n(&g_format_dev, __ATOMIC_ACQUIRE) == dev;
}

// Claims `dev` for a whole-volume job: 0, with nothing claimed, when it
// is mounted. Under fs_exclusive, so no mount_add() is between its check
// and its record. The caller holds g_format_lock.
static int claim_volume(const struct block_device *dev) {
    fs_exclusive_begin();
    int mounted = 0;
    for (int i = 0; i < mount_count() && !mounted; i++) {
        const struct mount *m = mount_at(i);
        mounted = m && m->used && m->dev == dev;
    }
    if (!mounted) __atomic_store_n(&g_format_dev, dev, __ATOMIC_RELEASE);
    fs_exclusive_end();
    return !mounted;
}

struct fs_check_live *fs_format_live(void) { return g_format_dev ? &g_format_live : 0; }

int fs_format_progress(const struct block_device *dev, struct fs_check_progress *out) {
    if (!dev || g_format_dev != dev || !__atomic_load_n(&g_format_live.prog.running, __ATOMIC_ACQUIRE))
        return -ESRCH;
    k_memcpy(out, &g_format_live.prog, sizeof *out);
    return 0;
}

static int format_device_locked(const struct block_device *dev, const char *fstype);

int fs_format_device(const struct block_device *dev, const char *fstype) {
    kmutex_lock(&g_format_lock);
    int ok = format_device_locked(dev, fstype);
    g_format_dev = 0;
    __atomic_store_n(&g_format_live.prog.running, 0, __ATOMIC_RELEASE);
    kmutex_unlock(&g_format_lock);
    return ok;
}

static int format_device_locked(const struct block_device *dev, const char *fstype) {
    if (!dev || !fstype || !fstype[0]) return 0;
    const struct fs_ops *target = mount_backend_named(fstype);
    if (!target || !target->format) return 0;

    // ONLY THE TARGET ITSELF: format() and wipe() run on a SCRATCH
    // state below, so there is no mounted volume for them to repoint.
    // Before per-mount state they wrote into the one set of globals a
    // mounted root was reading -- two crashes that took /bin with them,
    // one of them with a FAT32 target, because mount_wipe_others() runs
    // every OTHER backend's wipe against the same device.
    if (!claim_volume(dev)) {
        klog_printf(KLOG_ERR "mkfs: %s is mounted -- refused\n", blk_device_name(dev));
        return 0;
    }

    // The wipefs rule, same as below and for the same reason: another
    // backend's leftover signature outlives a format and the next probe
    // mounts the corpse.
    mount_wipe_others(target, dev);
    wipe_foreign(dev);

    struct fs_scratch sc;
    int ok = 0;
    if (mount_scratch_own(target, &sc)) {
        ok = target->format(sc.st, dev) ? 1 : 0;
        mount_scratch_end(&sc);
    }
    return ok;
}

static void resize_why(struct fs_resize_info *out, const char *why) {
    k_strlcpy(out->why, why, sizeof out->why);
}

int fs_resize_device(const struct block_device *dev, uint64_t sectors, int info_only,
                     struct fs_resize_info *out) {
    k_memset(out, 0, sizeof *out);
    if (!dev || !blkdev_sector_count(dev)) { resize_why(out, "no such volume"); return -ENODEV; }
    // The system disk first: its root is mounted too, and "unmount it"
    // would be the wrong advice.
    uint64_t base;
    const struct block_device *disk = blk_part_parent(dev, &base);
    if ((dev == blk_root_disk() || (disk && disk == blk_root_disk())) && fs_is_persistent()) {
        resize_why(out, "the system disk's volumes cannot be resized while it runs");
        return -EBUSY;
    }
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (m && m->used && m->dev == dev) { resize_why(out, "it is mounted -- unmount it first"); return -EBUSY; }
    }

    const struct fs_ops *fs = mount_probe_volume(dev);
    if (!fs) {
        // A filesystem this kernel cannot mount is still somebody's data:
        // moving the table under it would cut it short or leave it small.
        struct fs_ident id;
        if (fs_ident(dev, &id) && id.type[0]) {
            k_strlcpy(out->fstype, id.type, sizeof out->fstype);
            k_snprintf(out->why, sizeof out->why, "SaunaOS cannot resize %s volumes", id.type);
            return -ENOTSUP;
        }
        // Nothing on it: the table alone changes.
        out->flags = FS_RESIZE_CAN_GROW | FS_RESIZE_CAN_SHRINK;
        out->max_bytes = UINT64_MAX;
        return 0;
    }
    k_strlcpy(out->fstype, fs->name, sizeof out->fstype);
    if (!fs->resize) {
        k_snprintf(out->why, sizeof out->why, "%s volumes cannot be resized yet",
                   !k_strcmp(fs->name, "fat32") ? "FAT32" : !k_strcmp(fs->name, "exfat") ? "exFAT" : fs->name);
        return -ENOTSUP;
    }

    // The format's lock and progress record: one whole-volume job at a
    // time, and MKFS_PROGRESS/FSRESIZE_PROGRESS read the same record.
    kmutex_lock(&g_format_lock);
    if (!info_only && !claim_volume(dev)) {
        kmutex_unlock(&g_format_lock);
        resize_why(out, "it is mounted -- unmount it first");
        return -EBUSY;
    }
    struct fs_scratch sc;
    int rc = -ENOMEM;
    if (mount_scratch_own(fs, &sc)) {
        uint64_t want = info_only ? 0 : sectors ? sectors : blkdev_sector_count(dev);
        rc = fs->resize(sc.st, dev, want, out);
        mount_scratch_end(&sc);
    } else {
        resize_why(out, "out of memory");
    }
    k_strlcpy(out->fstype, fs->name, sizeof out->fstype);
    g_format_dev = 0;
    __atomic_store_n(&g_format_live.prog.running, 0, __ATOMIC_RELEASE);
    kmutex_unlock(&g_format_lock);
    if (!info_only)
        klog_printf("fs_resize: %s %s -> %d%s%s\n", blk_device_name(dev), fs->name, rc,
                    out->why[0] ? " -- " : "", out->why);
    return rc;
}

int fs_format_backend(const char *name) {
    const struct fs_ops *target = mount_backend_named(name);
    if (!target) return 0;
    if (!blk_root_present()) return 0;
    const struct block_device *dev = blk_root();

    // Formatting the volume something is mounted from, while it is
    // mounted, is how a live mount ends up describing a filesystem that
    // is no longer there. Everything is dropped and re-probed below,
    // but the ESP at /boot is on a DIFFERENT volume and would survive a
    // root reformat with a stale device -- so it goes first.
    for (int i = mount_count() - 1; i >= 0; i--) {
        const struct mount *m = mount_at(i);
        if (!m || m->point_len == 1) continue;
        const char *why;
        if (!mount_remove(m->point, &why)) {
            klog_printf("fsformat: %s is still mounted (%s)\n", m->point, why);
            return 0;
        }
    }

    // The wipefs rule -- see mount_wipe_others(). Found live: formatting
    // a TFS3 disk as TFS2 left TFS3's backups intact, and the probe
    // mounted the corpse.
    mount_wipe_others(target, dev);

    struct fs_scratch sc;
    if (!mount_scratch_begin(target, &sc)) return 0;
    int formatted = target->format(sc.st, dev);
    mount_scratch_end(&sc);
    if (!formatted) return 0;

    // Remount through the same probe path a boot takes -- the freshly
    // written superblock is what should claim the disk.
    if (!mount_reprobe_root(target)) return 0;
    // Same layout a boot would leave behind. Without this the disk came
    // back with no /etc and no /tmp until the next reboot, so the very
    // next `timezone` or `font` change silently failed to persist.
    ensure_layout();
    mount_boot_auto();
    return 1;
}

// The directories the rest of the OS assumes exist on whatever is
// mounted. Every mkdir is a no-op if the directory is already there, so
// this is safe to call after every mount, which is the point:
//
//   /etc  the config-file convention (see api/etc_config.h). Anything
//         calling etc_config_set() before this has run writes nothing
//         and reports failure -- a persisted timezone or font size
//         silently stops persisting.
//   /tmp  the one directory POSIX actually mandates by name (it says
//         almost nothing else about layout -- see
//         docs/filesystem-layout.md). Scratch space has to exist on any
//         disk, including one this build never seeded, which is why it
//         is created rather than seeded. Deliberately NOT emptied here:
//         fs_delete() refuses non-empty directories on purpose and
//         there is no recursive delete (see docs/decisions.md), so
//         clearing it needs a real directory walk that nothing has
//         needed yet. It is a MOUNT POINT now -- the `tmpfs` service
//         puts a ramfs over it at boot -- so what is created here is
//         what a machine sees only if that service is removed.
//   /run  RUNTIME state -- init's control file and its status, and a
//         service's stop marker. The FHS's directory for exactly this,
//         and what init's own comment asked for while saying "/tmp is
//         the only such directory here". It stopped being a reasonable
//         stand-in the moment /tmp became a mount point a SERVICE
//         mounts: init's own channel cannot live under a filesystem one
//         of init's services puts there.
//   /var/tmp  scratch that must SURVIVE, and must be real storage.
//         Once /tmp is in RAM the two stop being interchangeable, which
//         is exactly the FHS's distinction and Linux's reason for
//         keeping both. Anything that needs a file to still be there
//         after a reboot, or needs the disk to actually be written,
//         belongs here -- six KTESTs were quietly relying on /tmp for
//         the second of those.
//   /boot  THE MOUNT POINT FOR THE ESP, and it has to exist on the ROOT
//         before anything can be mounted over it -- kernel/mount.h's
//         rule 3, which is Linux's. On a machine with no ESP it stays
//         an empty directory, which is the honest picture: this build's
//         kernel came from somewhere else.
//   /mnt  the conventional place to mount something by hand, empty for
//         the same reason.
//
// This lives beside the mount rather than in kernel_main(), where it
// used to be, because a mount is not only a boot-time event: the
// `fsformat` command reformats and remounts a live disk, and that path
// left the directories missing until the next reboot.
static void ensure_layout(void) {
    fs_mkdir("/etc");
    // THE COMPILED DEFAULTS, not the settings. This pass runs before
    // /etc can be read -- it is what creates /etc -- so it cannot ask
    // where scratch is configured to be. storage_config_init() makes
    // the configured ones once it has read them (INIT_CONFIG, after
    // INIT_FS), which is the second half of the same job.
    fs_mkdir(TMP_DIR_DEFAULT);
    fs_mkdir("/var");
    fs_mkdir(TMP_VARDIR_DEFAULT);
    fs_mkdir("/var/log");
    fs_mkdir(TMP_RUNDIR);
    fs_mkdir("/boot");
    fs_mkdir("/mnt");
    fs_mkdir("/media");   // removable disks, mounted by automountd (FHS's place for them)
    // Where a config FILE declares itself -- one descriptor per file,
    // read by config_files_scan() (api/config_file.h). It has to exist
    // before anything can register, and `config register` on a machine
    // whose /etc/config.d is missing would fail for a reason the user
    // could do nothing about.
    fs_mkdir("/etc/config.d");
    rename_from_toy_os();
}

// THE FILES NAMED AFTER THE OS, from before it was SaunaOS (2026-10-27).
// Moved here because the kernel reads /etc/saunaos.conf at INIT_CONFIG,
// before any service could do it. Only where the new name is free: a
// machine that has both keeps both, and nothing is lost. Remove once no
// machine can still have the old names.
static void rename_from_toy_os(void) {
    static const char *const moves[][2] = {
        { "/etc/toyos.conf",     "/etc/saunaos.conf" },
        { "/var/log/toyos.log",   "/var/log/saunaos.log" },
        { "/var/log/toyos.log.1", "/var/log/saunaos.log.1" },
    };
    for (unsigned i = 0; i < sizeof moves / sizeof moves[0]; i++) {
        if (!fs_exists(moves[i][0]) || fs_exists(moves[i][1])) continue;
        if (fs_rename(moves[i][0], moves[i][1]))
            klog_printf("vfs: moved %s to %s\n", moves[i][0], moves[i][1]);
        else
            klog_printf(KLOG_WARN "vfs: could not move %s to %s\n", moves[i][0], moves[i][1]);
    }
}

// Bumped by every mutation below, on SUCCESS only -- a refused write
// changed nothing, and waking a watcher for it would make the counter
// mean "someone tried" rather than "something changed". See
// fs_generation() in api/fs.h for what reads this and why it is global.
static uint64_t g_generation;

uint64_t fs_generation(void) { return g_generation; }

// PER-DIRECTORY GENERATIONS (api/fs.h, fs_generation_of()). Counters
// indexed by the path hashes fswatch already computes, bumped for the
// changed path AND its parent -- so one moves when a thing or a direct
// child of it changes. NO REGISTRY: a collision only makes an unrelated
// change look like one, the false positive the global counter gives on
// every change.
#define DIRGEN_BUCKETS 256
static uint64_t g_dirgen[DIRGEN_BUCKETS];

static void dirgen_note(uint64_t self, uint64_t parent) {
    g_dirgen[self % DIRGEN_BUCKETS]++;
    g_dirgen[parent % DIRGEN_BUCKETS]++;
}

uint64_t fs_generation_of(const char *path) {
    uint64_t h;
    fswatch_hash(path, &h, 0);
    // + the mount epoch: a mount over a path changes what it names.
    return g_dirgen[h % DIRGEN_BUCKETS] + mount_epoch() + 1;   // never 0: "not sampled yet"
}

// Everything buffered anywhere on the way to a platter, on EVERY
// mounted volume. Two stages, because they are two different places
// data can be sitting and only one of them used to be emptied:
//
//   1. a driver's software write-back cache (only ATA has one), which
//      holds sectors in RAM;
//   2. the DRIVE's own volatile cache, which a device flush empties.
//
// `sys_sync()` did stage 1 alone and skipped both when the ATA cache
// was absent -- so on AHCI or virtio-blk it asked the disk for nothing
// and reported success. Backend- and driver-agnostic here: the block
// layer knows how to flush whatever is under each mount, and the
// filesystem on top of it never comes into it.
//
// `*wrote_out` is sectors moved out of a SOFTWARE cache, which is 0 on
// a machine that has none and is not a measure of how much work this
// did. Returns 0 if anything failed, and the caller must not treat that
// as cosmetic: it means data is still only in RAM or only in the drive.
// ONE FILE'S DURABILITY, which is what fsync(2) means -- but scoped to
// that file's VOLUME, and the difference is worth stating rather than
// glossing.
//
// There is no page cache here, so nothing is held per FILE: what is
// deferred under `storage.sync = batched` is a journal transaction that
// may carry several files' inode blocks at once, and a device flush is
// a whole-drive operation either way. So this commits the backend
// holding that path and flushes the device under it -- everything
// needed for THIS file to be durable, plus whatever else shares the
// transaction. Narrower than `sync` (other mounts are untouched) and
// wider than POSIX promises.
//
// Per-file granularity would need the write-back page cache
// docs/pagecache-design.md stages, which is precisely why that document
// puts fsync AFTER it.
//
// **THE DEVICE FLUSH HAPPENS UNDER THE MOUNT'S LOCK**, not after it. A
// flush sleeps holding the DRIVER's lock (ata.c's g_ata_lock), and
// fs_exclusive_begin() promises its holder that nothing is at the disk
// -- the legacy `run` then freezes rotation and spins on any lock it
// meets. Issued after the unlock, logd's periodic fsync slipped past
// that promise and hung the machine: `run` spun on g_ata_lock behind a
// flush that could never be rotated to. Linux's sync_filesystem() runs
// under s_umount for the same reason. The cost is that calls on this
// mount wait out the flush.
int fs_sync_path(const char *path) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    mount_lock(r.m);
    int ok = 0;
    if (r.m->used && r.m->gen == r.gen) {
        ok = 1;
        int s = r.m->fs->sync ? r.m->fs->sync(r.m->state) : 1;
        if (!s) {
            klog_printf(KLOG_ERR "fs: fsync FAILED -- %s could not commit\n", r.m->point);
            ok = 0;
        } else if (r.m->dev && s != FS_SYNC_FENCED) {
            // A mount with no device (ramfs) has nothing to flush and is
            // durable in the only sense it can be.
            ok = blkdev_flush(r.m->dev);
        }
        if (ok) ((struct mount *)r.m)->settle_owed = 0;
    }
    mount_unlock(r.m);
    return ok;
}

// The kernel's idle work, for any backend that defers something --
// scheduler_idle() is its one owner (scheduler.c), and this sits beside
// atac_idle() for the same reason: a threshold bounds how MUCH can
// accumulate, only a timer bounds how LONG a machine nobody is touching
// holds it.
//
// Cheap when there is nothing to do, because it runs in every wait loop
// in the kernel: a backend with no `idle` costs one NULL test.
void fs_idle(void) {
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (!m || !m->fs || !m->fs->idle) continue;
        // TRIED, NEVER WAITED FOR -- a busy mount is not idle. This runs
        // in the kernel context, which cannot sleep, so a blocking take
        // spun behind a holder in a disk wait; and every disk IRQ handed
        // that holder the CPU back, so nothing else ran at all.
        if (!mount_trylock(m)) continue;
        uint32_t g = m->gen;
        while (m->used && m->gen == g) {
            m->fs->idle(m->state);
            if (!fs_op_again(m, g)) break;
        }
        mount_unlock(m);
    }
}

void fs_threads_start(void) {
    tfs3_threads_start();
}

// UNDER fs_exclusive_begin(), for fs_sync_path()'s reason: the ATA
// write-back and the device flushes below sleep holding driver locks,
// and they are per DEVICE rather than per mount. `sync` is rare
// (the command, shutdown), so holding everything costs nothing real.
static int fs_sync_locked(uint32_t *wrote_out);

int fs_sync(uint32_t *wrote_out) {
    fs_exclusive_begin();
    int ok = fs_sync_locked(wrote_out);
    fs_exclusive_end();
    return ok;
}

static int fs_sync_locked(uint32_t *wrote_out) {
    uint32_t wrote = 0, pending = 0;
    int ok = 1;

    // STAGE 0: land whatever a BACKEND is holding back, before either
    // of the stages below. `storage.sync = batched` lets TFS3 keep a
    // journal transaction open across writes, and flushing the device
    // without committing it first would report a durability that had
    // not been reached. Optional per backend -- NULL means nothing is
    // ever deferred, which is true of ramfs (fat32 holds a directory
    // sector on a removable disk).
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (!m || !m->fs || !m->fs->sync) continue;
        if (m->flags & MNT_GONE) continue;   // its disk was pulled: nothing to commit to
        if (!FS_OP(m, m->gen, sync)) {
            klog_printf(KLOG_ERR "fs: sync FAILED -- %s could not commit\n", m->point);
            ok = 0;
        }
    }

    if (ata_cache_active() && !ata_sync(&wrote, &pending)) {
        klog_printf(KLOG_ERR "fs: sync FAILED -- %u sector(s) still in RAM\n", pending);
        if (wrote_out) *wrote_out = wrote;
        return 0;   // a barrier cannot rescue a write that never left
    }

    // Deduped by DEVICE. Two partitions of one disk still cost two
    // flushes -- a partition forwards flush to its parent
    // (block_part.c) and nothing here can see that it did -- which is
    // correct, merely not minimal, and a flush is ~0.7 ms on real
    // hardware rather than free.
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (!m || !m->dev) continue;    // ramfs has no volume to flush
        if (m->flags & MNT_GONE) continue;
        int seen = 0;   // an earlier mount on the same device already flushed it
        for (int j = 0; j < i && !seen; j++) {
            const struct mount *e = mount_at(j);
            seen = e && e->dev == m->dev;
        }
        if (seen) continue;
        if (!blkdev_flush(m->dev)) {
            klog_printf(KLOG_ERR "fs: sync FAILED -- %s did not flush\n", m->point);
            ok = 0;
        }
    }
    if (ok)   // every mount's lock is held (fs_exclusive_begin())
        for (int i = 0; i < mount_count(); i++)
            if (mount_at(i)) ((struct mount *)mount_at(i))->settle_owed = 0;

    if (wrote_out) *wrote_out = wrote;
    return ok;
}

// QUICK REMOVAL (docs/usb-storage-design.md): on a disk a person can
// pull, a call by path that changed anything is on the device before it
// returns -- the backend's own sync (a batched TFS3 commit), then a
// device flush unless that sync says it ended in one. Passes `result`
// through, so a wrapper stays one expression; a call that failed
// (result <= 0) changed nothing to keep. The flush is under the mount's
// lock, as every device access for a mount is (kernel/fs/CLAUDE.md), so
// fs_exclusive_begin() still promises a quiet disk.
static int settle(const struct resolved *r, int result) {
    const struct mount *m = r->m;
    if (result <= 0 || !m->dev || !m->dev->removable) return result;
    mount_lock(m);
    if (m->used && m->gen == r->gen) {
        ((struct mount *)m)->settle_owed = 0;
        int s = m->fs->sync ? FS_OP(m, r->gen, sync) : 1;
        // The call it settles has returned: a write held for this moment
        // that fails can only be reported here.
        if (!s && !(m->flags & MNT_GONE))
            klog_printf(KLOG_ERR "fs: %s could not land a change already reported done\n", m->point);
        if (s != FS_SYNC_FENCED && !(m->flags & MNT_GONE)) blkdev_flush(m->dev);
    }
    mount_unlock(m);
    return result;
}

// THROUGH AN OPEN FILE, settled at its close instead (fs_settle_path()),
// the shape of Linux's vfat `flush`: a copy is one settle per file, not
// one per create and per write. A file that stays open settles here on
// the first write a second after the change it owes, so a long copy or
// a log still lands; one left open and idle settles at close or exit.
// No idle-time settle: fs_idle() cannot sleep, and a USB stick's
// command lock is a kmutex another partition's caller may hold asleep.
#define SETTLE_LATE_TICKS COARSE_HZ

static int owe(const struct resolved *r, int result) {
    const struct mount *m = r->m;
    if (result <= 0 || !m->dev || !m->dev->removable) return result;
    int due = 0;
    mount_lock(m);
    if (m->used && m->gen == r->gen) {
        struct mount *w = (struct mount *)m;
        uint64_t now = coarse_ticks();
        if (!w->settle_owed) w->settle_owed = now ? now : 1;
        due = now - w->settle_owed >= SETTLE_LATE_TICKS;
    }
    mount_unlock(m);
    return due ? settle(r, result) : result;
}

void fs_settle_path(const char *path) {
    struct resolved r;
    // Read unlocked: a stale nonzero costs one extra settle, and a zero
    // means someone settled everything this file owed.
    if (!resolve(path, &r) || !r.m->settle_owed) return;
    settle(&r, 1);
}

// Bump on a truthy result, and pass that result straight through, so a
// wrapper stays a one-liner and no call site can bump without also
// returning what the backend said.
static inline int bumped(int ok) {
    if (ok) g_generation++;
    return ok;
}

// ...and tell anyone WATCHING that path or its directory (fswatch.h).
// The generation is for "anything changed"; this is "THIS changed".
static int changed(int ok, const char *path) {
    if (!bumped(ok)) return ok;
    uint64_t self, parent;
    fswatch_hash(path, &self, &parent);
    fswatch_note(self, parent);
    dirgen_note(self, parent);
    cachesim_file_changed(self);
    return ok;
}

// Two names, one change: a rename or a link bumps once.
static int changed2(int ok, const char *a, const char *b) {
    if (!changed(ok, a)) return ok;
    uint64_t self, parent;
    fswatch_hash(b, &self, &parent);
    fswatch_note(self, parent);
    dirgen_note(self, parent);
    cachesim_file_changed(self);
    return ok;
}

// `fin` is settle() for a call made by path, owe() for one made through
// an open file (fs_defer.h).
typedef int (*settle_fn)(const struct resolved *r, int result);

static int touch_with(const char *path, settle_fn fin) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "touch", path)) return 0;
    return changed(fin(&r, FS_OP(r.m, r.gen, touch, r.sub)), path);
}
int fs_touch(const char *path) { return touch_with(path, settle); }
int fs_touch_deferred(const char *path) { return touch_with(path, owe); }

static int write_with(const char *path, const char *data, int append, settle_fn fin) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "write", path)) return 0;
    imgcache_forget(path);
    return changed(fin(&r, FS_OP(r.m, r.gen, write, r.sub, data, append)), path);
}
int fs_write(const char *path, const char *data, int append) {
    return write_with(path, data, append, settle);
}
int fs_write_deferred(const char *path, const char *data, int append) {
    return write_with(path, data, append, owe);
}

static int create_excl_with(const char *path, settle_fn fin) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "create", path)) return 0;
    if (!r.m->fs->create_excl) return 0;
    int rc = fin(&r, FS_OP(r.m, r.gen, create_excl, r.sub));
    if (rc > 0) changed(1, path);
    return rc;
}
int fs_create_excl(const char *path) { return create_excl_with(path, settle); }
int fs_create_excl_deferred(const char *path) { return create_excl_with(path, owe); }

int fs_mkdir(const char *path) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "mkdir", path)) return 0;
    return changed(settle(&r, FS_OP(r.m, r.gen, mkdir, r.sub)), path);
}

int fs_delete(const char *path) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "delete", path)) return 0;
    // A MOUNT POINT IS NOT A FILE. Deleting one would remove the
    // directory the mount hangs on and leave a mount pointing at
    // nothing -- Unix's EBUSY.
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (m && k_strcmp(m->point, path) == 0) {
            klog_printf(KLOG_ERR "fs: refusing to delete \"%s\" -- %s is mounted there\n",
                        path, m->fs->name);
            return 0;
        }
    }
    return changed(settle(&r, FS_OP(r.m, r.gen, del, r.sub)), path);
}

uint32_t fs_read_into(const char *path, void *buf, uint32_t cap) {
    if (!buf || cap == 0) return 0;
    uint8_t *dst = (uint8_t *)buf;
    dst[0] = '\0';

    struct resolved r;
    if (!resolve(path, &r)) return 0;

    uint64_t size = FS_OP(r.m, r.gen, size, r.sub);
    // Room for the NUL as well, so a text caller can scan the result as
    // a string without a separate length check at every step.
    if (size == 0 || size + 1 > (uint64_t)cap) return 0;

    // A loop rather than one call: fs_read_range() may legitimately
    // return short (see its contract), and treating a short read as the
    // whole file is how a truncated parse gets in.
    uint32_t got = 0;
    while (got < (uint32_t)size) {
        uint32_t n = FS_OP(r.m, r.gen, read_range, r.sub, got, dst + got, (uint32_t)size - got);
        if (n == 0) return 0; // EOF-before-size or a real failure; either way, refuse
        got += n;
    }
    dst[got] = '\0';
    cachesim_file_read(path, 0, got);
    return got;
}

uint64_t fs_size(const char *path) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return FS_OP(r.m, r.gen, size, r.sub);
}

uint32_t fs_read_range(const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    uint32_t n = FS_OP(r.m, r.gen, read_range, r.sub, offset, buf, len);
    cachesim_file_read(path, offset, n);
    return n;
}

static int write_range_with(const char *path, uint64_t offset, const void *buf, uint32_t len,
                            settle_fn fin) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "write", path)) return 0;
    imgcache_forget(path);
    return changed(fin(&r, FS_OP(r.m, r.gen, write_range, r.sub, offset, buf, len)), path);
}
int fs_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    return write_range_with(path, offset, buf, len, settle);
}
int fs_write_range_deferred(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    return write_range_with(path, offset, buf, len, owe);
}

// A STEP CARRIES NO PATH, so it cannot be resolved. The handle came
// from a backend and has to go back to the same one -- which is what
// this records when begin() succeeded. Two streams from two mounts can
// be in flight at once, so it is per handle rather than a single "the
// last backend that began something".
//
// The wrapper is also what makes a handle SAFE TO STEP TWICE: a
// terminal result clears the slot, so a second step finds no backend
// and fails cleanly rather than handing a freed pointer to a driver.
struct step_handle {
    const struct mount *m;   // NULL = free slot, and = "already finished"
    uint32_t gen;            // the mount's generation at begin -- see FS_OP
    void *inner;
    uint64_t self, parent;   // the written path's fswatch hashes -- the
                             // path itself is the caller's, and gone
};

// Its OWN lock: two mounts' streams can begin at once, and a slot is
// claimed by a test-then-set that no mount's lock covers.
static struct step_handle g_steps[8];
static struct kmutex g_steps_lock;

static void *step_wrap(const struct resolved *r, void *inner) {
    if (!inner) return 0;
    struct step_handle *h = 0;
    kmutex_lock(&g_steps_lock);
    for (unsigned i = 0; i < sizeof g_steps / sizeof g_steps[0]; i++) {
        if (!g_steps[i].m) {
            h = &g_steps[i];
            h->m = r->m; h->gen = r->gen; h->inner = inner;
            break;
        }
    }
    kmutex_unlock(&g_steps_lock);
    return h; // NULL: more streams in flight than slots -- refused, not misrouted
}

void *fs_write_range_begin(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "write", path)) return 0;
    struct step_handle *h = step_wrap(&r, FS_OP(r.m, r.gen, write_range_begin, r.sub, offset, buf, len));
    if (h) fswatch_hash(path, &h->self, &h->parent);
    return h;
}

enum fs_step_result fs_write_range_step(void *handle) {
    // A NULL handle means fs_write_range_begin() already failed (or the
    // caller mistakenly stepped a handle twice past its terminal
    // result, which frees it) -- fail cleanly here rather than handing
    // NULL to a backend that assumes a valid handle, same "defend at
    // the dispatch boundary, not in every backend" spirit as the rest
    // of this file.
    if (!handle) return FS_STEP_FAILED;
    struct step_handle *h = handle;
    if (!h->m) return FS_STEP_FAILED;
    struct resolved at = { h->m, 0, h->gen };   // for settle(), before a terminal result clears it
    enum fs_step_result r = (enum fs_step_result)FS_OP(h->m, h->gen, write_range_step, h->inner);
    uint64_t self = h->self, parent = h->parent;
    if (r == FS_STEP_DONE || r == FS_STEP_FAILED) { h->m = 0; h->inner = 0; }
    // Bump once, on completion -- not per step. A streamed write is one
    // change to the filesystem however many slices it took, and bumping
    // per step would wake a watcher repeatedly through a single save.
    // This path is the one Notepad saves through, so without it editing
    // a file in the editor would not be seen by anything watching.
    if (r == FS_STEP_DONE) {
        settle(&at, 1);
        g_generation++; fswatch_note(self, parent); dirgen_note(self, parent);
        cachesim_file_changed(self);
    }
    return r;
}

void *fs_read_range_begin(const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    void *h = step_wrap(&r, FS_OP(r.m, r.gen, read_range_begin, r.sub, offset, buf, len));
    // Counted at begin, the whole range: the steps carry no path.
    if (h) cachesim_file_read(path, offset, len);
    return h;
}

enum fs_step_result fs_read_range_step(void *handle, uint32_t *out_total) {
    // Same "defend at the dispatch boundary" reasoning as
    // fs_write_range_step() above.
    if (!handle) {
        if (out_total) *out_total = 0;
        return FS_STEP_FAILED;
    }
    struct step_handle *h = handle;
    if (!h->m) { if (out_total) *out_total = 0; return FS_STEP_FAILED; }
    enum fs_step_result r = (enum fs_step_result)FS_OP(h->m, h->gen, read_range_step, h->inner, out_total);
    if (r == FS_STEP_DONE || r == FS_STEP_FAILED) { h->m = 0; h->inner = 0; }
    return r;
}

// TWO PATHS, ONE MOUNT. Unix returns EXDEV for a cross-device rename
// and every tool knows to fall back to copy-then-delete; doing the
// copy here would make one call mean two very different amounts of
// work, and a half-finished one leave a file in both places.
int fs_rename(const char *oldpath, const char *newpath) {
    struct resolved a, b;
    if (!resolve(oldpath, &a) || !resolve(newpath, &b)) return 0;
    if (a.m != b.m) {
        klog_printf(KLOG_ERR "fs: refusing to rename across mounts (\"%s\" -> \"%s\") -- use cp\n",
                    oldpath, newpath);
        return 0;
    }
    if (!writable(&a, "rename", oldpath)) return 0;
    // BOTH names change meaning, and the DESTINATION is the one that
    // matters: an updated /lib file arrives by rename-into-place
    // (tftpd's, dpkg's shape), so hooking only the write paths left the
    // cache serving the file this replaces.
    imgcache_forget(oldpath);
    imgcache_forget(newpath);
    return changed2(settle(&a, FS_OP(a.m, a.gen, rename, a.sub, b.sub)), oldpath, newpath);
}

int fs_path_has(const char *path, uint32_t cap) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return (r.m->fs->caps & cap) == cap;
}

int fs_rename_replace(const char *oldpath, const char *newpath) {
    struct resolved a, b;
    if (!resolve(oldpath, &a) || !resolve(newpath, &b)) return 0;
    if (a.m != b.m) return 0;
    if (!FS_OP(b.m, b.gen, exists, b.sub)) return fs_rename(oldpath, newpath);
    if (!writable(&a, "rename", oldpath)) return 0;
    if (!a.m->fs->rename_replace) return 0;   // no atomic swap: refused, never emulated
    imgcache_forget(oldpath);
    imgcache_forget(newpath);
    return changed2(settle(&a, FS_OP(a.m, a.gen, rename_replace, a.sub, b.sub)), oldpath, newpath);
}

int fs_truncate(const char *path, uint64_t size) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "truncate", path)) return 0;
    imgcache_forget(path);
    return changed(settle(&r, FS_OP(r.m, r.gen, truncate, r.sub, size)), path);
}

int fs_is_dir(const char *path) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return FS_OP(r.m, r.gen, is_dir, r.sub);
}

int fs_exists(const char *path) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return FS_OP(r.m, r.gen, exists, r.sub);
}

int fs_readonly(const char *path) {
    struct resolved r;
    return resolve(path, &r) && (r.m->flags & MNT_RDONLY);
}

void fs_list(const char *dir_path, fs_list_cb cb, void *ctx) {
    struct resolved r;
    if (!resolve(dir_path, &r)) return;
    // The callback runs inside the section too -- it has to, since the
    // walk holds backend state across it. A callback that only records
    // what it is handed (every caller here) is fine; one that called
    // back into fs_* would be re-entering the backend directly, which
    // no amount of preemption control can make safe.
    FS_OP_VOID(r.m, r.gen, list, r.sub, cb, ctx);
}

int fs_stat(const char *path, struct fs_stat_info *out) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return FS_OP(r.m, r.gen, stat, r.sub, out);
}

int fs_chmod(const char *path, uint16_t mode) {
    struct resolved r;
    if (!resolve(path, &r)) return -ENOENT;
    // A backend with nowhere to keep permission bits leaves the slot
    // NULL rather than accepting and discarding them -- see fs_ops.h.
    if (!r.m->fs->chmod) return -ENOTSUP;
    // PERMISSIONS ONLY. The type bits are the filesystem's, and a chmod
    // that could rewrite them would be a corruption primitive.
    int rc = FS_OP(r.m, r.gen, chmod, r.sub, (uint16_t)(mode & 07777));
    if (rc == 0) settle(&r, 1);   // chmod's success is 0, not a count
    return rc;
}

// THE ROOT's usage. `df` reports every mount by walking the mount table
// through QUERY_FSINFO; this stays the root's answer because every
// caller of it means "how full is the disk".
int fs_disk_usage(uint64_t *out_used_bytes, uint64_t *out_total_bytes) {
    const struct mount *m = mount_root();
    if (!m) return 0;
    return FS_OP(m, m->gen, disk_usage, out_used_bytes, out_total_bytes);
}

// Per-mount usage, guarded. See api/fs.h -- the point of this function
// existing at all is that the QUERY_FSINFO provider was calling
// `m->fs->disk_usage()` directly, which is one line and skips the
// preemption guard every other backend call in this file takes.
static struct fs_check_live *live_of(const struct mount *m);

int fs_mount_usage(const void *mount, uint64_t *out_used_bytes, uint64_t *out_total_bytes) {
    struct mount *m = (struct mount *)mount;   // the cache below is the slot's own state
    if (!m || !m->fs) return 0;
    if (__atomic_load_n(&m->long_ops, __ATOMIC_ACQUIRE) &&
        m->usage_known) {
        if (out_used_bytes) *out_used_bytes = m->usage_used;
        if (out_total_bytes) *out_total_bytes = m->usage_total;
        return 1;
    }
    uint64_t used = 0, total = 0;
    int ok = FS_OP(m, m->gen, disk_usage, &used, &total);
    if (ok) {
        m->usage_used = used;
        m->usage_total = total;
        m->usage_known = 1;
        if (out_used_bytes) *out_used_bytes = used;
        if (out_total_bytes) *out_total_bytes = total;
    }
    return ok;
}

// A whole-volume pass, announced before its lock is taken (mount.h's
// `long_ops`), so fs_mount_usage() answers from memory meanwhile.
static int long_op(const struct mount *mc, uint32_t gen, int repair, struct fs_check_result *out) {
    struct mount *m = (struct mount *)mc;
    __atomic_add_fetch(&m->long_ops, 1, __ATOMIC_ACQ_REL);
    int rc = FS_OP(m, gen, check, repair, out, live_of(m));
    __atomic_sub_fetch(&m->long_ops, 1, __ATOMIC_ACQ_REL);
    return rc;
}

// The slot's progress is the slot's own mutable state, as its lock is.
static struct fs_check_live *live_of(const struct mount *m) {
    return (struct fs_check_live *)&m->check;
}

int fs_check(int repair, struct fs_check_result *out) {
    const struct mount *m = mount_root();
    if (!m) return 0;
    if (repair && (m->flags & MNT_RDONLY)) return 0;
    return long_op(m, m->gen, repair, out) == 1;
}

int fs_check_at(const char *path, int repair, struct fs_check_result *out) {
    struct resolved r;
    if (!path || !resolve(path, &r)) return -ENOENT;
    if (repair && (r.m->flags & MNT_RDONLY)) return -EROFS;
    if (repair && !(r.m->fs->caps & FS_CAP_REPAIR)) return -ENOTSUP;
    int rc = long_op(r.m, r.gen, repair, out);
    if (repair) settle(&r, 1);   // a repair is a mutating call like any other
    if (rc < 0) return rc;   // -ECANCELED: stopped on request
    if (!rc) return -EIO;
    k_strlcpy(out->fstype, r.m->fs->name, sizeof out->fstype);
    k_strlcpy(out->point, r.m->point, sizeof out->point);
    return 0;
}

// NO LOCK: the pass this reads holds the mount's for its whole length.
int fs_check_progress_at(const char *path, struct fs_check_progress *out) {
    struct resolved r;
    if (!path || !resolve(path, &r)) return -ENOENT;
    const struct fs_check_progress *p = &r.m->check.prog;
    k_memset(out, 0, sizeof *out);
    out->running = __atomic_load_n(&p->running, __ATOMIC_ACQUIRE);
    if (!out->running) return 0;
    out->repair = __atomic_load_n(&p->repair, __ATOMIC_RELAXED);
    out->stage = __atomic_load_n(&p->stage, __ATOMIC_RELAXED);
    out->stages = __atomic_load_n(&p->stages, __ATOMIC_RELAXED);
    out->done = __atomic_load_n(&p->done, __ATOMIC_RELAXED);
    out->total = __atomic_load_n(&p->total, __ATOMIC_RELAXED);
    out->problems = __atomic_load_n(&p->problems, __ATOMIC_RELAXED);
    out->records = __atomic_load_n(&p->records, __ATOMIC_RELAXED);
    out->blocks = __atomic_load_n(&p->blocks, __ATOMIC_RELAXED);
    // The names are written before `running` is set, and only then.
    k_memcpy(out->names, p->names, sizeof out->names);
    k_memcpy(out->units, p->units, sizeof out->units);
    return 0;
}

int fs_check_stop_at(const char *path) {
    struct resolved r;
    if (!path || !resolve(path, &r)) return -ENOENT;
    struct fs_check_live *l = live_of(r.m);
    if (!__atomic_load_n(&l->prog.running, __ATOMIC_ACQUIRE)) return -ESRCH;
    if (__atomic_load_n(&l->prog.repair, __ATOMIC_RELAXED)) return -EBUSY;
    __atomic_store_n(&l->stop, 1, __ATOMIC_RELAXED);
    return 0;
}

int fs_link(const char *existing, const char *newpath) {
    struct resolved a, b;
    if (!resolve(existing, &a) || !resolve(newpath, &b)) return 0;
    if (a.m != b.m) return 0; // a hard link cannot cross a filesystem, anywhere
    if (!writable(&a, "link", newpath)) return 0;
    // Optional op -- the caps bit and this NULL check are the same
    // fact, and caps_are_honest() made sure they can't disagree.
    if (!a.m->fs->link) return 0;
    return changed(settle(&a, FS_OP(a.m, a.gen, link, a.sub, b.sub)), newpath);
}
