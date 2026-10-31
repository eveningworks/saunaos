// KTESTs for a disk that goes away (block.h's blk_device_gone(), the
// USB-stick case) and for quick removal (vfs.c's settle()), on a RAM
// disk the test adds after boot, partitions, formats, mounts -- and
// pulls. Asserted through the same calls a person's tools use: the
// device table's names, mount_add()/mount_remove(), fs_write().
#include "ktest.h"
#include "block.h"
#include "mount.h"
#include "fat32.h"
#include "tfs3.h"
#include "storage_config.h"
#include "fs.h"
#include "heap.h"
#include "string.h"
#include "tmppath.h"
#include "fs_defer.h"
#include "syscalls.h" // an open file's description, closed for real
#include "kfmt.h"   // klog_printf: the flush counts
#include "timer.h"

#define YK_SECTORS 16384u            // 8 MiB
#define YK_PART_LBA 2048u

static uint8_t *g_disk;
static int g_unplugged, g_released;

static uint64_t yk_count(const struct block_device *self) { (void)self; return YK_SECTORS; }

static int yk_read(const struct block_device *self, uint64_t lba, int count, void *buf) {
    (void)self;
    if (g_unplugged || !g_disk || count <= 0 || lba + (uint64_t)count > YK_SECTORS) return 0;
    k_memcpy(buf, g_disk + lba * 512, (uint32_t)count * 512);
    return 1;
}

static int g_writes;
static int yk_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    (void)self;
    g_writes++;
    if (g_unplugged || !g_disk || count <= 0 || lba + (uint64_t)count > YK_SECTORS) return 0;
    k_memcpy(g_disk + lba * 512, buf, (uint32_t)count * 512);
    return 1;
}

static int yk_xfer(const struct block_device *self) { (void)self; return 64; }
static int g_flushes;
static int yk_flush(const struct block_device *self) { (void)self; g_flushes++; return !g_unplugged; }
static void yk_release(const struct block_device *self) { (void)self; g_released++; }

static const struct block_device YK_DEV = {
    .name = "ktyank",
    .sector_count = yk_count,
    .read_sectors = yk_read,
    .write_sectors = yk_write,
    .max_sectors_per_xfer = yk_xfer,
    .persistent = 1,
    .caps = BLK_CAP_FLUSH,
    .flush = yk_flush,
    .removable = 1,
    .no_automount = 1,   // the tests mount it themselves; automountd racing them failed one
    .release = yk_release,
};

// An MBR with one FAT32 (LBA) partition, then that partition formatted.
static int build_disk(void) {
    if (!g_disk) g_disk = kmalloc(YK_SECTORS * 512u);
    if (!g_disk) return 0;
    k_memset(g_disk, 0, YK_SECTORS * 512u);
    g_unplugged = g_released = 0;
    uint8_t *e = g_disk + 0x1BE;
    e[4] = 0x0C;
    uint32_t start = YK_PART_LBA, n = YK_SECTORS - YK_PART_LBA;
    for (int i = 0; i < 4; i++) { e[8 + i] = (uint8_t)(start >> (8 * i)); e[12 + i] = (uint8_t)(n >> (8 * i)); }
    g_disk[510] = 0x55; g_disk[511] = 0xAA;
    return 1;
}

static int format_window(const struct block_device *part, const struct fs_ops *fs) {
    struct fs_scratch sc;
    if (!mount_scratch_begin(fs, &sc)) return 0;
    int ok = fs->format(sc.st, part);
    mount_scratch_end(&sc);
    return ok;
}

static const char *point(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "yank");
    return p;
}

static const char *file_on(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) {
        k_strlcpy(p, point(), sizeof p);
        k_strlcat(p, "/a.txt", sizeof p);
    }
    return p;
}

KTEST("blk-gone", "a pulled disk's mount turns read-only, and the disk is released at unmount") {
    if (!build_disk()) KTEST_SKIP("could not allocate an 8 MiB test disk");
    KTEST_ASSERT(blk_add_disk(&YK_DEV));
    const struct blk_entry *part = blk_device_by_name("ktyank0p1");
    KTEST_ASSERT(part != 0);   // the added hook named its partition
    const struct block_device *pdev = part->dev;
    KTEST_ASSERT(pdev->removable);   // inherited from the disk
    KTEST_ASSERT(format_window(pdev, &fat32_ops));

    fs_mkdir(point());
    const char *why = "";
    int mounted = mount_add(pdev, "fat32", point(), 0, 0, &why);
    int wrote = mounted && fs_write(file_on(), "before", 0);

    g_unplugged = 1;
    blk_device_gone(&YK_DEV);
    int named_while_mounted = blk_device_by_name("ktyank0p1") != 0;
    int write_refused = !fs_write(file_on(), "after", 0);
    const struct mount *m = mount_resolve(file_on(), 0);
    int flagged = m && (m->flags & MNT_GONE) && (m->flags & MNT_RDONLY);
    int remount_refused = !mount_add(pdev, "fat32", "/", 0, 0, &why);
    int released_early = g_released;

    int unmounted = mount_remove(point(), &why);
    int released = g_released;
    int names_gone = !blk_device_by_name("ktyank0") && !blk_device_by_name("ktyank0p1");
    fs_delete(point());

    KTEST_ASSERT(mounted);
    KTEST_ASSERT(wrote);
    KTEST_ASSERT(named_while_mounted);
    KTEST_ASSERT(write_refused);
    KTEST_ASSERT(flagged);
    KTEST_ASSERT(remount_refused);
    KTEST_ASSERT_EQ(released_early, 0);
    KTEST_ASSERT(unmounted);
    KTEST_ASSERT_EQ(released, 1);
    KTEST_ASSERT(names_gone);
}

KTEST("blk-gone", "a pulled disk nothing mounted is released at once") {
    if (!build_disk()) KTEST_SKIP("could not allocate an 8 MiB test disk");
    KTEST_ASSERT(blk_add_disk(&YK_DEV));
    int named = blk_device_by_name("ktyank0p1") != 0;
    g_unplugged = 1;
    blk_device_gone(&YK_DEV);
    KTEST_ASSERT(named);
    KTEST_ASSERT_EQ(g_released, 1);
    KTEST_ASSERT(!blk_device_by_name("ktyank0"));
    KTEST_ASSERT(!blk_device_by_name("ktyank0p1"));
}

// The pulled stick, as the next machine sees it: a copy of the disk taken
// the moment a call returned, mounted fresh.
static uint8_t *g_snap;
static uint64_t snap_count(const struct block_device *self) { (void)self; return YK_SECTORS - YK_PART_LBA; }
static int snap_read(const struct block_device *self, uint64_t lba, int count, void *buf) {
    (void)self;
    if (count <= 0 || lba + (uint64_t)count > YK_SECTORS - YK_PART_LBA) return 0;
    k_memcpy(buf, g_snap + (YK_PART_LBA + lba) * 512, (uint32_t)count * 512);
    return 1;
}
static int snap_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    (void)self;
    if (count <= 0 || lba + (uint64_t)count > YK_SECTORS - YK_PART_LBA) return 0;
    k_memcpy(g_snap + (YK_PART_LBA + lba) * 512, buf, (uint32_t)count * 512);   // a journal replay
    return 1;
}
static const struct block_device SNAP_DEV = {
    .name = "ktsnap",
    .sector_count = snap_count,
    .read_sectors = snap_read,
    .write_sectors = snap_write,
    .max_sectors_per_xfer = yk_xfer,
};

// The case quick removal exists for: TFS3 under `storage.sync = batched`
// keeps a write's inode update in an open transaction, which a pull
// right after the call returned would lose. On a removable disk it may not.
KTEST("blk-gone", "a write on a removable disk is on the disk when the call returns") {
    if (!build_disk()) KTEST_SKIP("could not allocate an 8 MiB test disk");
    if (!g_snap) g_snap = kmalloc(YK_SECTORS * 512u);
    if (!g_snap) KTEST_SKIP("could not allocate the snapshot");
    KTEST_ASSERT(blk_add_disk(&YK_DEV));
    const struct blk_entry *part = blk_device_by_name("ktyank0p1");
    KTEST_ASSERT(part != 0);
    KTEST_ASSERT(format_window(part->dev, &tfs3_ops));

    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);   // barriers real, commits deferred
    fs_mkdir(point());
    const char *why = "";
    int mounted = mount_add(part->dev, "tfs3", point(), 0, 0, &why);
    static char data[4096];
    for (int i = 0; i < 4096; i++) data[i] = (char)('a' + i % 26);
    int wrote = mounted && fs_write_range(file_on(), 0, data, sizeof data);
    k_memcpy(g_snap, g_disk, YK_SECTORS * 512u);   // pulled right here
    storage_config_set_mode_for_test(was_strict, was_batched);

    if (mounted) mount_remove(point(), &why);
    fs_delete(point());
    g_unplugged = 1;
    blk_device_gone(&YK_DEV);

    uint64_t size = 0;
    int same = 0;
    struct fs_scratch sc;
    if (mount_scratch_begin(&tfs3_ops, &sc)) {
        if (tfs3_ops.init(sc.st, &SNAP_DEV, 0) == 1) {
            size = tfs3_ops.size(sc.st, "/a.txt");
            static char back[4096];
            same = size == sizeof back &&
                   tfs3_ops.read_range(sc.st, "/a.txt", 0, back, sizeof back) == sizeof back &&
                   k_memcmp(back, data, sizeof back) == 0;
            if (tfs3_ops.umount) tfs3_ops.umount(sc.st, &SNAP_DEV);
        }
        mount_scratch_end(&sc);
    }
    KTEST_ASSERT(mounted);
    KTEST_ASSERT(wrote);
    KTEST_ASSERT_EQ(size, 4096);
    KTEST_ASSERT(same);
}

// Partition windows GROW: thirty on one disk, more than the sixteen the
// pool used to hold, each its own device and each released.
KTEST("blk-gone", "partition windows grow past sixteen") {
    if (!build_disk()) KTEST_SKIP("could not allocate an 8 MiB test disk");
    KTEST_ASSERT(blk_add_disk(&YK_DEV));
    enum { N = 30 };
    const struct block_device *w[N];
    int made = 0, distinct = 1;
    for (int i = 0; i < N; i++) {
        w[i] = blk_part_create(&YK_DEV, 4096u + (uint64_t)i * 64u, 64, 10 + i);
        if (w[i]) made++;
        for (int j = 0; j < i; j++) if (w[i] && w[i] == w[j]) distinct = 0;
    }
    int released = 0;
    for (int i = 0; i < N; i++) if (w[i]) released += blk_part_release(w[i]);
    g_unplugged = 1;
    blk_device_gone(&YK_DEV);
    KTEST_ASSERT_EQ(made, N);
    KTEST_ASSERT(distinct);
    KTEST_ASSERT_EQ(released, N);
}

// ---- settled at close (vfs.c's owe()) ----------------------------------

// What a fresh mount of the snapshot says /a.txt holds: its size, and
// whether its bytes are `want`'s. -1 when the snapshot does not mount.
static int64_t snap_file(const char *want, uint32_t len, int *same) {
    int64_t size = -1;
    *same = 0;
    struct fs_scratch sc;
    if (!mount_scratch_begin(&tfs3_ops, &sc)) return -1;
    if (tfs3_ops.init(sc.st, &SNAP_DEV, 0) == 1) {
        size = (int64_t)tfs3_ops.size(sc.st, "/a.txt");
        static char back[4096];
        *same = size == len && len <= sizeof back &&
                tfs3_ops.read_range(sc.st, "/a.txt", 0, back, len) == len &&
                k_memcmp(back, want, len) == 0;
        if (tfs3_ops.umount) tfs3_ops.umount(sc.st, &SNAP_DEV);
    }
    mount_scratch_end(&sc);
    return size;
}

static const struct mount *mount_of(const char *pt) {
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (m && m->used && k_strcmp(m->point, pt) == 0) return m;
    }
    return 0;
}

// The TFS3 stick, mounted under `batched` so a commit can be deferred at
// all; 0 when any of that failed. Undone by yank_down().
static int g_was_strict, g_was_batched;
static int yank_up_as(const struct fs_ops *fs) {
    if (!build_disk()) return 0;
    if (!g_snap) g_snap = kmalloc(YK_SECTORS * 512u);
    if (!g_snap || !blk_add_disk(&YK_DEV)) return 0;
    const struct blk_entry *part = blk_device_by_name("ktyank0p1");
    if (!part || !format_window(part->dev, fs)) return 0;
    g_was_strict = storage_sync_strict(); g_was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    fs_mkdir(point());
    const char *why = "";
    return mount_add(part->dev, fs->name, point(), 0, 0, &why) == 1;
}
static int yank_up(void) { return yank_up_as(&tfs3_ops); }
static void yank_down(void) {
    const char *why = "";
    storage_config_set_mode_for_test(g_was_strict, g_was_batched);
    mount_remove(point(), &why);
    fs_delete(point());
    g_unplugged = 1;
    blk_device_gone(&YK_DEV);
}

// What an open file wrote is NOT on the stick before its close -- the
// saving -- and IS once it closes. A pull between loses that file only.
KTEST("blk-gone", "a write through an open file on a removable disk lands at its close") {
    int up = yank_up();
    static char data[4096];
    for (int i = 0; i < 4096; i++) data[i] = (char)('A' + i % 26);
    int di = up ? fd_desc_alloc(&file_fd_ops, -1) : -1;
    if (di >= 0) {
        k_strlcpy(fd_desc_at(di)->file.name, file_on(), sizeof fd_desc_at(di)->file.name);
        fd_desc_at(di)->file.mode = FD_MODE_WRITE;
    }
    int wrote = di >= 0 && fs_touch_deferred(file_on()) &&
                fs_write_range_deferred(file_on(), 0, data, sizeof data);
    k_memcpy(g_snap, g_disk, YK_SECTORS * 512u);   // pulled before the close
    int same_open = 0;
    int64_t open_size = snap_file(data, sizeof data, &same_open);
    if (di >= 0) fd_desc_unref(di);                  // the close
    k_memcpy(g_snap, g_disk, YK_SECTORS * 512u);   // pulled after it
    int same = 0;
    int64_t size = snap_file(data, sizeof data, &same);
    if (up) yank_down();
    KTEST_ASSERT(up);
    KTEST_ASSERT(wrote);
    KTEST_ASSERT(!same_open);   // deferred: nothing was settled per call
    KTEST_ASSERT(open_size != (int64_t)sizeof data);
    KTEST_ASSERT_EQ(size, (int64_t)sizeof data);
    KTEST_ASSERT(same);
}

// A file kept open still lands: the first write a second after the
// change it owes settles everything owed.
KTEST("blk-gone", "an open file's write settles once its change is a second old") {
    if (coarse_ticks() <= COARSE_HZ) KTEST_SKIP("booted under a second ago");
    int up = yank_up();
    static char data[4096];
    for (int i = 0; i < 4096; i++) data[i] = (char)('a' + i % 26);
    int wrote = up && fs_touch_deferred(file_on()) &&
                fs_write_range_deferred(file_on(), 0, data, 2048);
    const struct mount *m = up ? mount_of(point()) : 0;
    int owed = m && m->settle_owed;
    if (m) ((struct mount *)m)->settle_owed = coarse_ticks() - COARSE_HZ;   // a second ago
    wrote = wrote && fs_write_range_deferred(file_on(), 2048, data + 2048, 2048);
    int settled = m && !m->settle_owed;
    k_memcpy(g_snap, g_disk, YK_SECTORS * 512u);   // pulled, never closed
    int same = 0;
    int64_t size = snap_file(data, sizeof data, &same);
    if (up) yank_down();
    KTEST_ASSERT(up);
    KTEST_ASSERT(wrote);
    KTEST_ASSERT(owed);
    KTEST_ASSERT(settled);
    KTEST_ASSERT_EQ(size, (int64_t)sizeof data);
    KTEST_ASSERT(same);
}

// THE POINT OF IT, counted: small files copied as `cp` does (create,
// write, close) against the same files written by path, where every call
// settles on its own. On FAT32 the close's one flush is all a file costs,
// and its directory sector is written once (fat32.c's dir_put()) -- seven
// writes a file before; a TFS3 create still commits its own transaction
// (t3_txn_begin()), so there the saving is the flush after a commit that
// already ended in a barrier (FS_SYNC_FENCED) -- eight a file before.
static int g_fd_writes;
static void count_flushes(const struct fs_ops *fs, int *by_path, int *by_fd, int *ok) {
    enum { FILES = 8 };
    static char data[1024];
    for (int i = 0; i < 1024; i++) data[i] = (char)i;
    char path[FS_PATH_MAX];
    *ok = yank_up_as(fs);
    int up = *ok;
    g_flushes = 0;
    g_writes = 0;
    for (int i = 0; *ok && i < FILES; i++) {
        k_strlcpy(path, point(), sizeof path);
        k_strlcat(path, "/p_", sizeof path);
        path[k_strlen(path) - 1] = (char)('a' + i);
        *ok = fs_touch(path) && fs_write_range(path, 0, data, sizeof data);
    }
    *by_path = g_flushes;
    int w_path = g_writes;
    g_flushes = 0;
    g_writes = 0;
    for (int i = 0; *ok && i < FILES; i++) {
        k_strlcpy(path, point(), sizeof path);
        k_strlcat(path, "/q_", sizeof path);
        path[k_strlen(path) - 1] = (char)('a' + i);
        *ok = fs_touch_deferred(path) && fs_write_range_deferred(path, 0, data, sizeof data);
        fs_settle_path(path);
    }
    *by_fd = g_flushes;
    g_fd_writes = g_writes;
    klog_printf("ktest: %s, %d small files: %d flushes by path, %d through open files; "
                "%d writes by path, %d through open files\n",
                fs->name, FILES, *by_path, *by_fd, w_path, g_writes);
    if (up) yank_down();
}

KTEST("blk-gone", "a FAT32 stick's small files cost one flush each through open files") {
    int by_path, by_fd, ok;
    count_flushes(&fat32_ops, &by_path, &by_fd, &ok);
    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(by_fd, 8);
    KTEST_ASSERT(by_fd * 2 <= by_path);
    KTEST_ASSERT(g_fd_writes <= 5 * 8);
}

KTEST("blk-gone", "a TFS3 stick skips the flush after a commit that ended in one") {
    int by_path, by_fd, ok;
    count_flushes(&tfs3_ops, &by_path, &by_fd, &ok);
    KTEST_ASSERT(ok);
    KTEST_ASSERT(by_fd <= by_path);
    KTEST_ASSERT(by_path <= 6 * 8);
}

// FAT32's HELD DIRECTORY SECTOR (fat32.c's dir_put()), pulled after the
// settle: every change that went through it -- a file made through an
// open file and closed, a rename, a delete, a folder and a file in it --
// is on the stick, the bytes are right, and a check of it finds nothing.
static int snap_fat_file(void *st, const char *path, const char *want, uint32_t len) {
    static char back[2048];
    return fat32_ops.size(st, path) == len && len <= sizeof back &&
           fat32_ops.read_range(st, path, 0, back, len) == len && k_memcmp(back, want, len) == 0;
}

KTEST("blk-gone", "a FAT32 stick's held directory sector is all on it after the settle") {
    int up = yank_up_as(&fat32_ops);
    static char a[2048], b[700];
    for (int i = 0; i < 2048; i++) a[i] = (char)(i * 7);
    for (int i = 0; i < 700; i++) b[i] = (char)('k' + i % 9);
    char p1[FS_PATH_MAX], p2[FS_PATH_MAX], p3[FS_PATH_MAX], d[FS_PATH_MAX], p4[FS_PATH_MAX];
    k_strlcpy(p1, point(), sizeof p1); k_strlcat(p1, "/first file.bin", sizeof p1);
    k_strlcpy(p2, point(), sizeof p2); k_strlcat(p2, "/renamed.bin", sizeof p2);
    k_strlcpy(p3, point(), sizeof p3); k_strlcat(p3, "/gone.txt", sizeof p3);
    k_strlcpy(d, point(), sizeof d);   k_strlcat(d, "/Folder", sizeof d);
    k_strlcpy(p4, d, sizeof p4);       k_strlcat(p4, "/inner.txt", sizeof p4);
    int ok = up && fs_touch_deferred(p1) && fs_write_range_deferred(p1, 0, a, sizeof a);
    if (up) fs_settle_path(p1);
    ok = ok && fs_touch_deferred(p3) && fs_write_range_deferred(p3, 0, b, sizeof b);
    if (up) fs_settle_path(p3);
    ok = ok && fs_rename(p1, p2) && fs_delete(p3) && fs_mkdir(d) &&
         fs_touch_deferred(p4) && fs_write_range_deferred(p4, 0, b, sizeof b);
    if (up) fs_settle_path(p4);
    k_memcpy(g_snap, g_disk, YK_SECTORS * 512u);   // pulled
    if (up) yank_down();

    int mounted = 0, renamed = 0, first_gone = 0, deleted = 0, inner = 0, clean = 0;
    struct fs_scratch sc;
    if (mount_scratch_begin(&fat32_ops, &sc)) {
        if (fat32_ops.init(sc.st, &SNAP_DEV, 0) == 1) {
            mounted = 1;
            renamed = snap_fat_file(sc.st, "/renamed.bin", a, sizeof a);
            first_gone = !fat32_ops.exists(sc.st, "/first file.bin");
            deleted = !fat32_ops.exists(sc.st, "/gone.txt");
            inner = snap_fat_file(sc.st, "/Folder/inner.txt", b, sizeof b);
            static struct fs_check_result res;
            k_memset(&res, 0, sizeof res);
            clean = fat32_ops.check(sc.st, 0, &res, 0) == 1 && res.leaked == 0 &&
                    res.referenced_but_free == 0 && res.double_allocated == 0 &&
                    res.out_of_range == 0 && res.dir_errors == 0;
            if (fat32_ops.umount) fat32_ops.umount(sc.st, &SNAP_DEV);
        }
        mount_scratch_end(&sc);
    }
    KTEST_ASSERT(up);
    KTEST_ASSERT(ok);
    KTEST_ASSERT(mounted);
    KTEST_ASSERT(renamed);
    KTEST_ASSERT(first_gone);
    KTEST_ASSERT(deleted);
    KTEST_ASSERT(inner);
    KTEST_ASSERT(clean);
}
