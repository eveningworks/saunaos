// Tests for the TFS3 backend driven DIRECTLY, over RAM volumes of their
// own -- the fat32_test.c shape. fs_test.c covers TFS3 through fs_* on
// the real root; what it cannot build is TWO TFS3 volumes side by side,
// which is what these need.
//
// Each volume is 4 MiB: TFS3 refuses anything smaller than one partial
// group (T3_MIN_GROUP_BLOCKS past group0), and this is the next round
// size above it.
#include "ktest.h"
#include "tfs3.h"
#include "fs_ident.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "mount.h"  // mount_scratch_begin/end, mount_add -- each volume owns its state
#include "fs.h"     // fs_sync_path(), fs_lock_held_at()
#include "block_stat.h" // blk_stat_get() -- the device reads a create costs
#include "kfmt.h"   // k_snprintf, klog_printf
#include "tfs3_internal.h"  // the ordering test reads the volume's layout
#include "storage_config.h" // ...and skips under `lazy`
#include "scheduler.h"   // the slow device sleeps in its flush
#include "clocksource.h"
#include "timer.h"
#include "barrier.h"   // cpu_relax()

#define VOL_SECTORS 8192u           // 4 MiB
#define VOL_BYTES   (VOL_SECTORS * 512u)

static uint8_t *g_vol[2];

static uint64_t vol_count(const struct block_device *self) { (void)self; return VOL_SECTORS; }

static int vol_rw(int v, uint64_t lba, int count, void *rd, const void *wr) {
    if (!g_vol[v] || count <= 0) return 0;
    if ((uint64_t)lba + (uint64_t)count > VOL_SECTORS) return 0;
    uint8_t *at = g_vol[v] + (uint64_t)lba * 512;
    if (rd) k_memcpy(rd, at, (uint32_t)count * 512);
    else k_memcpy(at, wr, (uint32_t)count * 512);
    return 1;
}
static int v0_read(const struct block_device *self, uint64_t lba, int n, void *b) { (void)self; return vol_rw(0, lba, n, b, 0); }
static int v0_write(const struct block_device *self, uint64_t lba, int n, const void *b) { (void)self; return vol_rw(0, lba, n, 0, b); }
static int v1_read(const struct block_device *self, uint64_t lba, int n, void *b) { (void)self; return vol_rw(1, lba, n, b, 0); }
static int v1_write(const struct block_device *self, uint64_t lba, int n, const void *b) { (void)self; return vol_rw(1, lba, n, 0, b); }
static int vol_xfer(const struct block_device *self) { (void)self; return 8; }

static const struct block_device VOL_DEV[2] = {
    { .name = "t3test0", .sector_count = vol_count, .read_sectors = v0_read,
      .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer },
    { .name = "t3test1", .sector_count = vol_count, .read_sectors = v1_read,
      .write_sectors = v1_write, .max_sectors_per_xfer = vol_xfer },
};

static const struct fs_ops *T(void) { return &tfs3_ops; }

// Block `i` of a file on volume `v` says which volume and which block it
// is, in every byte -- so a read served from the other volume's
// metadata cannot come back looking right.
static void fill(uint8_t *b, int v, int i) {
    for (int k = 0; k < 4096; k++) b[k] = (uint8_t)(0x40 + v * 0x20 + i);
}

// A POINTER TABLE AT THE SAME BLOCK NUMBER ON TWO VOLUMES, WITH DIFFERENT
// CONTENTS. Both files take blocks 0-11 directly, so the single-indirect
// table lands on the same block number on both; volume 0's table maps
// index 12, volume 1's leaves 12 a hole and maps 13. Reading volume 0's
// block 12 loads its table into the read-side pointer cache; reading
// volume 1's block 13 straight after, with no write between to drop the
// cache, must use volume 1's table. When that cache was file-scope it
// used volume 0's -- where index 13 is a hole -- and returned zeros.
KTEST("tfs3", "a pointer table cached for one volume is not served to another") {
    struct fs_scratch sc[2];
    int have[2] = {0, 0};
    for (int v = 0; v < 2; v++) {
        if (!g_vol[v]) g_vol[v] = kmalloc(VOL_BYTES);
        if (!g_vol[v]) break;
        k_memset(g_vol[v], 0, VOL_BYTES);
        if (!mount_scratch_begin(T(), &sc[v])) break;
        have[v] = 1;
    }
    if (!have[0] || !have[1]) {
        for (int v = 0; v < 2; v++) if (have[v]) mount_scratch_end(&sc[v]);
        KTEST_SKIP("could not allocate two 4 MiB volumes");
    }

    static uint8_t blk[4096], back[4096];
    int ok = 1;
    for (int v = 0; v < 2 && ok; v++) {
        void *st = sc[v].st;
        ok = T()->format(st, &VOL_DEV[v]) && T()->init(st, &VOL_DEV[v], 0) >= 0;
        for (int i = 0; ok && i < 12; i++) {
            fill(blk, v, i);
            ok = T()->write_range(st, "/f", (uint64_t)i * 4096, blk, 4096);
        }
        int last = v == 0 ? 12 : 13;           // volume 1 leaves 12 a hole
        fill(blk, v, last);
        if (ok) ok = T()->write_range(st, "/f", (uint64_t)last * 4096, blk, 4096);
    }
    KTEST_ASSERT(ok);

    uint32_t got0 = T()->read_range(sc[0].st, "/f", 12u * 4096, back, 4096);
    fill(blk, 0, 12);
    int same0 = got0 == 4096 && k_memcmp(back, blk, 4096) == 0;

    uint32_t got1 = T()->read_range(sc[1].st, "/f", 13u * 4096, back, 4096);
    fill(blk, 1, 13);
    int same1 = got1 == 4096 && k_memcmp(back, blk, 4096) == 0;
    uint8_t first1 = back[0];

    for (int v = 1; v >= 0; v--) mount_scratch_end(&sc[v]);
    for (int v = 0; v < 2; v++) { kfree(g_vol[v]); g_vol[v] = 0; }

    KTEST_ASSERT(same0);
    KTEST_ASSERT_EQ((int)first1, 0x40 + 0x20 + 13);   // not 0: the hole in volume 0's table
    KTEST_ASSERT(same1);
}

// ---- the flush happens under the mount's lock ----------------------

// A MOUNTED RAM volume whose flush() records whether its mount's lock is
// held at that moment. fs_exclusive_begin() promises its holder that
// nothing is at the disk, and the legacy `run` spins on any lock it then
// meets -- so a flush issued after the mount lock is released (fsync did
// this) is a hang waiting for a sleeping flusher. Both entry points are
// asked, because they took different routes to the device.
#define FLUSH_POINT "/var/tmp/.ktest_flushmnt"

static int g_flush_watch, g_flush_calls, g_flush_locked;

static int v0_flush(const struct block_device *self) { (void)self;
    if (g_flush_watch) {
        g_flush_calls++;
        if (fs_lock_held_at(FLUSH_POINT "/f")) g_flush_locked++;
    }
    return 1;
}

static const struct block_device FLUSH_DEV = {
    .name = "t3flush", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_FLUSH, .flush = v0_flush,
};

KTEST("tfs3", "fsync and sync flush the device while its mount is locked") {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) KTEST_SKIP("could not allocate a 4 MiB volume");
    k_memset(g_vol[0], 0, VOL_BYTES);

    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, &FLUSH_DEV);
        mount_scratch_end(&sc);
    }
    // What format() wrote is what the signature recogniser calls tfs3.
    struct fs_ident id;
    KTEST_ASSERT(!ok || (fs_ident(&FLUSH_DEV, &id) && !k_strcmp(id.type, "tfs3")));
    fs_mkdir(FLUSH_POINT);
    const char *why = 0;
    int mounted = ok && mount_add(&FLUSH_DEV, "tfs3", FLUSH_POINT, 0, 0, &why);
    int wrote = mounted && fs_write(FLUSH_POINT "/f", "x", 0);

    g_flush_calls = g_flush_locked = 0;
    g_flush_watch = 1;
    int fsynced = wrote && fs_sync_path(FLUSH_POINT "/f");
    int fsync_calls = g_flush_calls, fsync_locked = g_flush_locked;
    g_flush_calls = g_flush_locked = 0;
    int synced = wrote && fs_sync(0);
    int sync_calls = g_flush_calls, sync_locked = g_flush_locked;
    g_flush_watch = 0;

    if (mounted) mount_remove(FLUSH_POINT, &why);
    fs_delete(FLUSH_POINT);
    kfree(g_vol[0]);
    g_vol[0] = 0;

    KTEST_ASSERT(mounted);
    KTEST_ASSERT(fsynced && synced);
    KTEST_ASSERT(fsync_calls >= 1);                 // the fixture reached the device
    KTEST_ASSERT_EQ(fsync_locked, fsync_calls);     // ...and every flush was locked
    KTEST_ASSERT(sync_calls >= 1);
    KTEST_ASSERT_EQ(sync_locked, sync_calls);
}

// ---- a data read drops the mount's lock, and only where it may -------

// fslock stage 3: a whole-block run is read with the mount's lock
// DROPPED (tfs3.c's vol_read_run()), so the rest of the volume is not
// held up for the transfer. This asserts both halves from the device's
// side: an ordinary read reaches it UNLOCKED at least once, with the
// gap counted; the same read under fs_exclusive_begin() never does --
// exclusion promises its holder that nothing is at the disk, and a gap
// opened under it is the hang 6fc7b6b5 fixed for flushes.
#define GAP_POINT "/var/tmp/.ktest_gapmnt"

static int g_gap_watch, g_gap_reads, g_gap_unlocked, g_gap_counted;

static int gap_read(const struct block_device *self, uint64_t lba, int n, void *b) { (void)self;
    if (g_gap_watch) {
        g_gap_reads++;
        if (!fs_lock_held_at(GAP_POINT "/f")) {
            g_gap_unlocked++;
            const struct mount *m = mount_resolve(GAP_POINT "/f", 0);
            if (m && __atomic_load_n(&m->io_gaps, __ATOMIC_ACQUIRE) > 0) g_gap_counted++;
        }
    }
    return vol_rw(0, lba, n, b, 0);
}

static const struct block_device GAP_DEV = {
    .name = "t3gap", .sector_count = vol_count, .read_sectors = gap_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
};

KTEST("tfs3", "a whole-block read drops the mount's lock, and not under exclusion") {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) KTEST_SKIP("could not allocate a 4 MiB volume");
    k_memset(g_vol[0], 0, VOL_BYTES);

    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, &GAP_DEV);
        mount_scratch_end(&sc);
    }
    fs_mkdir(GAP_POINT);
    const char *why = 0;
    int mounted = ok && mount_add(&GAP_DEV, "tfs3", GAP_POINT, 0, 0, &why);
    static uint8_t data[64 * 1024], back[64 * 1024];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 13u + 5u);
    int wrote = mounted && fs_write_range(GAP_POINT "/f", 0, data, sizeof data);

    g_gap_reads = g_gap_unlocked = g_gap_counted = 0;
    g_gap_watch = 1;
    uint32_t got = wrote ? fs_read_range(GAP_POINT "/f", 0, back, sizeof back) : 0;
    g_gap_watch = 0;
    int plain_reads = g_gap_reads, plain_unlocked = g_gap_unlocked, plain_counted = g_gap_counted;
    int same = got == sizeof back && k_memcmp(back, data, sizeof back) == 0;

    g_gap_reads = g_gap_unlocked = 0;
    fs_exclusive_begin();
    g_gap_watch = 1;
    uint32_t got_x = wrote ? fs_read_range(GAP_POINT "/f", 0, back, sizeof back) : 0;
    g_gap_watch = 0;
    fs_exclusive_end();
    int excl_unlocked = g_gap_unlocked;

    const struct mount *m = mount_resolve(GAP_POINT "/f", 0);
    int gaps_after = m ? __atomic_load_n(&m->io_gaps, __ATOMIC_ACQUIRE) : -1;

    if (mounted) mount_remove(GAP_POINT, &why);
    fs_delete(GAP_POINT);
    kfree(g_vol[0]);
    g_vol[0] = 0;

    KTEST_ASSERT(mounted && wrote);
    KTEST_ASSERT(same);                              // the unlocked read is still right
    KTEST_ASSERT(plain_reads >= 1);                  // the fixture reached the device
    KTEST_ASSERT(plain_unlocked >= 1);               // ...without the lock
    KTEST_ASSERT_EQ(plain_counted, plain_unlocked);  // ...and every such read counted a gap
    KTEST_ASSERT_EQ((int)got_x, (int)sizeof back);
    KTEST_ASSERT_EQ(excl_unlocked, 0);               // never under exclusion
    KTEST_ASSERT_EQ(gaps_after, 0);                  // nothing left open
}

// ---- a delete reads its pointer tables with the lock dropped ----------

// free_all_blocks() reads a dead file's tables in the lock's gaps, so a
// large delete no longer holds the volume for every table read. Asserted
// from the device: the table read arrives unlocked; ANOTHER OP RUN IN
// THAT GAP (a write, from the read's own callback) completes and reads
// back intact on a disk whose TRIM really zeroes -- a discard queued
// before the gap and sent after it would have wiped blocks that write
// reused; and every block of both files comes back.
#define DEL_POINT "/var/tmp/.ktest_delmnt"

static int g_del_watch, g_del_unlocked, g_del_nested, g_del_nested_ok;
static uint8_t g_del_other[16 * 1024];

static int del_read(const struct block_device *self, uint64_t lba, int n, void *b) { (void)self;
    int r = vol_rw(0, lba, n, b, 0);
    if (g_del_watch && !fs_lock_held_at(DEL_POINT "/f")) {
        g_del_unlocked++;
        if (!g_del_nested) {
            g_del_nested = 1;
            g_del_watch = 0;
            g_del_nested_ok = fs_write_range(DEL_POINT "/g", 0, g_del_other, sizeof g_del_other);
            g_del_watch = 1;
        }
    }
    return r;
}

static int del_trim(const struct block_device *self, uint64_t lba, uint32_t count) { (void)self;   // a discard really loses the data
    if ((uint64_t)lba + count > VOL_SECTORS) return 0;
    k_memset(g_vol[0] + (uint64_t)lba * 512, 0, (size_t)count * 512);
    return 1;
}

static const struct block_device DEL_DEV = {
    .name = "t3del", .sector_count = vol_count, .read_sectors = del_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_TRIM, .trim = del_trim,
};

KTEST("tfs3", "a delete reads its tables unlocked, and a write in that gap survives") {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) KTEST_SKIP("could not allocate a 4 MiB volume");
    k_memset(g_vol[0], 0, VOL_BYTES);

    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, &DEL_DEV);
        mount_scratch_end(&sc);
    }
    fs_mkdir(DEL_POINT);
    const char *why = 0;
    int mounted = ok && mount_add(&DEL_DEV, "tfs3", DEL_POINT, 0, 0, &why);
    const struct mount *m = mounted ? mount_resolve(DEL_POINT "/f", 0) : 0;
    uint64_t used0 = 0, used1 = 0, total = 0;
    int have0 = m && fs_mount_usage(m, &used0, &total);

    // 64 blocks: twelve direct and a single-indirect table to read.
    static uint8_t data[256 * 1024], back[sizeof g_del_other];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7u + 3u);
    for (unsigned i = 0; i < sizeof g_del_other; i++) g_del_other[i] = (uint8_t)(i * 29u + 11u);
    int wrote = have0 && fs_write_range(DEL_POINT "/f", 0, data, sizeof data);

    g_del_unlocked = g_del_nested = g_del_nested_ok = 0;
    g_del_watch = 1;
    int deleted = wrote && fs_delete(DEL_POINT "/f");
    g_del_watch = 0;

    uint32_t got = g_del_nested_ok ? fs_read_range(DEL_POINT "/g", 0, back, sizeof back) : 0;
    int intact = got == sizeof back && k_memcmp(back, g_del_other, sizeof back) == 0;
    int deleted_g = g_del_nested_ok && fs_delete(DEL_POINT "/g");
    int have1 = m && fs_mount_usage(m, &used1, &total);
    int gaps_after = m ? __atomic_load_n(&m->io_gaps, __ATOMIC_ACQUIRE) : -1;

    if (mounted) mount_remove(DEL_POINT, &why);
    fs_delete(DEL_POINT);
    kfree(g_vol[0]);
    g_vol[0] = 0;

    KTEST_ASSERT(mounted && have0 && wrote && deleted);
    KTEST_ASSERT(g_del_unlocked >= 1);          // the table read had the lock dropped
    KTEST_ASSERT(g_del_nested && g_del_nested_ok); // ...and another op ran in that gap
    KTEST_ASSERT(intact);                        // ...whose data no late discard reached
    KTEST_ASSERT(deleted_g && have1);
    KTEST_ASSERT_EQ(used1, used0);               // every block of both files came back
    KTEST_ASSERT_EQ(gaps_after, 0);
}

// ---- the directory inode cache follows every change -----------------

// t3_read_inode() caches DIRECTORY inodes. A stale entry is silent and
// serious: a cached size one block short makes a listing stop early and
// a lookup miss names that exist. So the cache is driven through the
// changes a directory's inode sees -- its link count (a subdirectory
// made and removed) and its SIZE (enough entries to need a second
// block) -- and each is read straight back. Then the cost it exists
// for: creates in a warm directory, counted in device reads.
#define IC_POINT "/var/tmp/.ktest_icmnt"

static int g_ic_listed;
static void ic_count(void *ctx, const char *name, uint32_t size, int is_dir) {
    (void)ctx; (void)name; (void)size; (void)is_dir;
    g_ic_listed++;
}

static const struct block_device IC_DEV = {
    .name = "t3ic", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
};

KTEST("tfs3", "a cached directory inode follows its link count and its size") {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) KTEST_SKIP("could not allocate a 4 MiB volume");
    k_memset(g_vol[0], 0, VOL_BYTES);
    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, &IC_DEV);
        mount_scratch_end(&sc);
    }
    fs_mkdir(IC_POINT);
    const char *why = 0;
    int mounted = ok && mount_add(&IC_DEV, "tfs3", IC_POINT, 0, 0, &why);
    int made = mounted && fs_mkdir(IC_POINT "/d");

    // Links: read (and so cache) the parent, change it, read it again.
    struct fs_stat_info st;
    int l0 = made && fs_stat(IC_POINT "/d", &st) ? st.nlink : -1;
    int sub = made && fs_mkdir(IC_POINT "/d/sub");
    int l1 = sub && fs_stat(IC_POINT "/d", &st) ? st.nlink : -1;
    int gone = sub && fs_delete(IC_POINT "/d/sub");
    int l2 = gone && fs_stat(IC_POINT "/d", &st) ? st.nlink : -1;

    // Size: enough names that the directory needs a second block, listed
    // after every create has been through the cached parent.
    enum { NAMES = 400 };
    char path[64];
    int touched = 0;
    for (int i = 0; made && i < NAMES; i++) {
        k_snprintf(path, sizeof path, IC_POINT "/d/entry_%03d", i);
        touched += fs_touch(path);
    }
    g_ic_listed = 0;
    fs_list(IC_POINT "/d", ic_count, 0);
    int listed = g_ic_listed;

    // Cost: creates in a warm directory, in device reads.
    enum { WARM = 32 };
    uint64_t r0 = 0, r1 = 0;
    blk_stat_get(BLK_STAT_READ, &r0, 0, 0, 0);
    int warm = 0;
    for (int i = 0; made && i < WARM; i++) {
        k_snprintf(path, sizeof path, IC_POINT "/w_%02d", i);
        warm += fs_touch(path);
    }
    blk_stat_get(BLK_STAT_READ, &r1, 0, 0, 0);
    klog_printf("ktest: %d warm creates read %lu blocks\n", warm, (unsigned long)(r1 - r0));

    if (mounted) mount_remove(IC_POINT, &why);
    fs_delete(IC_POINT);
    kfree(g_vol[0]);
    g_vol[0] = 0;

    KTEST_ASSERT(mounted && made);
    KTEST_ASSERT(sub && gone);
    KTEST_ASSERT_EQ(l1, l0 + 1);                 // the cached parent saw the subdirectory
    KTEST_ASSERT_EQ(l2, l0);                     // ...and its removal
    KTEST_ASSERT_EQ(touched, NAMES);
    KTEST_ASSERT_EQ(listed, NAMES);              // no name hidden past a stale size
    KTEST_ASSERT_EQ(warm, WARM);
    KTEST_ASSERT((r1 - r0) <= 5u * WARM);        // 6 per create before (cache, no flush)
}

// ---- an overwrite drops the lock; an allocating write does not -------

// fslock stage 3b: rewriting blocks a file already has, inside its size,
// is written with the mount's lock dropped (tfs3.c's do_overwrite()) --
// and ONLY that: a write that allocates or extends builds pointers and
// uses the per-mount rollback log, so it stays locked. Asserted from the
// device: the overwrite arrives unlocked at least once, the append never.
#define OW_POINT "/var/tmp/.ktest_owmnt"

static int g_ow_watch, g_ow_writes, g_ow_unlocked;

static int ow_write(const struct block_device *self, uint64_t lba, int n, const void *b) { (void)self;
    if (g_ow_watch) {
        g_ow_writes++;
        if (!fs_lock_held_at(OW_POINT "/f")) g_ow_unlocked++;
    }
    return vol_rw(0, lba, n, 0, b);
}

static const struct block_device OW_DEV = {
    .name = "t3ow", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = ow_write, .max_sectors_per_xfer = vol_xfer,
};

KTEST("tfs3", "an overwrite drops the mount's lock, an append does not") {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) KTEST_SKIP("could not allocate a 4 MiB volume");
    k_memset(g_vol[0], 0, VOL_BYTES);

    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, &OW_DEV);
        mount_scratch_end(&sc);
    }
    fs_mkdir(OW_POINT);
    const char *why = 0;
    int mounted = ok && mount_add(&OW_DEV, "tfs3", OW_POINT, 0, 0, &why);
    static uint8_t a[64 * 1024], b[64 * 1024], back[68 * 1024];
    for (unsigned i = 0; i < sizeof a; i++) { a[i] = (uint8_t)(i * 7u + 1u); b[i] = (uint8_t)~a[i]; }
    int wrote = mounted && fs_write_range(OW_POINT "/f", 0, a, sizeof a);

    g_ow_writes = g_ow_unlocked = 0;
    g_ow_watch = 1;
    int over = wrote && fs_write_range(OW_POINT "/f", 0, b, sizeof b);   // in place
    g_ow_watch = 0;
    int over_writes = g_ow_writes, over_unlocked = g_ow_unlocked;

    g_ow_writes = g_ow_unlocked = 0;
    g_ow_watch = 1;
    int appended = over && fs_write_range(OW_POINT "/f", sizeof b, a, 4096);  // extends
    g_ow_watch = 0;
    int app_writes = g_ow_writes, app_unlocked = g_ow_unlocked;

    uint64_t size = mounted ? fs_size(OW_POINT "/f") : 0;
    uint32_t got = appended ? fs_read_range(OW_POINT "/f", 0, back, sizeof back) : 0;
    int same = got == sizeof b + 4096 && k_memcmp(back, b, sizeof b) == 0 &&
               k_memcmp(back + sizeof b, a, 4096) == 0;

    if (mounted) mount_remove(OW_POINT, &why);
    fs_delete(OW_POINT);
    kfree(g_vol[0]);
    g_vol[0] = 0;

    KTEST_ASSERT(mounted && wrote && over && appended);
    KTEST_ASSERT(over_writes >= 1);          // the overwrite reached the device
    KTEST_ASSERT(over_unlocked >= 1);        // ...without the lock
    KTEST_ASSERT(app_writes >= 1);
    KTEST_ASSERT_EQ(app_unlocked, 0);        // the append never
    KTEST_ASSERT_EQ((int)size, (int)(sizeof b + 4096));
    KTEST_ASSERT(same);
}

// ---- inode locks (fslock stage 4) ------------------------------------

// A mounted RAM volume for the tests below, with a device whose read and
// write callbacks can report how the file under test is locked -- they
// run in the middle of an op's unlocked gap.
#define L4_POINT "/var/tmp/.ktest_l4mnt"
static int g_l4_watch, g_l4_rd_mode = -1, g_l4_wr_mode = -1;
static uint64_t g_l4_ino;
static void *l4_state(void) {
    const struct mount *m = mount_resolve(L4_POINT "/x", 0);
    return m ? m->state : 0;
}
static int l4_read(const struct block_device *self, uint64_t lba, int n, void *b) { (void)self;
    if (g_l4_watch && !fs_lock_held_at(L4_POINT "/x"))
        g_l4_rd_mode = tfs3_test_lock_mode(l4_state(), g_l4_ino);
    return vol_rw(0, lba, n, b, 0);
}
static int l4_write(const struct block_device *self, uint64_t lba, int n, const void *b) { (void)self;
    if (g_l4_watch && !fs_lock_held_at(L4_POINT "/x"))
        g_l4_wr_mode = tfs3_test_lock_mode(l4_state(), g_l4_ino);
    return vol_rw(0, lba, n, 0, b);
}
static const struct block_device L4_DEV = {
    .name = "t3l4", .sector_count = vol_count, .read_sectors = l4_read,
    .write_sectors = l4_write, .max_sectors_per_xfer = vol_xfer,
};

static int l4_mount(void) {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) return 0;
    k_memset(g_vol[0], 0, VOL_BYTES);
    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, &L4_DEV);
        mount_scratch_end(&sc);
    }
    fs_mkdir(L4_POINT);
    const char *why = 0;
    return ok && mount_add(&L4_DEV, "tfs3", L4_POINT, 0, 0, &why);
}
static void l4_unmount(void) {
    const char *why = 0;
    mount_remove(L4_POINT, &why);
    fs_delete(L4_POINT);
    kfree(g_vol[0]);
    g_vol[0] = 0;
}
static int l4_stream(const char *path, uint64_t off, const void *buf, uint32_t len, int steps) {
    void *h = fs_write_range_begin(path, off, buf, len);
    if (!h) return -1;
    enum fs_step_result r = FS_STEP_PENDING;
    for (int i = 0; (steps < 0 || i < steps) && r == FS_STEP_PENDING; i++) r = fs_write_range_step(h);
    return (int)r;
}

KTEST("tfs3", "an unlocked read holds its file shared, an overwrite exclusive, and both let go") {
    if (!l4_mount()) KTEST_SKIP("could not mount a 4 MiB volume");
    static uint8_t buf[64 * 1024];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 5u + 3u);
    int wrote = fs_write_range(L4_POINT "/x", 0, buf, sizeof buf);
    struct fs_stat_info si;
    int statted = fs_stat(L4_POINT "/x", &si);
    g_l4_ino = si.ino;

    g_l4_rd_mode = g_l4_wr_mode = -1;
    g_l4_watch = 1;
    uint32_t got = fs_read_range(L4_POINT "/x", 0, buf, sizeof buf);   // gap: read
    int over = fs_write_range(L4_POINT "/x", 0, buf, sizeof buf);      // gap: overwrite
    g_l4_watch = 0;
    int after = tfs3_test_lock_mode(l4_state(), g_l4_ino);
    l4_unmount();

    KTEST_ASSERT(wrote && statted && got == sizeof buf && over);
    KTEST_ASSERT_EQ(g_l4_rd_mode, 1);   // shared, held across the read's gap
    KTEST_ASSERT_EQ(g_l4_wr_mode, 2);   // exclusive across the overwrite's
    KTEST_ASSERT_EQ(after, 0);          // op_end let go of both
}

// A STREAM SPANS SYSCALLS, and its lock is per step -- so another write
// can land between two steps. The stream must then FAIL, not commit its
// stale inode copy over the other write (its size would drop the append).
KTEST("tfs3", "a stepped write fails, rather than undo, a write made between its steps") {
    if (!l4_mount()) KTEST_SKIP("could not mount a 4 MiB volume");
    static uint8_t a[8192], b[4096];
    k_memset(a, 'a', sizeof a);
    k_memset(b, 'b', sizeof b);
    int base = fs_write_range(L4_POINT "/x", 0, a, 4096);
    void *h = fs_write_range_begin(L4_POINT "/x", 0, a, sizeof a);
    enum fs_step_result first = h ? fs_write_range_step(h) : FS_STEP_FAILED;
    int appended = fs_write_range(L4_POINT "/x", sizeof a, b, sizeof b);   // between steps
    enum fs_step_result last = FS_STEP_PENDING;
    while (h && last == FS_STEP_PENDING) last = fs_write_range_step(h);
    uint64_t size = fs_size(L4_POINT "/x");
    l4_unmount();

    KTEST_ASSERT(base && h && appended);
    KTEST_ASSERT_EQ((int)first, (int)FS_STEP_PENDING);
    KTEST_ASSERT_EQ((int)last, (int)FS_STEP_FAILED);    // noticed, not overwritten
    KTEST_ASSERT_EQ((int)size, (int)(sizeof a + sizeof b));   // the append survived
}

// A stream builds its block pointers in the shared pointer-table cache,
// and another stream's begin() drops that cache. So every PENDING step
// lands it first -- or blocks written before the other stream began
// read back as holes.
KTEST("tfs3", "a stepped write's pointers survive another stream between its steps") {
    if (!l4_mount()) KTEST_SKIP("could not mount a 4 MiB volume");
    static uint8_t a[16 * 4096], back[16 * 4096], g[4096];
    for (unsigned i = 0; i < sizeof a; i++) a[i] = (uint8_t)(i / 4096 + 1);   // block k holds k+1
    k_memset(g, 'g', sizeof g);
    void *h = fs_write_range_begin(L4_POINT "/x", 0, a, sizeof a);
    int steps = 0;
    enum fs_step_result r = FS_STEP_PENDING;
    while (h && r == FS_STEP_PENDING && steps < 14) { r = fs_write_range_step(h); steps++; }
    int other = l4_stream(L4_POINT "/y", 0, g, sizeof g, -1);       // begin() drops the cache
    while (h && r == FS_STEP_PENDING) r = fs_write_range_step(h);
    uint32_t got = fs_read_range(L4_POINT "/x", 0, back, sizeof back);
    int holes = 0;
    for (unsigned k = 0; k < 16; k++) if (back[k * 4096] != (uint8_t)(k + 1)) holes++;
    l4_unmount();

    KTEST_ASSERT(h);
    KTEST_ASSERT_EQ(other, (int)FS_STEP_DONE);
    KTEST_ASSERT_EQ((int)r, (int)FS_STEP_DONE);
    KTEST_ASSERT_EQ((int)got, (int)sizeof back);
    KTEST_ASSERT_EQ(holes, 0);
}

// ---- allocation state is on the media before a commit ----------------

// A DEVICE WITH A VOLATILE WRITE CACHE: a write is PENDING until the next
// flush, and a pending write may reach the media after a later one. So at
// the moment a COMMITTED journal header is written, no bitmap or GDT block
// may be pending -- or a crash there replays an inode onto space its
// bitmap calls free, and the allocator hands it out again.
#define ORDER_POINT "/var/tmp/.ktest_ordermnt"
#define ORDER_BLOCKS (VOL_SECTORS / T3_SPB)

static uint8_t g_pend[ORDER_BLOCKS];
static struct t3_state *g_order_sbi;
static int g_order_commits, g_order_alloc_writes, g_order_bad;

static int is_alloc_block(struct t3_state *sbi, uint32_t b) {
    if (b >= sbi->gdt_block && b < sbi->gdt_block + sbi->gdt_blocks) return 1;
    for (uint32_t g = 0; g < sbi->sb.gc; g++)
        if (b == t3_group_base(sbi, g) || b == t3_group_base(sbi, g) + 1) return 1;
    return 0;
}

static int order_write(const struct block_device *self, uint64_t lba, int n, const void *b) {
    struct t3_state *sbi = g_order_sbi;
    const uint8_t *p = b;
    if (sbi && lba == T3_JH_BLOCK * T3_SPB && p[0] == 'J' && p[3] == '3' && p[4]) {
        g_order_commits++;
        for (uint32_t k = 0; k < ORDER_BLOCKS; k++)
            if (g_pend[k] && is_alloc_block(sbi, k)) g_order_bad++;
    }
    for (uint64_t s = lba; s < lba + (uint64_t)n; s++) {
        uint32_t blk = (uint32_t)(s / T3_SPB);
        if (blk >= ORDER_BLOCKS) continue;
        g_pend[blk] = 1;
        if (sbi && s % T3_SPB == 0 && is_alloc_block(sbi, blk)) g_order_alloc_writes++;
    }
    return v0_write(self, lba, n, b);
}

static int order_flush(const struct block_device *self) {
    (void)self;
    k_memset(g_pend, 0, sizeof g_pend);
    return 1;
}

static const struct block_device ORDER_DEV = {
    .name = "t3order", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = order_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_FLUSH, .flush = order_flush,
};

// One pass of the fixture in one commit mode: a file, a directory, a file
// in it, and a write that allocates, then a sync.
static int order_pass(int batched, int *commits, int *alloc_writes, int *bad) {
    k_memset(g_vol[0], 0, VOL_BYTES);
    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, &ORDER_DEV);
        mount_scratch_end(&sc);
    }
    fs_mkdir(ORDER_POINT);
    const char *why = 0;
    int mounted = ok && mount_add(&ORDER_DEV, "tfs3", ORDER_POINT, 0, 0, &why);
    const struct mount *m = mounted ? mount_resolve(ORDER_POINT "/x", 0) : 0;
    g_order_sbi = m ? m->state : 0;
    k_memset(g_pend, 0, sizeof g_pend);
    g_order_commits = g_order_alloc_writes = g_order_bad = 0;

    int was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, batched);
    int did = g_order_sbi && fs_write(ORDER_POINT "/a", "one", 0) &&
              fs_mkdir(ORDER_POINT "/d") && fs_write(ORDER_POINT "/d/b", "two", 0) &&
              fs_write(ORDER_POINT "/a", "more", 1) && fs_sync(0);
    storage_config_set_mode_for_test(1, was_batched);
    *commits = g_order_commits;
    *alloc_writes = g_order_alloc_writes;
    *bad = g_order_bad;
    g_order_sbi = 0;

    if (mounted) mount_remove(ORDER_POINT, &why);
    fs_delete(ORDER_POINT);
    return did;
}

// BOTH MODES, because they reach the header differently: `strict` commits
// per op, while under `batched` every op stops and the sync's ONE commit
// has to carry all of their bitmaps out first (t3_txn_commit()).
KTEST("tfs3", "a commit flushes the bitmaps it depends on before its journal header") {
    if (!storage_sync_strict()) KTEST_SKIP("storage.sync = lazy issues no barriers");
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) KTEST_SKIP("could not allocate a 4 MiB volume");

    int sc = 0, sa = 0, sb = 0, bc = 0, ba = 0, bb = 0;
    int strict_did = order_pass(0, &sc, &sa, &sb);
    int batched_did = order_pass(1, &bc, &ba, &bb);
    kfree(g_vol[0]);
    g_vol[0] = 0;

    KTEST_ASSERT(strict_did);
    KTEST_ASSERT(sc >= 3);        // the fixture reached the journal...
    KTEST_ASSERT(sa >= 3);        // ...and the bitmaps
    KTEST_ASSERT_EQ(sb, 0);
    KTEST_ASSERT(batched_did);
    KTEST_ASSERT(bc >= 1);
    KTEST_ASSERT(ba >= 1);
    KTEST_ASSERT_EQ(bb, 0);
}

// ---- handles on the running transaction (fslock stage 5b) -----------
//
// On a RAM volume of their own: nothing else writes to it, and a pattern
// staged by mistake can never reach a real disk.
#define JOIN_POINT "/var/tmp/.ktest_joinmnt"

static struct t3_state *join_mount_on(const struct block_device *dev) {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) return 0;
    k_memset(g_vol[0], 0, VOL_BYTES);
    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, dev);
        mount_scratch_end(&sc);
    }
    fs_mkdir(JOIN_POINT);
    const char *why = 0;
    if (!ok || !mount_add(dev, "tfs3", JOIN_POINT, 0, 0, &why)) return 0;
    const struct mount *m = mount_resolve(JOIN_POINT "/x", 0);
    return m ? m->state : 0;
}

static struct t3_state *join_mount(void) { return join_mount_on(&VOL_DEV[0]); }

static void join_unmount(void) {
    const char *why = 0;
    mount_remove(JOIN_POINT, &why);
    fs_delete(JOIN_POINT);
    kfree(g_vol[0]);
    g_vol[0] = 0;
}

// Under `batched` a create on ANY disk joins the writes staged before it
// instead of committing them and then itself -- two commits, four
// barriers -- and one later commit lands them all. The commit count is
// the journal's sequence number. In a fresh directory, so no full block
// makes the create grow it (a grow commits).
KTEST("tfs3", "under batched a create joins the staged writes and commits with them") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    static char buf[4096];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (char)(i * 11u + 3u);
    const char *a = JOIN_POINT "/d/a", *b = JOIN_POINT "/d/b";
    int wrote = fs_mkdir(JOIN_POINT "/d") && fs_write_range(a, 0, buf, sizeof buf) &&
                fs_sync_path(JOIN_POINT);

    fs_exclusive_begin();   // nothing commits while it is held
    uint32_t seq0 = sbi->jrn_seq;
    int again = fs_write_range(a, sizeof buf, buf, sizeof buf);   // staged, not committed
    int made = fs_write_range(b, 0, buf, 100);                    // a create, then a write
    uint32_t seq1 = sbi->jrn_seq;
    int deferred = sbi->txn_deferred;
    int seen = fs_size(a) == 2 * sizeof buf && fs_size(b) == 100;
    int synced = fs_sync_path(JOIN_POINT);
    uint32_t seq2 = sbi->jrn_seq;
    fs_exclusive_end();
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote);
    KTEST_ASSERT(again == 1);
    KTEST_ASSERT(made == 1);
    KTEST_ASSERT_EQ((int64_t)(seq1 - seq0), 0);   // the create committed nothing
    KTEST_ASSERT(deferred);
    KTEST_ASSERT(seen);                            // ...and both read back before the commit
    KTEST_ASSERT(synced);
    KTEST_ASSERT_EQ((int64_t)(seq2 - seq1), 1);   // one commit for all three
}

// A failed handle takes back only its own work: a block an EARLIER op
// staged comes back from the undo copy, and that op's work stays staged.
KTEST("tfs3", "a handle that fails puts back the block an earlier op staged") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    uint32_t last = (uint32_t)(sbi->vol.sector_count / T3_SPB) - 1;
    int clean = 0, first = 0, second = 0, kept = 0, restored = 0, undone = 0;
    fs_exclusive_begin();
    if (t3_txn_flush_deferred(sbi) && sbi->txn_count == 0) {
        clean = 1;
        uint8_t *img = t3_txn_begin(sbi, 1) ? t3_txn_stage(sbi, last) : 0;
        if (img) {
            first = 1;
            for (uint32_t k = 0; k < T3_BLOCK; k++) img[k] = (uint8_t)(k * 3 + 1);
            t3_txn_stop(sbi);
            uint8_t *again = t3_txn_begin(sbi, 2) ? t3_txn_stage(sbi, last) : 0;
            uint8_t *other = again ? t3_txn_stage(sbi, last - 1) : 0;
            if (again == img && other) {
                second = 1;
                for (uint32_t k = 0; k < T3_BLOCK; k++) again[k] = 0xEE;
                t3_txn_reset(sbi);
                kept = sbi->txn_count == 1 && sbi->txn_deferred && sbi->txn_target[0] == last;
                restored = 1;
                for (uint32_t k = 0; k < T3_BLOCK; k++) restored &= img[k] == (uint8_t)(k * 3 + 1);
                undone = sbi->txn_undo[0] == 0 && !sbi->txn_hopen;
            }
        }
        t3_txn_discard(sbi);   // never committed
    }
    fs_exclusive_end();
    join_unmount();

    KTEST_ASSERT(clean);
    KTEST_ASSERT(first);
    KTEST_ASSERT(second);
    KTEST_ASSERT(kept);
    KTEST_ASSERT(restored);
    KTEST_ASSERT(undone);
}

// fslock stage 5c: the commit thread's flushes run with the volume lock
// DROPPED, so a stat on the same volume answers while a commit is in
// flight. A device whose flush sleeps 50 ms makes the commit long enough
// to be seen; a commit under the lock is never seen in flight at all,
// and the stat would queue behind its three barriers.
#define SLOW_FLUSH_NS 50000000ull
static char g_slow_chan;

static int slow_flush(const struct block_device *self) {
    (void)self;
    uint64_t until = clocksource_now_ns() + SLOW_FLUSH_NS;
    while (clocksource_now_ns() < until)
        if (!scheduler_block_kernel_until(&g_slow_chan, SCHED_WAIT_DISK, until)) cpu_relax();
    return 1;
}

static const struct block_device SLOW_DEV = {
    .name = "t3slow", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_FLUSH, .flush = slow_flush,
};

KTEST("tfs3", "a stat on a volume answers while its commit thread is flushing") {
    if (!t3_commit_thread_running()) KTEST_SKIP("no commit thread on this boot");
    struct t3_state *sbi = join_mount_on(&SLOW_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);   // barriers real, commits deferred
    static char buf[4096], back[4096];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (char)(i * 5u + 7u);
    const char *f = JOIN_POINT "/slow";
    int wrote = fs_write_range(f, 0, buf, sizeof buf);   // staged, not committed

    // Due now, and the thread told.
    sbi->txn_staged_tick = coarse_ticks() - storage_writeback_ticks() - 1;
    t3_commit_kick();
    uint64_t t0 = clocksource_now_ns();
    int in_flight = 0;
    while (clocksource_now_ns() - t0 < 2000000000ull)
        if (__atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) == 1) { in_flight = 1; break; }
    uint64_t s0 = clocksource_now_ns();
    struct fs_stat_info st;
    int stat_ok = in_flight && fs_stat(f, &st) && st.size == sizeof buf;
    uint64_t stat_ns = clocksource_now_ns() - s0;
    int still = __atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) != 0;
    t0 = clocksource_now_ns();
    while (__atomic_load_n(&sbi->txn_deferred, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE))
        if (clocksource_now_ns() - t0 > 2000000000ull) break;
    int landed = !sbi->txn_deferred && !sbi->committing;
    int same = fs_read_range(f, 0, back, sizeof back) == sizeof back &&
               k_memcmp(buf, back, sizeof buf) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote == 1);
    KTEST_ASSERT(in_flight);              // the thread committed in a gap
    KTEST_ASSERT(stat_ok);                // ...the stat answered, with the staged size
    KTEST_ASSERT(stat_ns < 20000000ull);  // well inside one 50 ms flush
    KTEST_ASSERT(still);                  // ...while the commit was still going
    KTEST_ASSERT(landed);
    KTEST_ASSERT(same);
}

// ONE COMMIT IN FLIGHT PER MOUNT: an op's own commit (a delete) arriving
// while the commit thread is in its gap must wait for it -- the journal
// area and txn_io are one. Checked across a REMOUNT, so what is read back
// is what the journal and its targets actually left on the device.
KTEST("tfs3", "a commit that arrives during the thread's commit waits for it") {
    if (!t3_commit_thread_running()) KTEST_SKIP("no commit thread on this boot");
    struct t3_state *sbi = join_mount_on(&SLOW_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    static char buf[8192], back[8192];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (char)(i * 3u + 11u);
    const char *a = JOIN_POINT "/a", *b = JOIN_POINT "/b";
    int setup = fs_write_range(b, 0, buf, 4096) && fs_sync_path(JOIN_POINT) &&
                fs_write_range(a, 0, buf, sizeof buf);   // a: staged, not committed

    sbi->txn_staged_tick = coarse_ticks() - storage_writeback_ticks() - 1;
    t3_commit_kick();
    uint64_t t0 = clocksource_now_ns();
    int in_flight = 0;
    while (clocksource_now_ns() - t0 < 2000000000ull)
        if (__atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) == 1) { in_flight = 1; break; }
    int deleted = in_flight && fs_delete(b);   // its own commit, while the thread's is in flight
    int synced = fs_sync_path(JOIN_POINT);

    const char *why = 0;
    int down = mount_remove(JOIN_POINT, &why);
    int up = down && mount_add(&SLOW_DEV, "tfs3", JOIN_POINT, 0, 0, &why);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = up && fs_check_at(JOIN_POINT, 0, &r) == 0;
    int a_ok = up && fs_size(a) == sizeof buf && fs_read_range(a, 0, back, sizeof back) == sizeof back &&
               k_memcmp(buf, back, sizeof buf) == 0;
    int b_gone = up && !fs_exists(b);
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(setup);
    KTEST_ASSERT(in_flight);
    KTEST_ASSERT(deleted);
    KTEST_ASSERT(synced);
    KTEST_ASSERT(up);
    KTEST_ASSERT(checked);
    KTEST_ASSERT(a_ok);       // the thread's commit landed whole
    KTEST_ASSERT(b_gone);     // ...and so did the delete's
    KTEST_ASSERT_EQ(r.leaked + r.referenced_but_free + r.double_allocated + r.links_wrong +
                    r.bad_names + r.dir_errors, 0);
}

// ---- fsck reports every kind of problem it finds ---------------------

// Three kinds that were only kernel log lines, so a pass that found them
// still printed "clean": a group's free count, a link count, and a name
// whose record fails its checksum.
#define FSCK_POINT "/var/tmp/.ktest_fsckmnt"

KTEST("tfs3", "fsck counts wrong free counts, link counts and damaged names") {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) KTEST_SKIP("could not allocate a 4 MiB volume");
    k_memset(g_vol[0], 0, VOL_BYTES);

    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) {
        ok = T()->format(sc.st, &FLUSH_DEV);
        mount_scratch_end(&sc);
    }
    fs_mkdir(FSCK_POINT);
    const char *why = 0;
    int mounted = ok && mount_add(&FLUSH_DEV, "tfs3", FSCK_POINT, 0, 0, &why);
    int made = mounted && fs_write(FSCK_POINT "/a", "a", 0) && fs_write(FSCK_POINT "/b", "b", 0) &&
               fs_sync(0);
    const struct mount *m = made ? mount_resolve(FSCK_POINT "/a", 0) : 0;
    struct t3_state *sbi = m ? m->state : 0;

    struct fs_check_result clean, found, fixed;
    k_memset(&clean, 0, sizeof clean);
    k_memset(&found, 0, sizeof found);
    k_memset(&fixed, 0, sizeof fixed);
    int checked = sbi && fs_check_at(FSCK_POINT, 0, &clean) == 0;

    int broke = 0;
    if (checked) {
        mount_lock(m);
        uint64_t a = 0, b = 0, lba;
        uint32_t off;
        struct t3_inode node;
        sbi->gd[0].free_blocks++;
        if (t3_resolve(sbi, "/a", &a) && t3_read_inode(sbi, a, &node)) {
            node.links = 3;
            if (t3_txn_begin(sbi, 1) && t3_txn_stage_inode(sbi, a, &node) && t3_txn_commit(sbi)) broke++;
        }
        // A byte of /b's record changed on the media: its checksum fails.
        if (t3_resolve(sbi, "/b", &b) && t3_inode_pos(sbi, b, &lba, &off)) {
            g_vol[0][lba * 512 + off + 4] ^= 0x5a;
            t3_rcache_drop(sbi);
            t3_icache_drop(sbi);
            broke++;
        }
        mount_unlock(m);
    }
    int found_ok = broke == 2 && fs_check_at(FSCK_POINT, 0, &found) == 0;
    int fixed_ok = found_ok && fs_check_at(FSCK_POINT, FSCK_REPAIR, &fixed) == 0;

    if (mounted) mount_remove(FSCK_POINT, &why);
    fs_delete(FSCK_POINT);
    kfree(g_vol[0]);
    g_vol[0] = 0;

    KTEST_ASSERT(made && checked);
    KTEST_ASSERT_EQ(fs_check_problems(&clean), 0u);
    KTEST_ASSERT(found_ok);
    KTEST_ASSERT(found.counts_wrong >= 1);
    KTEST_ASSERT_EQ(found.links_wrong, 1u);
    KTEST_ASSERT_EQ(found.bad_names, 1u);
    KTEST_ASSERT(fs_check_problems(&found) >= 3);
    KTEST_ASSERT(fixed_ok);
    KTEST_ASSERT_EQ(fixed.links_fixed, 1u);
    KTEST_ASSERT(fixed.counts_fixed >= 1);
    KTEST_ASSERT_EQ(fs_check_unfixed(&fixed), 1u);   // the damaged name stays
}
