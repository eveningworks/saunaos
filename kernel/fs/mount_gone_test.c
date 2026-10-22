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

static int yk_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    (void)self;
    if (g_unplugged || !g_disk || count <= 0 || lba + (uint64_t)count > YK_SECTORS) return 0;
    k_memcpy(g_disk + lba * 512, buf, (uint32_t)count * 512);
    return 1;
}

static int yk_xfer(const struct block_device *self) { (void)self; return 64; }
static int yk_flush(const struct block_device *self) { (void)self; return !g_unplugged; }
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
