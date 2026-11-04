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
#include "kthread.h"   // a second op, run alongside the test

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

// ---- a batched delete joins the running transaction ------------------
//
// Its frees wait for the commit that carries it (struct t3_pfree). These
// run under fs_exclusive_begin() wherever a commit thread or idle commit
// landing in the middle would make the check pass for the wrong reason.

static char g_del_buf[64 * 1024];

static void del_fill(void) {
    for (unsigned i = 0; i < sizeof g_del_buf; i++) g_del_buf[i] = (char)(i * 7u + 13u);
}

// Usage as the volume reports it, in blocks.
static uint64_t del_used(struct t3_state *sbi) {
    uint64_t used = 0, total = 0;
    tfs3_disk_usage(sbi, &used, &total);
    return used / T3_BLOCK;
}

KTEST("tfs3", "under batched a delete commits nothing of its own") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *a = JOIN_POINT "/a", *b = JOIN_POINT "/b";
    int wrote = fs_write_range(a, 0, g_del_buf, 4096) && fs_write_range(b, 0, g_del_buf, 4096) &&
                fs_sync_path(JOIN_POINT);

    fs_exclusive_begin();
    uint32_t seq0 = sbi->jrn_seq;
    int deleted = fs_delete(a) == 1;
    uint32_t seq1 = sbi->jrn_seq;
    int deferred = sbi->txn_deferred && sbi->pf.head != 0;
    int gone = !fs_exists(a) && fs_exists(b);   // the staged view, before any commit
    int synced = fs_sync_path(JOIN_POINT);
    uint32_t seq2 = sbi->jrn_seq;
    int released = !t3_pf_pending(sbi) && sbi->pf_blocks == 0;
    fs_exclusive_end();
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote);
    KTEST_ASSERT(deleted);
    KTEST_ASSERT_EQ((int64_t)(seq1 - seq0), 0);   // the delete committed nothing
    KTEST_ASSERT(deferred);                        // ...its frees are waiting
    KTEST_ASSERT(gone);
    KTEST_ASSERT(synced);
    KTEST_ASSERT_EQ((int64_t)(seq2 - seq1), 1);   // one commit carried it
    KTEST_ASSERT(released);                        // ...and released them
}

// NOTHING REUSES A DELETED FILE'S BLOCKS OR INODE BEFORE ITS COMMIT: the
// allocator, asked for the very block (the adjacent-first probe), must
// not get it, and the inode bit stays set -- until a sync, after which
// the same probe does get it. `df` counts the space free throughout.
KTEST("tfs3", "a batched delete's blocks and inode are not reused before its commit") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *f = JOIN_POINT "/f";
    int ready = fs_sync_path(JOIN_POINT);
    uint64_t used0 = del_used(sbi);
    int wrote = ready && fs_write_range(f, 0, g_del_buf, 2 * T3_BLOCK) && fs_sync_path(JOIN_POINT);

    fs_exclusive_begin();
    uint64_t ino = 0;
    struct t3_inode node;
    int found = wrote && t3_resolve(sbi, "/f", &ino) && t3_read_inode(sbi, ino, &node) &&
                node.ptrs[0] && node.ptrs[1];
    uint32_t blk = found ? node.ptrs[0] : 0;
    uint32_t bg = found ? (blk - sbi->group0) / T3_BPG : 0, bi = found ? (blk - sbi->group0) % T3_BPG : 0;
    uint32_t ig = found ? (uint32_t)(ino / sbi->sb.ipg) : 0, ii = found ? (uint32_t)(ino % sbi->sb.ipg) : 0;
    int deleted = found && fs_delete(f) == 1;
    int blk_held = deleted && t3_bbm_test(sbi, bg, bi);
    int ino_held = deleted && t3_ibm_test(sbi, ig, ii);
    uint32_t probe = deleted ? t3_alloc_block(sbi, bg, blk - 1) : 0;
    int not_reused = deleted && probe != blk;
    if (probe) t3_free_block_bit(sbi, probe);
    uint64_t used_pending = del_used(sbi);
    int synced = deleted && fs_sync_path(JOIN_POINT);
    int blk_free = synced && !t3_bbm_test(sbi, bg, bi);
    int ino_free = synced && !t3_ibm_test(sbi, ig, ii);
    uint32_t again = synced ? t3_alloc_block(sbi, bg, blk - 1) : 0;
    int probe_works = again == blk;
    if (again) t3_free_block_bit(sbi, again);
    t3_flush_alloc_state(sbi);
    uint64_t used_after = del_used(sbi);
    fs_exclusive_end();
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = fs_check_at(JOIN_POINT, 0, &r) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(found);
    KTEST_ASSERT(deleted);
    KTEST_ASSERT(blk_held);          // still marked used before the commit
    KTEST_ASSERT(ino_held);
    KTEST_ASSERT(not_reused);        // ...so the allocator could not hand it out
    KTEST_ASSERT_EQ((int64_t)used_pending, (int64_t)used0);   // df: free already
    KTEST_ASSERT(synced);
    KTEST_ASSERT(blk_free);          // released by the commit
    KTEST_ASSERT(ino_free);
    KTEST_ASSERT(probe_works);       // ...and the same probe now gets it
    KTEST_ASSERT_EQ((int64_t)used_after, (int64_t)used0);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r), 0u);
}

// THE CRASH PROMISE, by snapshotting the RAM volume (no disk cache sits
// over an unregistered device): a power cut after the delete but before
// its commit replays a volume where the file is whole; one after the
// sync, a volume where it is gone and every block of it free. A file
// past the direct pointers, so its frees include a table read in a gap.
KTEST("tfs3", "a batched delete is undone by a crash before its commit, and whole after") {
    struct t3_state *sbi = join_mount();
    uint8_t *crash1 = sbi ? kmalloc(VOL_BYTES) : 0, *crash2 = crash1 ? kmalloc(VOL_BYTES) : 0;
    if (!crash2) {
        kfree(crash1);
        join_unmount();
        KTEST_SKIP("could not mount and snapshot a 4 MiB volume");
    }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *f = JOIN_POINT "/f", *k = JOIN_POINT "/k";
    uint32_t len = 20 * T3_BLOCK;   // 12 direct + a single-indirect table
    int ready = fs_write_range(k, 0, g_del_buf, 100) && fs_sync_path(JOIN_POINT);
    uint64_t used0 = del_used(sbi);
    int wrote = ready && fs_write_range(f, 0, g_del_buf, len) && fs_sync_path(JOIN_POINT);

    fs_exclusive_begin();
    int deleted = wrote && fs_delete(f) == 1;
    k_memcpy(crash1, g_vol[0], VOL_BYTES);    // power cut: the delete was only staged
    int synced = deleted && fs_sync_path(JOIN_POINT);
    k_memcpy(crash2, g_vol[0], VOL_BYTES);    // power cut after the commit and its frees
    fs_exclusive_end();

    const char *why = 0;
    int back1 = 0, whole = 0, clean1 = 0, back2 = 0, gone2 = 0, clean2 = 0;
    uint64_t used2 = 0;
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    static char back[20 * T3_BLOCK];
    if (synced && mount_remove(JOIN_POINT, &why)) {
        k_memcpy(g_vol[0], crash1, VOL_BYTES);
        back1 = mount_add(&VOL_DEV[0], "tfs3", JOIN_POINT, 0, 0, &why);
        whole = back1 && fs_size(f) == len && fs_read_range(f, 0, back, len) == len &&
                k_memcmp(back, g_del_buf, len) == 0;
        clean1 = back1 && fs_check_at(JOIN_POINT, 0, &r1) == 0;
        if (back1 && mount_remove(JOIN_POINT, &why)) {
            k_memcpy(g_vol[0], crash2, VOL_BYTES);
            back2 = mount_add(&VOL_DEV[0], "tfs3", JOIN_POINT, 0, 0, &why);
            const struct mount *m = back2 ? mount_resolve(JOIN_POINT "/x", 0) : 0;
            gone2 = m && !fs_exists(f) && fs_exists(k);
            clean2 = m && fs_check_at(JOIN_POINT, 0, &r2) == 0;
            if (m) used2 = del_used(m->state);
        }
    }
    kfree(crash1);
    kfree(crash2);
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && deleted && synced);
    KTEST_ASSERT(back1);
    KTEST_ASSERT(whole);                               // before the commit: still there
    KTEST_ASSERT(clean1);
    KTEST_ASSERT_EQ(fs_check_problems(&r1), 0u);
    KTEST_ASSERT(back2);
    KTEST_ASSERT(gone2);                               // after it: gone
    KTEST_ASSERT(clean2);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);       // ...nothing leaked
    KTEST_ASSERT_EQ((int64_t)used2, (int64_t)used0);   // ...and every block free on disk
}

// DELETE TO MAKE ROOM, THEN WRITE, on a full volume: the write's allocation
// finds nothing while the delete's frees wait, so it commits, they are
// released, and it retries (t3_pf_reclaim()). Under exclusion, so no
// commit thread or idle commit makes the room first.
KTEST("tfs3", "a write on a full volume retries after a batched delete's commit") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *a = JOIN_POINT "/a", *fill = JOIN_POINT "/fill", *c = JOIN_POINT "/c";
    int wrote = fs_write_range(a, 0, g_del_buf, 16 * T3_BLOCK) && fs_sync_path(JOIN_POINT);
    uint32_t n = 0;
    while (wrote && n < VOL_BYTES / T3_BLOCK &&
           fs_write_range(fill, (uint64_t)n * T3_BLOCK, g_del_buf, T3_BLOCK) == 1) n++;
    int synced = fs_sync_path(JOIN_POINT);

    fs_exclusive_begin();
    uint32_t seq0 = sbi->jrn_seq;
    uint32_t probe = t3_alloc_block(sbi, 0, 0);
    int full = probe == 0;
    if (probe) t3_free_block_bit(sbi, probe);
    int deleted = fs_delete(a) == 1;
    int still_full = t3_alloc_block(sbi, 0, 0) == 0;   // the frees wait
    uint32_t seq1 = sbi->jrn_seq;
    int made = fs_write_range(c, 0, g_del_buf, 12 * T3_BLOCK) == 1;
    uint32_t seq2 = sbi->jrn_seq;
    fs_exclusive_end();
    static char back[12 * T3_BLOCK];
    int same = made && fs_read_range(c, 0, back, sizeof back) == sizeof back &&
               k_memcmp(back, g_del_buf, sizeof back) == 0;
    int synced2 = fs_sync_path(JOIN_POINT);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = fs_check_at(JOIN_POINT, 0, &r) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && synced);
    KTEST_ASSERT(n > 0);
    KTEST_ASSERT(full);            // the fixture really filled it
    KTEST_ASSERT(deleted);
    KTEST_ASSERT(still_full);
    KTEST_ASSERT_EQ((int64_t)(seq1 - seq0), 0);
    KTEST_ASSERT(made);            // the write found the room
    KTEST_ASSERT(seq2 != seq1);    // ...by committing the delete
    KTEST_ASSERT(same);
    KTEST_ASSERT(synced2 && checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r), 0u);
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
    // `strict`, or a delete joins the running transaction and has no
    // commit of its own to make wait.
    storage_config_set_mode_for_test(1, 0);
    int deleted = in_flight && fs_delete(b);   // its own commit, while the thread's is in flight
    storage_config_set_mode_for_test(1, 1);
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

// ---- an op's rollback log across a dropped lock -----------------------
//
// A device whose flush sleeps like SLOW_DEV's and FAILS for one pid: the
// op under test fails its own commit, and nobody else's does.
static volatile int g_fail_pid = -1;   // the KTEST body itself may be pid 0

static int failpid_flush(const struct block_device *self) {
    slow_flush(self);
    return scheduler_current_pid() != g_fail_pid;
}

static const struct block_device FAILPID_DEV = {
    .name = "t3failpid", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_FLUSH, .flush = failpid_flush,
};

// An fs call in a kernel thread, so the test can run a second one beside it.
struct t3_side_op {
    const char *path;
    const void *buf;
    uint32_t len;
    volatile int pid, done, ok;
};

static void side_write(void *arg) {
    struct t3_side_op *o = arg;
    o->pid = scheduler_current_pid();
    o->ok = fs_write_range(o->path, 0, o->buf, o->len) == 1;
    o->done = 1;
}

static int wait_flag(volatile int *v, uint64_t ns) {
    uint64_t t0 = clocksource_now_ns();
    while (!*v)
        if (clocksource_now_ns() - t0 > ns) return 0;
    return 1;
}

// THE HANDLE IS ONE PER MOUNT. A strict write that met the thread's commit
// in flight waited for it with the lock dropped and its handle open; a
// second write let in there unwound that handle at its t3_txn_begin() and
// staged its own, which the first then committed as its own -- and failed.
// The second committed nothing, reported success, and leaked its blocks.
KTEST("tfs3", "a write that waits for the thread's commit keeps its own handle") {
    if (!t3_commit_thread_running()) KTEST_SKIP("no commit thread on this boot");
    struct t3_state *sbi = join_mount_on(&FAILPID_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    static char buf[8192], back[8192];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (char)(i * 7u + 3u);
    const char *a = JOIN_POINT "/a", *c = JOIN_POINT "/c", *s = JOIN_POINT "/s";
    // a and c in different inode-table blocks: c staging a's block while
    // a's own commit is in flight would stop the volume when a's fails.
    int setup = fs_write(a, "", 0);
    for (int i = 0; setup && i < 40; i++) {
        char f[48];
        k_snprintf(f, sizeof f, JOIN_POINT "/f%d", i);
        setup = fs_write(f, "", 0);
    }
    setup = setup && fs_write(c, "", 0) && fs_sync_path(JOIN_POINT) &&
            fs_write_range(s, 0, buf, 4096) == 1;   // s: staged, for the thread

    sbi->txn_staged_tick = coarse_ticks() - storage_writeback_ticks() - 1;
    t3_commit_kick();
    uint64_t t0 = clocksource_now_ns();
    int in_flight = 0;
    while (clocksource_now_ns() - t0 < 2000000000ull)
        if (__atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) == 1) { in_flight = 1; break; }
    storage_config_set_mode_for_test(1, 0);   // a and c commit their own, and a's fails

    static struct t3_side_op op;
    k_memset((void *)&op, 0, sizeof op);
    op.path = a; op.buf = buf; op.len = sizeof buf;
    uint32_t waits0 = sbi->commit_waits;
    int started = in_flight && kthread_start("ktest-a", side_write, &op) > 0;
    int a_waits = started && wait_flag(&op.pid, 2000000000ull);
    t0 = clocksource_now_ns();
    while (a_waits && sbi->commit_waits == waits0 && clocksource_now_ns() - t0 < 2000000000ull) {}
    a_waits = a_waits && sbi->commit_waits != waits0;
    g_fail_pid = op.pid;
    int c_ok = a_waits && fs_write_range(c, 0, buf, sizeof buf) == 1;   // begins while a waits
    int a_done = started && wait_flag(&op.done, 5000000000ull);
    g_fail_pid = -1;
    int writable = !sbi->readonly;
    int synced = fs_sync_path(JOIN_POINT);

    const char *why = 0;
    int down = mount_remove(JOIN_POINT, &why);
    int up = down && mount_add(&FAILPID_DEV, "tfs3", JOIN_POINT, 0, 0, &why);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = up && fs_check_at(JOIN_POINT, 0, &r) == 0;
    int c_same = up && fs_size(c) == sizeof buf && fs_read_range(c, 0, back, sizeof back) == sizeof back &&
                 k_memcmp(buf, back, sizeof buf) == 0;
    int a_empty = up && fs_size(a) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(setup);
    KTEST_ASSERT(in_flight);
    KTEST_ASSERT(a_waits);    // a parked in the thread's commit
    KTEST_ASSERT(c_ok);
    KTEST_ASSERT(a_done);
    KTEST_ASSERT(!op.ok);     // ...and failed its own commit
    KTEST_ASSERT(writable);   // an unwound op does not stop the volume
    KTEST_ASSERT(synced);
    KTEST_ASSERT(checked);
    KTEST_ASSERT(c_same);
    KTEST_ASSERT(a_empty);
    KTEST_ASSERT_EQ(r.leaked + r.referenced_but_free + r.double_allocated + r.links_wrong +
                    r.bad_names + r.dir_errors, 0);
}

// ---- an op's own commit in a lock gap (fslock 5c-2) --------------------

// A side write that fails its own commits: it names itself, so nothing
// the test does before it gets there can fail by mistake.
static void side_write_failing(void *arg) {
    struct t3_side_op *o = arg;
    o->pid = scheduler_current_pid();
    g_fail_pid = o->pid;
    o->ok = fs_write_range(o->path, 0, o->buf, o->len) == 1;
    o->done = 1;
}

// THE ROLLBACK LOG ACROSS AN OP'S OWN GAP: a strict write's commit runs
// with the lock dropped, and a second write that allocates in the gap
// arms the mount's one log. When the first write's commit aborts, its
// rollback must free its own blocks -- not the second's, which that write
// then commits. The two inodes sit in different table blocks, so the
// abort unwinds rather than stopping the volume.
KTEST("tfs3", "a strict write whose commit aborts in its gap frees only its own blocks") {
    struct t3_state *sbi = join_mount_on(&FAILPID_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    static char buf[8192], back[8192];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (char)(i * 13u + 5u);
    const char *a = JOIN_POINT "/a", *b = JOIN_POINT "/b";
    int setup = fs_write(a, "", 0);
    for (int i = 0; setup && i < 40; i++) {   // past a's inode-table block
        char f[48];
        k_snprintf(f, sizeof f, JOIN_POINT "/f%d", i);
        setup = fs_write(f, "", 0);
    }
    setup = setup && fs_write(b, "", 0) && fs_sync_path(JOIN_POINT);
    storage_config_set_mode_for_test(1, 0);

    static struct t3_side_op op;
    k_memset((void *)&op, 0, sizeof op);
    op.path = a; op.buf = buf; op.len = sizeof buf;
    int started = setup && kthread_start("ktest-a", side_write_failing, &op) > 0;
    uint64_t t0 = clocksource_now_ns();
    int in_gap = 0;
    while (started && clocksource_now_ns() - t0 < 2000000000ull)
        if (__atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) == 1) { in_gap = 1; break; }
    storage_config_set_mode_for_test(1, 1);   // b allocates and stops, inside a's gap
    int b_ok = in_gap && fs_write_range(b, 0, buf, sizeof buf) == 1;
    int b_in_gap = __atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) == 1;
    int a_done = started && wait_flag(&op.done, 5000000000ull);
    g_fail_pid = -1;
    int writable = !sbi->readonly;
    int synced = fs_sync_path(JOIN_POINT);

    const char *why = 0;
    int down = mount_remove(JOIN_POINT, &why);
    int up = down && mount_add(&FAILPID_DEV, "tfs3", JOIN_POINT, 0, 0, &why);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = up && fs_check_at(JOIN_POINT, 0, &r) == 0;
    int b_same = up && fs_size(b) == sizeof buf && fs_read_range(b, 0, back, sizeof back) == sizeof back &&
                 k_memcmp(buf, back, sizeof buf) == 0;
    int a_empty = up && fs_size(a) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(setup);
    KTEST_ASSERT(in_gap);       // a's own commit, with the lock dropped
    KTEST_ASSERT(b_ok);
    KTEST_ASSERT(b_in_gap);     // ...and b ran inside it
    KTEST_ASSERT(a_done);
    KTEST_ASSERT(!op.ok);
    KTEST_ASSERT(writable);     // unwound, not stopped
    KTEST_ASSERT(synced);
    KTEST_ASSERT(checked);
    KTEST_ASSERT(b_same);
    KTEST_ASSERT(a_empty);
    KTEST_ASSERT_EQ(r.leaked + r.referenced_but_free + r.double_allocated + r.links_wrong +
                    r.bad_names + r.dir_errors, 0);
}

static void side_delete(void *arg) {
    struct t3_side_op *o = arg;
    o->pid = scheduler_current_pid();
    o->ok = fs_delete(o->path);
    o->done = 1;
}

// Under `strict` a delete commits itself; its flushes now run with the
// lock dropped, so a stat on the volume answers in the middle of them.
KTEST("tfs3", "a stat answers while a strict delete's own commit is flushing") {
    struct t3_state *sbi = join_mount_on(&SLOW_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    const char *b = JOIN_POINT "/b", *k = JOIN_POINT "/k";
    int setup = fs_write(b, "gone soon", 0) && fs_write(k, "kept", 0);

    static struct t3_side_op op;
    k_memset((void *)&op, 0, sizeof op);
    op.path = b;
    int started = setup && kthread_start("ktest-rm", side_delete, &op) > 0;
    uint64_t t0 = clocksource_now_ns();
    int in_flight = 0;
    while (started && clocksource_now_ns() - t0 < 2000000000ull)
        if (__atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) == 1) { in_flight = 1; break; }
    uint64_t s0 = clocksource_now_ns();
    struct fs_stat_info st;
    int stat_ok = in_flight && fs_stat(k, &st) && st.size == 4;
    uint64_t stat_ns = clocksource_now_ns() - s0;
    int still = __atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) != 0;
    int done = started && wait_flag(&op.done, 2000000000ull);
    int gone = done && !fs_exists(b);
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(setup);
    KTEST_ASSERT(in_flight);              // the delete's own commit, in a gap
    KTEST_ASSERT(stat_ok);
    KTEST_ASSERT(stat_ns < 20000000ull);  // well inside one 50 ms flush
    KTEST_ASSERT(still);
    KTEST_ASSERT(done && op.ok);
    KTEST_ASSERT(gone);
}

// An op's own commit that aborts in its gap unwinds that op alone: a
// strict create joining a create that `batched` left staged in the SAME
// directory block fails, and the earlier name stays staged -- put back
// from the create's undo copy -- and lands at the next sync.
KTEST("tfs3", "a strict create whose commit aborts in its gap unwinds only itself") {
    struct t3_state *sbi = join_mount_on(&FAILPID_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    const char *a = JOIN_POINT "/a", *x = JOIN_POINT "/x";
    int staged = fs_write(a, "first", 0) && sbi->txn_deferred;
    uint32_t seq0 = sbi->jrn_seq;
    storage_config_set_mode_for_test(1, 0);
    g_fail_pid = scheduler_current_pid();
    int made = fs_write(x, "second", 0);
    g_fail_pid = -1;
    int tried = sbi->jrn_seq != seq0;   // it reached a commit
    int x_gone = !fs_exists(x);
    int a_kept = sbi->txn_deferred && fs_exists(a);
    int writable = !sbi->readonly;
    int synced = fs_sync_path(JOIN_POINT);

    const char *why = 0;
    int down = mount_remove(JOIN_POINT, &why);
    int up = down && mount_add(&FAILPID_DEV, "tfs3", JOIN_POINT, 0, 0, &why);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = up && fs_check_at(JOIN_POINT, 0, &r) == 0;
    int a_landed = up && fs_size(a) == 5;
    int x_never = up && !fs_exists(x);
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(staged);
    KTEST_ASSERT(!made);
    KTEST_ASSERT(tried);
    KTEST_ASSERT(x_gone);
    KTEST_ASSERT(a_kept);     // the earlier op's work is still staged
    KTEST_ASSERT(writable);
    KTEST_ASSERT(synced);
    KTEST_ASSERT(checked);
    KTEST_ASSERT(a_landed);
    KTEST_ASSERT(x_never);
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
    // A name whose inode cannot be read hides what that inode holds, so the
    // pass is partial: link counts are reported, not changed.
    KTEST_ASSERT_EQ(fixed.links_fixed, 0u);
    KTEST_ASSERT(fixed.counts_fixed >= 1);
    KTEST_ASSERT_EQ(fs_check_unfixed(&fixed), 3u);   // the damaged name, /a's count, /b's block
}

// ---- what a failed allocation leaves behind ----------------------------

// THE WALK WRITES A TABLE BACK WHEN IT MOVES ON TO THE NEXT, so a write
// that fails later has already put a pointer to one of its new blocks in a
// table the file had before -- and the rollback frees that block. A sparse
// write across the single/double-indirect boundary with three blocks free:
// the data block in the old table, the double table, its leaf, and then
// nothing for the next data block.
KTEST("tfs3", "a write that runs out of space leaves no table naming a block it freed") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    del_fill();
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write_range(a, 12 * T3_BLOCK, g_del_buf, T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);

    static uint32_t held[VOL_BYTES / T3_BLOCK];
    uint32_t nheld = 0, b;
    fs_exclusive_begin();
    while (nheld < sizeof held / sizeof held[0] && (b = t3_alloc_block(sbi, 0, 0))) held[nheld++] = b;
    int room = nheld >= 3;
    for (int k = 0; k < 3 && nheld; k++) t3_free_block_bit(sbi, held[--nheld]);
    int failed = fs_write_range(a, (uint64_t)(12 + T3_PTRS_PER_BLOCK - 1) * T3_BLOCK,
                                g_del_buf, 2 * T3_BLOCK) == 0;
    while (nheld) t3_free_block_bit(sbi, held[--nheld]);
    t3_flush_alloc_state(sbi);
    fs_exclusive_end();

    static char back[T3_BLOCK];
    int same = fs_read_range(a, 12 * T3_BLOCK, back, T3_BLOCK) == T3_BLOCK &&
               k_memcmp(back, g_del_buf, T3_BLOCK) == 0;
    uint64_t size = fs_size(a);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = fs_check_at(JOIN_POINT, 0, &r) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && room);
    KTEST_ASSERT(failed);                     // the fixture really ran it out
    KTEST_ASSERT(same);
    KTEST_ASSERT_EQ((int64_t)size, (int64_t)(13 * T3_BLOCK));   // the failed write changed nothing
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(r.referenced_but_free, 0u);   // no pointer left at a freed block
    KTEST_ASSERT_EQ(fs_check_problems(&r), 0u);
}

// The stepped write fails its step like any other allocating op, and is
// run again the same way (t3_alog_rollback()) -- with its handle, which a
// failed step would otherwise free.
KTEST("tfs3", "a stepped write on a full volume runs its step again after a batched delete's commit") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *a = JOIN_POINT "/a", *fill = JOIN_POINT "/fill", *c = JOIN_POINT "/c";
    int wrote = fs_write_range(a, 0, g_del_buf, 16 * T3_BLOCK) && fs_sync_path(JOIN_POINT);
    uint32_t n = 0;
    while (wrote && n < VOL_BYTES / T3_BLOCK &&
           fs_write_range(fill, (uint64_t)n * T3_BLOCK, g_del_buf, T3_BLOCK) == 1) n++;
    int synced = fs_sync_path(JOIN_POINT);

    fs_exclusive_begin();
    uint32_t probe = t3_alloc_block(sbi, 0, 0);
    int full = probe == 0;
    if (probe) t3_free_block_bit(sbi, probe);
    int deleted = fs_delete(a) == 1;
    int still_full = t3_alloc_block(sbi, 0, 0) == 0;   // the frees wait
    void *h = fs_write_range_begin(c, 0, g_del_buf, 12 * T3_BLOCK);
    enum fs_step_result last = FS_STEP_PENDING;
    while (h && last == FS_STEP_PENDING) last = fs_write_range_step(h);
    fs_exclusive_end();
    static char back[12 * T3_BLOCK];
    int same = last == FS_STEP_DONE && fs_read_range(c, 0, back, sizeof back) == sizeof back &&
               k_memcmp(back, g_del_buf, sizeof back) == 0;
    int synced2 = fs_sync_path(JOIN_POINT);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = fs_check_at(JOIN_POINT, 0, &r) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && synced && n > 0);
    KTEST_ASSERT(full && deleted && still_full);
    KTEST_ASSERT(h);
    KTEST_ASSERT_EQ((int)last, (int)FS_STEP_DONE);
    KTEST_ASSERT(same);
    KTEST_ASSERT(synced2 && checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r), 0u);
}

// A device that refuses to read one sector or to write another (and
// counts the reads it refused), and one whose flush can be made to fail.
static volatile uint64_t g_bad_lba = ~0ull, g_bad_write_lba = ~0ull;
static volatile uint32_t g_bad_reads;
static volatile int g_flush_fails;

static int fault_read(const struct block_device *self, uint64_t lba, int n, void *b) {
    (void)self;
    if (g_bad_lba >= lba && g_bad_lba < lba + (uint64_t)n) { g_bad_reads++; return 0; }
    return vol_rw(0, lba, n, b, 0);
}
static int fault_write(const struct block_device *self, uint64_t lba, int n, const void *b) {
    (void)self;
    if (g_bad_write_lba >= lba && g_bad_write_lba < lba + (uint64_t)n) return 0;
    return vol_rw(0, lba, n, 0, b);
}
static int failflush_flush(const struct block_device *self) { (void)self; return !g_flush_fails; }

static const struct block_device FAULT_DEV = {
    .name = "t3fault", .sector_count = vol_count, .read_sectors = fault_read,
    .write_sectors = fault_write, .max_sectors_per_xfer = vol_xfer,
};
static const struct block_device FAILFLUSH_DEV = {
    .name = "t3failflush", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_FLUSH, .flush = failflush_flush,
};

// A table fsck cannot read hides what it maps, which then looks leaked: a
// repair that reclaimed it would hand out blocks the file still names.
KTEST("tfs3", "fsck that cannot read a table reclaims nothing beneath it") {
    struct t3_state *sbi = join_mount_on(&FAULT_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write_range(a, 0, g_del_buf, 13 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint32_t table = 0;
    if (wrote) {
        const struct mount *m = mount_resolve(a, 0);
        mount_lock(m);
        uint64_t ino;
        struct t3_inode node;
        if (t3_resolve(sbi, "/a", &ino) && t3_read_inode(sbi, ino, &node)) table = node.ptrs[12];
        t3_rcache_drop(sbi);
        t3_pcache_drop(sbi);
        mount_unlock(m);
    }
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    g_bad_lba = sbi->vol.base_lba + (uint64_t)table * T3_SPB;
    int repaired = table && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    g_bad_lba = ~0ull;
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    static char back[T3_BLOCK];
    int same = fs_read_range(a, 12 * T3_BLOCK, back, T3_BLOCK) == T3_BLOCK &&
               k_memcmp(back, g_del_buf + 12 * T3_BLOCK, T3_BLOCK) == 0;
    join_unmount();

    KTEST_ASSERT(wrote && table);
    KTEST_ASSERT(repaired);
    KTEST_ASSERT(r1.leaked >= 1);                  // the block beneath looked leaked...
    KTEST_ASSERT_EQ(r1.reclaimed, 0u);             // ...and was left alone
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);   // readable again: nothing wrong
    KTEST_ASSERT(same);
}

// fsck lands the running transaction first; with a delete's frees in it,
// a commit that fails leaves them looking leaked, and a repair that took
// them would let the delete's later commit free them a second time.
KTEST("tfs3", "fsck refuses a pass when a delete's commit cannot land first") {
    struct t3_state *sbi = join_mount_on(&FAILFLUSH_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *a = JOIN_POINT "/a";
    uint64_t used0 = del_used(sbi);
    int wrote = fs_write_range(a, 0, g_del_buf, 8 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);

    fs_exclusive_begin();
    int deleted = fs_delete(a) == 1;
    int pending = t3_pf_pending(sbi);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    g_flush_fails = 1;
    int rc = fs_check_at(JOIN_POINT, FSCK_REPAIR, &r);
    g_flush_fails = 0;
    fs_exclusive_end();
    int synced = fs_sync_path(JOIN_POINT);
    struct fs_check_result r2;
    k_memset(&r2, 0, sizeof r2);
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    uint64_t used1 = del_used(sbi);
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && deleted && pending);
    KTEST_ASSERT(rc != 0);                         // not checked
    KTEST_ASSERT_EQ(r.reclaimed, 0u);
    KTEST_ASSERT(synced && checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
    KTEST_ASSERT_EQ((int64_t)used1, (int64_t)used0);   // the delete freed its blocks, once
}

// ---- the whole-TFS3 review (2026-11-03) --------------------------------

static const struct mount *join_m(void) { return mount_resolve(JOIN_POINT "/x", 0); }

static int join_inode(struct t3_state *sbi, const char *rel, uint64_t *ino, struct t3_inode *node) {
    const struct mount *m = join_m();
    if (!m) return 0;
    mount_lock(m);
    int ok = t3_resolve(sbi, rel, ino) && t3_read_inode(sbi, *ino, node);
    mount_unlock(m);
    return ok;
}

static int join_set_links(struct t3_state *sbi, uint64_t ino, uint16_t links) {
    const struct mount *m = join_m();
    mount_lock(m);
    struct t3_inode node;
    int ok = t3_read_inode(sbi, ino, &node);
    node.links = links;
    ok = ok && t3_txn_begin(sbi, 1) && t3_txn_stage_inode(sbi, ino, &node) && t3_txn_commit(sbi);
    mount_unlock(m);
    return ok;
}

KTEST("tfs3", "a link past 65535 names is refused, not wrapped") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    const char *a = JOIN_POINT "/a", *b = JOIN_POINT "/b";
    uint64_t ino = 0;
    struct t3_inode node;
    int made = fs_write(a, "a", 0) && join_inode(sbi, "/a", &ino, &node) &&
               join_set_links(sbi, ino, (uint16_t)T3_LINK_MAX);
    int linked = fs_link(a, b);
    struct fs_stat_info st;
    k_memset(&st, 0, sizeof st);
    int stat = fs_stat(a, &st);
    join_set_links(sbi, ino, 1);   // so the volume checks clean on its way out
    join_unmount();

    KTEST_ASSERT(made && stat);
    KTEST_ASSERT(!linked);
    KTEST_ASSERT_EQ((int)st.nlink, (int)T3_LINK_MAX);
}

// Holds every free block of the volume but `keep`, under exclusion.
static uint32_t g_held[VOL_BYTES / T3_BLOCK];
static uint32_t hold_all_but(struct t3_state *sbi, uint32_t keep) {
    uint32_t n = 0, b;
    while (n < sizeof g_held / sizeof g_held[0] && (b = t3_alloc_block(sbi, 0, 0))) g_held[n++] = b;
    for (uint32_t k = 0; k < keep && n; k++) t3_free_block_bit(sbi, g_held[--n]);
    return n;
}
static void release_held(struct t3_state *sbi, uint32_t n) {
    while (n) t3_free_block_bit(sbi, g_held[--n]);
    t3_flush_alloc_state(sbi);
}

KTEST("tfs3", "what a failed write left past end-of-file does not come back on a grow") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    del_fill();
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write_range(a, 0, g_del_buf, 100) == 1 && fs_sync_path(JOIN_POINT);
    fs_exclusive_begin();
    uint32_t held = hold_all_but(sbi, 0);
    int failed = fs_write_range(a, 100, g_del_buf + 100, 2 * T3_BLOCK) == 0;   // block 1 has no room
    release_held(sbi, held);
    fs_exclusive_end();
    int grew = fs_truncate(a, 2 * T3_BLOCK) == 1;
    static char back[T3_BLOCK];
    int read = fs_read_range(a, 0, back, T3_BLOCK) == T3_BLOCK;
    int zeros = 1;
    for (uint32_t i = 100; i < T3_BLOCK; i++) if (back[i]) zeros = 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && failed && grew && read);
    KTEST_ASSERT(k_memcmp(back, g_del_buf, 100) == 0);
    KTEST_ASSERT(zeros);
}

// A TABLE A BATCHED APPEND JOURNALED, then cut by a truncate: the cut goes
// into the journal with it. Written in place, the append's staged image
// landed after it and named the freed blocks again.
KTEST("tfs3", "a truncate after a batched append cuts the table the append journaled") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write_range(a, 0, g_del_buf, 16 * T3_BLOCK) == 1 &&
                fs_write_range(a, 16 * T3_BLOCK, g_del_buf, 4 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t ino;
    struct t3_inode node;
    int found = wrote && join_inode(sbi, "/a", &ino, &node) && node.ptrs[12];
    fs_exclusive_begin();
    int appended = found && fs_write_range(a, 20 * T3_BLOCK, g_del_buf, T3_BLOCK) == 1;
    int cut = appended && fs_truncate(a, 14 * T3_BLOCK) == 1;   // inside the single table
    fs_exclusive_end();
    int synced = fs_sync_path(JOIN_POINT);
    const uint8_t *tbl = g_vol[0] + (uint64_t)node.ptrs[12] * T3_BLOCK;
    int tail_clear = 1;
    for (uint32_t i = 2; i < T3_PTRS_PER_BLOCK; i++) if (rd32(tbl + i * 4)) tail_clear = 0;
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = fs_check_at(JOIN_POINT, 0, &r) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(found && appended && cut && synced);
    KTEST_ASSERT(tail_clear);                       // on disk, past the cut: nothing named
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(r.referenced_but_free, 0u);
    KTEST_ASSERT_EQ(fs_check_problems(&r), 0u);
}

KTEST("tfs3", "fsck that cannot read a folder's inode reclaims nothing inside it") {
    struct t3_state *sbi = join_mount_on(&FAULT_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    // Four files first, so the folder's inode is in a sector of its own
    // rather than the root's.
    int made = fs_write(JOIN_POINT "/x1", "1", 0) && fs_write(JOIN_POINT "/x2", "2", 0) &&
               fs_write(JOIN_POINT "/x3", "3", 0) && fs_write(JOIN_POINT "/x4", "4", 0) &&
               fs_mkdir(JOIN_POINT "/d") &&
               fs_write_range(JOIN_POINT "/d/f", 0, g_del_buf, 3 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t dino = 0, lba = 0, rlba = 0;
    uint32_t off;
    struct t3_inode node;
    const struct mount *m = join_m();
    if (made && join_inode(sbi, "/d", &dino, &node)) {
        mount_lock(m);
        t3_inode_pos(sbi, dino, &lba, &off);
        t3_inode_pos(sbi, T3_INO_ROOT, &rlba, &off);
        t3_icache_drop(sbi);
        t3_rcache_drop(sbi);
        mount_unlock(m);
    }
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    g_bad_lba = sbi->vol.base_lba + lba;
    int repaired = dino && lba != rlba && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    g_bad_lba = ~0ull;
    mount_lock(m);
    t3_icache_drop(sbi);
    mount_unlock(m);
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    static char back[3 * T3_BLOCK];
    int same = fs_read_range(JOIN_POINT "/d/f", 0, back, sizeof back) == sizeof back &&
               k_memcmp(back, g_del_buf, sizeof back) == 0;
    join_unmount();

    KTEST_ASSERT(made && dino && lba != rlba);
    KTEST_ASSERT(repaired);
    KTEST_ASSERT(r1.bad_names >= 1);   // the sector's other inodes too
    KTEST_ASSERT_EQ(r1.reclaimed, 0u);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
    KTEST_ASSERT(same);
}

KTEST("tfs3", "a pointer fsck clears is not written back from the write cache") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    const char *a = JOIN_POINT "/a";
    // Inside the file, with slots 4 and 5 holes: a pointer past end-of-file
    // would be cut by the first write past it (cut_past_eof()).
    int wrote = fs_write_range(a, 0, g_del_buf, 16 * T3_BLOCK) == 1 &&
                fs_write_range(a, 18 * T3_BLOCK, g_del_buf, 6 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t ino;
    struct t3_inode node;
    int found = wrote && join_inode(sbi, "/a", &ino, &node) && node.ptrs[12];
    const struct mount *m = join_m();
    if (found) {
        // Slot 10 of the table names a block past the volume's end.
        mount_lock(m);
        uint8_t *tbl = g_vol[0] + ((uint64_t)node.ptrs[12] * T3_SPB) * 512;
        wr32(tbl + 10 * 4, 0xfffff000u);
        t3_pcache_drop(sbi);
        t3_rcache_drop(sbi);
        mount_unlock(m);
    }
    // A write into slot 5 caches the table, bad slot and all.
    int cached = found && fs_write_range(a, (12 + 5) * T3_BLOCK, g_del_buf, T3_BLOCK) == 1;
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    int repaired = cached && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    int wrote10 = repaired && fs_write_range(a, (12 + 10) * T3_BLOCK, g_del_buf, T3_BLOCK) == 1;
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    join_unmount();

    KTEST_ASSERT(cached && repaired);
    KTEST_ASSERT(r1.pointers_cleared >= 1);
    KTEST_ASSERT(wrote10);              // slot 10 is a hole now, not the bad pointer
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
}

struct long_names { int n; };
static void count_long(void *ctx, const char *name, uint32_t size, int is_dir) {
    (void)size; (void)is_dir;
    if (name[0] == 'z') ((struct long_names *)ctx)->n++;   // the crafted record, listed
}

KTEST("tfs3", "a dirent whose name runs past its record is not read past the block") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    struct t3_inode root;
    const struct mount *m = join_m();
    mount_lock(m);
    int found = t3_read_inode(sbi, T3_INO_ROOT, &root) && root.ptrs[0];
    mount_unlock(m);
    if (found) {
        // `.` at 0 (12 bytes), `..` cut to end at 4088, then a record of
        // 8 bytes claiming a 255-byte name.
        mount_lock(m);
        uint8_t *blk = g_vol[0] + ((uint64_t)root.ptrs[0] * T3_SPB) * 512;
        wr16(blk + 12 + 4, (uint16_t)(4088 - 12));
        wr32(blk + 4088, T3_INO_ROOT);
        wr16(blk + 4088 + 4, 8);
        blk[4088 + 6] = 255;
        blk[4088 + 7] = 'z';
        t3_rcache_drop(sbi);
        t3_icache_drop(sbi);
        t3_ncache_flush(sbi);
        mount_unlock(m);
    }
    struct long_names ln = { 0 };
    fs_list(JOIN_POINT, count_long, &ln);
    join_unmount();

    KTEST_ASSERT(found);
    KTEST_ASSERT_EQ(ln.n, 0);
}

KTEST("tfs3", "a journal header that cannot be read mounts the volume read-only") {
    if (!g_vol[0]) g_vol[0] = kmalloc(VOL_BYTES);
    if (!g_vol[0]) KTEST_SKIP("could not allocate a 4 MiB volume");
    k_memset(g_vol[0], 0, VOL_BYTES);
    struct fs_scratch sc;
    int ok = mount_scratch_begin(T(), &sc);
    if (ok) { ok = T()->format(sc.st, &FAULT_DEV); mount_scratch_end(&sc); }
    fs_mkdir(JOIN_POINT);
    g_bad_lba = (uint64_t)T3_JH_BLOCK * T3_SPB;
    const char *why = 0;
    int mounted = ok && mount_add(&FAULT_DEV, "tfs3", JOIN_POINT, 0, 0, &why);
    g_bad_lba = ~0ull;
    const struct mount *m = mounted ? join_m() : 0;
    struct t3_state *sbi = m ? m->state : 0;
    int readonly = sbi && sbi->readonly;
    join_unmount();

    KTEST_ASSERT(ok);
    KTEST_ASSERT(!mounted || readonly);   // never read-write over an unread journal
}

KTEST("tfs3", "a batched writer appending without pause does not keep resetting the commit clock") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *a = JOIN_POINT "/a";
    int synced = fs_write_range(a, 0, g_del_buf, T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    fs_exclusive_begin();   // no commit thread, no idle commit
    int first = fs_write_range(a, FS_OFFSET_END, g_del_buf, 100) == 1;
    uint64_t t0 = sbi->txn_staged_tick;
    uint64_t start = coarse_ticks();
    while (coarse_ticks() < start + 3) cpu_relax();
    int second = fs_write_range(a, FS_OFFSET_END, g_del_buf, 100) == 1;
    uint64_t t1 = sbi->txn_staged_tick;
    int deferred = sbi->txn_deferred;
    fs_exclusive_end();
    fs_sync_path(JOIN_POINT);
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(synced && first && second && deferred);
    KTEST_ASSERT_EQ((int64_t)t1, (int64_t)t0);   // timed from the first stop
}

KTEST("tfs3", "a descriptor block that fails to write stays dirty for the next flush") {
    struct t3_state *sbi = join_mount_on(&FAULT_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    const struct mount *m = join_m();
    mount_lock(m);
    t3_flush_alloc_state(sbi);
    uint32_t b = t3_alloc_block(sbi, 0, 0);
    g_bad_write_lba = sbi->vol.base_lba + (uint64_t)sbi->gdt_block * T3_SPB;
    int flushed = t3_flush_alloc_state(sbi);
    int dirty = t3_alloc_dirty_blocks(sbi);
    g_bad_write_lba = ~0ull;
    int flushed2 = t3_flush_alloc_state(sbi);
    int dirty2 = t3_alloc_dirty_blocks(sbi);
    if (b) t3_free_block_bit(sbi, b);
    t3_flush_alloc_state(sbi);
    mount_unlock(m);
    join_unmount();

    KTEST_ASSERT(b);
    KTEST_ASSERT(!flushed);
    KTEST_ASSERT(dirty >= 1);     // the descriptor block is still owed
    KTEST_ASSERT(flushed2);
    KTEST_ASSERT_EQ(dirty2, 0);
}

KTEST("tfs3", "an unreadable bitmap is read once per allocation, and a free into it moves no count") {
    struct t3_state *sbi = join_mount_on(&FAULT_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int wrote = fs_write(JOIN_POINT "/a", "a", 0) && fs_sync_path(JOIN_POINT);
    uint64_t ino;
    struct t3_inode node;
    int found = wrote && join_inode(sbi, "/a", &ino, &node);
    const struct mount *m = join_m();
    mount_lock(m);
    t3_flush_alloc_state(sbi);
    t3_bmap_drop(sbi);
    g_bad_lba = sbi->vol.base_lba + (uint64_t)(t3_group_base(sbi, 0) + T3_BMAP_BLOCK) * T3_SPB;
    uint32_t r0 = g_bad_reads;
    uint32_t got = t3_alloc_block(sbi, 0, 0);
    uint32_t reads = g_bad_reads - r0;
    uint32_t free0 = sbi->gd[0].free_blocks;
    if (found && node.ptrs[0]) t3_free_block_bit(sbi, node.ptrs[0]);
    uint32_t free1 = sbi->gd[0].free_blocks;
    g_bad_lba = ~0ull;
    t3_bmap_drop(sbi);
    mount_unlock(m);
    join_unmount();

    KTEST_ASSERT(found);
    KTEST_ASSERT_EQ(got, 0u);         // the only group cannot be read
    KTEST_ASSERT(reads <= 2);
    KTEST_ASSERT_EQ(free1, free0);
}

KTEST("tfs3", "a stepped read does not follow a truncated file's old blocks into another file") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    static uint8_t pa[2 * T3_BLOCK], pb[2 * T3_BLOCK], back[2 * T3_BLOCK];
    k_memset(pa, 'a', sizeof pa);
    k_memset(pb, 'b', sizeof pb);
    k_memset(back, 0, sizeof back);
    const char *a = JOIN_POINT "/a", *b = JOIN_POINT "/b";
    int wrote = fs_write_range(a, 0, pa, sizeof pa) == 1;
    void *h = fs_read_range_begin(a, 0, back, sizeof back);
    uint32_t total = 0;
    enum fs_step_result first = h ? fs_read_range_step(h, &total) : FS_STEP_FAILED;
    int cut = fs_truncate(a, 0) == 1 && fs_write_range(b, 0, pb, sizeof pb) == 1;
    enum fs_step_result last = first;
    while (h && last == FS_STEP_PENDING) last = fs_read_range_step(h, &total);
    int foreign = 0;
    for (uint32_t i = T3_BLOCK; i < sizeof back; i++) if (back[i] == 'b') foreign = 1;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && h && cut);
    KTEST_ASSERT_EQ((int)first, (int)FS_STEP_PENDING);
    KTEST_ASSERT(!foreign);
}

KTEST("tfs3", "fsck repair keeps the blocks an open stepped write has written") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    const char *c = JOIN_POINT "/c";
    void *h = fs_write_range_begin(c, 0, g_del_buf, 8 * T3_BLOCK);
    enum fs_step_result st = FS_STEP_PENDING;
    for (int i = 0; h && i < 3 && st == FS_STEP_PENDING; i++) st = fs_write_range_step(h);
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    int repaired = st == FS_STEP_PENDING && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    while (h && st == FS_STEP_PENDING) st = fs_write_range_step(h);
    static char back[8 * T3_BLOCK];
    int same = st == FS_STEP_DONE && fs_read_range(c, 0, back, sizeof back) == sizeof back &&
               k_memcmp(back, g_del_buf, sizeof back) == 0;
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    join_unmount();

    KTEST_ASSERT(h && repaired);
    KTEST_ASSERT_EQ(r1.reclaimed, 0u);
    KTEST_ASSERT(same);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
}

KTEST("tfs3", "group counts on a 16 TiB volume do not wrap") {
    KTEST_ASSERT_EQ(t3_groups_over(0xffffffffu, T3_V3_GDT), 131072u);
    KTEST_ASSERT_EQ(t3_v3_gdt_blocks(0xffffffffu), 512u);
}

// THE DISK AS A CRASH WOULD LEAVE IT, after a batched append that put a new
// block in a table the file already had: that table may name the block
// only if the bitmap on disk already says it is used. Written in place,
// the table landed seconds before its bitmap (the commit's flush).
KTEST("tfs3", "a table the file had never reaches the disk ahead of its bitmap") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write_range(a, 0, g_del_buf, 13 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t ino;
    struct t3_inode node;
    int found = wrote && join_inode(sbi, "/a", &ino, &node) && node.ptrs[12];
    fs_exclusive_begin();   // nothing commits meanwhile
    int appended = found && fs_write_range(a, 13 * T3_BLOCK, g_del_buf, T3_BLOCK) == 1;
    // What is on the media now.
    const uint8_t *tbl = g_vol[0] + (uint64_t)node.ptrs[12] * T3_BLOCK;
    uint32_t named = rd32(tbl + 1 * 4);   // slot 1 is file block 13
    int used_on_disk = 1;
    if (named) {
        uint32_t rel = named - sbi->group0, g = rel / T3_BPG, i = rel % T3_BPG;
        const uint8_t *bm = g_vol[0] + (uint64_t)(t3_group_base(sbi, g) + T3_BMAP_BLOCK) * T3_BLOCK;
        used_on_disk = (bm[i >> 3] >> (i & 7)) & 1;
    }
    fs_exclusive_end();
    int synced = fs_sync_path(JOIN_POINT);
    static char back[T3_BLOCK];
    int same = fs_read_range(a, 13 * T3_BLOCK, back, T3_BLOCK) == T3_BLOCK &&
               k_memcmp(back, g_del_buf, T3_BLOCK) == 0;
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = fs_check_at(JOIN_POINT, 0, &r) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(found && appended);
    KTEST_ASSERT(!named || used_on_disk);
    KTEST_ASSERT(synced && same && checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r), 0u);
}

// ---- the second whole-TFS3 review (2026-11-04) --------------------------

// Where the record named `name` starts in a directory block; -1 if none.
static int dirent_at(const uint8_t *blk, const char *name) {
    uint32_t nl = (uint32_t)k_strlen(name), off = 0;
    while (off + 8 <= T3_BLOCK) {
        uint16_t rec = rd16(blk + off + 4);
        if (rec < 8) return -1;
        if (rd32(blk + off) && blk[off + 6] == nl && k_memcmp(blk + off + 7, name, nl) == 0) return (int)off;
        off += rec;
    }
    return -1;
}

static uint8_t *vol_block(uint32_t blk) { return g_vol[0] + (uint64_t)blk * T3_BLOCK; }

static void join_drop_caches(struct t3_state *sbi) {
    t3_rcache_drop(sbi);
    t3_icache_drop(sbi);
    t3_ncache_flush(sbi);
    t3_pcache_drop(sbi);
}

// Sets pointer `slot` of an inode through the journal; the old value.
static uint32_t join_set_ptr(struct t3_state *sbi, uint64_t ino, int slot, uint32_t v) {
    const struct mount *m = join_m();
    mount_lock(m);
    struct t3_inode node;
    uint32_t was = 0;
    if (t3_read_inode(sbi, ino, &node)) {
        was = node.ptrs[slot];
        node.ptrs[slot] = v;
        if (!(t3_txn_begin(sbi, 1) && t3_txn_stage_inode(sbi, ino, &node) && t3_txn_commit(sbi))) was = 0;
    }
    join_drop_caches(sbi);
    mount_unlock(m);
    return was;
}

static int file_is(const char *path, const void *want, uint32_t len) {
    static char back[3 * T3_BLOCK];
    return len <= sizeof back && fs_read_range(path, 0, back, len) == (int64_t)len && k_memcmp(back, want, len) == 0;
}

// A BROKEN CHAIN HIDES THE REST OF ITS BLOCK: what those names reach looks
// unreached, and a repair that reclaimed it freed live files.
KTEST("tfs3", "fsck that meets a broken entry chain reclaims nothing the rest of the block names") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    int made = fs_mkdir(JOIN_POINT "/d") && fs_write_range(JOIN_POINT "/d/f1", 0, g_del_buf, 2 * T3_BLOCK) == 1 &&
               fs_write(JOIN_POINT "/d/f2", "2", 1) && fs_sync_path(JOIN_POINT);
    uint64_t dino = 0;
    struct t3_inode dn;
    int found = made && join_inode(sbi, "/d", &dino, &dn) && dn.ptrs[0];
    const struct mount *m = join_m();
    uint8_t *blk = found ? vol_block(dn.ptrs[0]) : 0;
    int at = blk ? dirent_at(blk, "f1") : -1;
    uint16_t was = 0;
    if (at >= 0) {
        mount_lock(m);
        was = rd16(blk + at + 4);
        wr16(blk + at + 4, 6);   // shorter than any record
        join_drop_caches(sbi);
        mount_unlock(m);
    }
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    int repaired = at >= 0 && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    if (at >= 0) {
        mount_lock(m);
        wr16(blk + at + 4, was);
        join_drop_caches(sbi);
        mount_unlock(m);
    }
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    int same = file_is(JOIN_POINT "/d/f1", g_del_buf, 2 * T3_BLOCK);
    join_unmount();

    KTEST_ASSERT(made && found && at >= 0);
    KTEST_ASSERT(repaired);
    KTEST_ASSERT(r1.dir_errors >= 1);
    KTEST_ASSERT_EQ(r1.reclaimed, 0u);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
    KTEST_ASSERT(same);
}

KTEST("tfs3", "fsck leaves a folder's out-of-range pointer, and the files its block names") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    int made = fs_mkdir(JOIN_POINT "/d") && fs_write_range(JOIN_POINT "/d/f1", 0, g_del_buf, 2 * T3_BLOCK) == 1 &&
               fs_sync_path(JOIN_POINT);
    uint64_t dino = 0;
    struct t3_inode dn;
    int found = made && join_inode(sbi, "/d", &dino, &dn) && dn.ptrs[0];
    uint32_t real = found ? join_set_ptr(sbi, dino, 0, 0xfffff000u) : 0;
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    int repaired = real && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    if (real) join_set_ptr(sbi, dino, 0, real);
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    int same = file_is(JOIN_POINT "/d/f1", g_del_buf, 2 * T3_BLOCK);
    join_unmount();

    KTEST_ASSERT(made && found && real);
    KTEST_ASSERT(repaired);
    KTEST_ASSERT(r1.out_of_range >= 1);
    KTEST_ASSERT_EQ(r1.pointers_cleared, 0u);
    KTEST_ASSERT_EQ(r1.reclaimed, 0u);
    KTEST_ASSERT(fs_check_unfixed(&r1) > 0);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
    KTEST_ASSERT(same);
}

KTEST("tfs3", "a `..` fsck puts right is not reported as left unfixed") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int made = fs_mkdir(JOIN_POINT "/d") && fs_mkdir(JOIN_POINT "/d/e") && fs_sync_path(JOIN_POINT);
    uint64_t eino = 0;
    struct t3_inode en;
    int found = made && join_inode(sbi, "/d/e", &eino, &en) && en.ptrs[0];
    const struct mount *m = join_m();
    uint8_t *blk = found ? vol_block(en.ptrs[0]) : 0;
    int at = blk ? dirent_at(blk, "..") : -1;
    if (at >= 0) {
        mount_lock(m);
        wr32(blk + at, T3_INO_ROOT);   // the grandparent
        join_drop_caches(sbi);
        mount_unlock(m);
    }
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    int repaired = at >= 0 && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    join_unmount();

    KTEST_ASSERT(made && found && at >= 0 && repaired);
    KTEST_ASSERT(r1.dir_errors >= 1);
    KTEST_ASSERT_EQ(r1.dirs_fixed, r1.dir_errors);
    KTEST_ASSERT_EQ(fs_check_unfixed(&r1), 0u);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
}

// A "TABLE" CROSS-LINKED WITH ANOTHER FILE'S DATA: its words are that
// file's bytes, and clearing the ones out of range rewrote them.
KTEST("tfs3", "fsck repair clears no word of a table that is another file's data") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    static uint8_t junk[T3_BLOCK];
    for (uint32_t i = 0; i < T3_BLOCK; i += 4) wr32(junk + i, 0xfffff000u);
    int made = fs_write_range(JOIN_POINT "/a", 0, g_del_buf, 13 * T3_BLOCK) == 1 &&
               fs_write_range(JOIN_POINT "/b", 0, junk, T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t aino = 0, bino = 0;
    struct t3_inode an, bn;
    int found = made && join_inode(sbi, "/a", &aino, &an) && an.ptrs[12] &&
                join_inode(sbi, "/b", &bino, &bn) && bn.ptrs[0];
    int linked = found && join_set_ptr(sbi, aino, 12, bn.ptrs[0]) == an.ptrs[12];
    struct fs_check_result r1;
    k_memset(&r1, 0, sizeof r1);
    int repaired = linked && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    int intact = found && k_memcmp(vol_block(bn.ptrs[0]), junk, T3_BLOCK) == 0;
    join_unmount();

    KTEST_ASSERT(made && found && linked && repaired);
    KTEST_ASSERT(r1.double_allocated >= 1);
    KTEST_ASSERT_EQ(r1.pointers_cleared, 0u);
    KTEST_ASSERT(intact);
}

// rec_len 15 left `used` (16) past the record, and `rec_len - used` wrapped
// to "room": the new entry went past the block's end.
KTEST("tfs3", "a dirent record of unaligned length takes no new entry") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    struct t3_inode root;
    const struct mount *m = join_m();
    mount_lock(m);
    int found = t3_read_inode(sbi, T3_INO_ROOT, &root) && root.ptrs[0];
    mount_unlock(m);
    uint8_t *blk = found ? vol_block(root.ptrs[0]) : 0;
    int at = blk ? dirent_at(blk, "..") : -1;
    // `..` packed tight, then the record of 15, then a free one: the
    // first with room in the old arithmetic is the record of 15.
    int x = at + 12;
    if (at >= 0) {
        mount_lock(m);
        wr16(blk + at + 4, 12);
        wr32(blk + x, T3_INO_ROOT);
        wr16(blk + x + 4, 15);
        blk[x + 6] = 8;
        k_memcpy(blk + x + 7, "zzzzzzzz", 8);
        wr32(blk + x + 15, 0);
        wr16(blk + x + 15 + 4, (uint16_t)(T3_BLOCK - (uint32_t)(x + 15)));
        blk[x + 15 + 6] = 0;
        join_drop_caches(sbi);
        mount_unlock(m);
    }
    int made = at >= 0 && fs_write(JOIN_POINT "/n", "n", 1);
    int there = made && fs_exists(JOIN_POINT "/n");
    uint16_t rec = blk ? rd16(blk + x + 4) : 0;
    join_unmount();

    KTEST_ASSERT(found && at >= 0 && made);
    KTEST_ASSERT(there);
    KTEST_ASSERT_EQ((int)rec, 15);
}

KTEST("tfs3", "a write past a gap zeroes what a failed write left past end-of-file") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    del_fill();
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write_range(a, 0, g_del_buf, 100) == 1 && fs_sync_path(JOIN_POINT);
    fs_exclusive_begin();
    uint32_t held = hold_all_but(sbi, 0);
    int failed = fs_write_range(a, 100, g_del_buf + 100, 2 * T3_BLOCK) == 0;   // block 1 has no room
    release_held(sbi, held);
    fs_exclusive_end();
    int past = fs_write_range(a, 2 * T3_BLOCK, "x", 1) == 1;
    static char back[T3_BLOCK];
    int read = fs_read_range(a, 0, back, T3_BLOCK) == T3_BLOCK;
    int zeros = 1;
    for (uint32_t i = 100; i < T3_BLOCK; i++) if (back[i]) zeros = 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && failed && past && read);
    KTEST_ASSERT(k_memcmp(back, g_del_buf, 100) == 0);
    KTEST_ASSERT(zeros);
}

KTEST("tfs3", "a truncate that cannot read its boundary table is refused, not half done") {
    struct t3_state *sbi = join_mount_on(&FAULT_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write_range(a, 0, g_del_buf, 14 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t ino = 0;
    struct t3_inode node;
    int found = wrote && join_inode(sbi, "/a", &ino, &node) && node.ptrs[12];
    const struct mount *m = join_m();
    mount_lock(m);
    join_drop_caches(sbi);
    mount_unlock(m);
    if (found) g_bad_lba = sbi->vol.base_lba + (uint64_t)node.ptrs[12] * T3_SPB;
    int cut = found && fs_truncate(a, 13 * T3_BLOCK) == 1;
    g_bad_lba = ~0ull;
    struct fs_stat_info st;
    k_memset(&st, 0, sizeof st);
    int stat = fs_stat(a, &st);
    join_unmount();

    KTEST_ASSERT(found && stat);
    KTEST_ASSERT(!cut);
    KTEST_ASSERT_EQ(st.size, (uint64_t)14 * T3_BLOCK);
}

KTEST("tfs3", "a stepped write of no bytes past end-of-file leaves the size alone") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write(a, "a", 1);
    void *h = wrote ? fs_write_range_begin(a, 1u << 20, "", 0) : 0;
    enum fs_step_result r = FS_STEP_PENDING;
    while (h && r == FS_STEP_PENDING) r = fs_write_range_step(h);
    struct fs_stat_info st;
    k_memset(&st, 0, sizeof st);
    int stat = fs_stat(a, &st);
    join_unmount();

    KTEST_ASSERT(wrote && stat);
    KTEST_ASSERT_EQ(st.size, 1ull);
}

KTEST("tfs3", "renaming a missing name onto itself fails") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int missing = fs_rename(JOIN_POINT "/nope", JOIN_POINT "/nope");
    int there = fs_write(JOIN_POINT "/a", "a", 1) && fs_rename(JOIN_POINT "/a", JOIN_POINT "/a");
    join_unmount();

    KTEST_ASSERT(!missing);
    KTEST_ASSERT(there);
}

KTEST("tfs3", "a stepped read does not carry on into a new file given the deleted one's number") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    static uint8_t pa[2 * T3_BLOCK], pb[2 * T3_BLOCK], back[2 * T3_BLOCK];
    k_memset(pa, 'a', sizeof pa);
    k_memset(pb, 'b', sizeof pb);
    k_memset(back, 0, sizeof back);
    const char *a = JOIN_POINT "/a", *b = JOIN_POINT "/b";
    uint64_t aino = 0, bino = 0;
    struct t3_inode node;
    int wrote = fs_write_range(a, 0, pa, sizeof pa) == 1 && join_inode(sbi, "/a", &aino, &node);
    void *h = wrote ? fs_read_range_begin(a, 0, back, sizeof back) : 0;
    uint32_t total = 0;
    enum fs_step_result first = h ? fs_read_range_step(h, &total) : FS_STEP_FAILED;
    int swapped = fs_delete(a) && fs_write_range(b, 0, pb, sizeof pb) == 1 && join_inode(sbi, "/b", &bino, &node);
    enum fs_step_result last = first;
    while (h && last == FS_STEP_PENDING) last = fs_read_range_step(h, &total);
    int foreign = 0;
    for (uint32_t i = 0; i < sizeof back; i++) if (back[i] == 'b') foreign = 1;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && h && swapped);
    KTEST_ASSERT_EQ((int)first, (int)FS_STEP_PENDING);
    KTEST_ASSERT(bino != aino);
    KTEST_ASSERT(!foreign);
}

KTEST("tfs3", "a bitmap that failed to read is read again a second later") {
    struct t3_state *sbi = join_mount_on(&FAULT_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    const struct mount *m = join_m();
    mount_lock(m);
    t3_flush_alloc_state(sbi);
    t3_bmap_drop(sbi);
    g_bad_lba = sbi->vol.base_lba + (uint64_t)(t3_group_base(sbi, 0) + T3_BMAP_BLOCK) * T3_SPB;
    uint32_t refused = t3_alloc_block(sbi, 0, 0);
    g_bad_lba = ~0ull;
    mount_unlock(m);
    uint64_t t0 = clocksource_now_ns();
    while (clocksource_now_ns() - t0 < 1200000000ull) cpu_relax();
    mount_lock(m);
    uint32_t got = t3_alloc_block(sbi, 0, 0);
    if (got) t3_free_block_bit(sbi, got);
    t3_flush_alloc_state(sbi);
    mount_unlock(m);
    join_unmount();

    KTEST_ASSERT_EQ(refused, 0u);
    KTEST_ASSERT(got != 0);
}

struct slash_names { int n; };
static void count_slash(void *ctx, const char *name, uint32_t size, int is_dir) {
    (void)size; (void)is_dir;
    for (const char *p = name; *p; p++) if (*p == '/') ((struct slash_names *)ctx)->n++;
}

KTEST("tfs3", "a name holding '/' is not listed, and fsck reports it") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    struct t3_inode root;
    const struct mount *m = join_m();
    mount_lock(m);
    int found = t3_read_inode(sbi, T3_INO_ROOT, &root) && root.ptrs[0];
    mount_unlock(m);
    uint8_t *blk = found ? vol_block(root.ptrs[0]) : 0;
    int at = blk ? dirent_at(blk, "..") : -1;
    if (at >= 0) {
        mount_lock(m);
        wr16(blk + at + 4, (uint16_t)(4080 - at));
        wr32(blk + 4080, T3_INO_ROOT);
        wr16(blk + 4080 + 4, 16);
        blk[4080 + 6] = 8;
        k_memcpy(blk + 4080 + 7, "../../x\0", 8);
        join_drop_caches(sbi);
        mount_unlock(m);
    }
    struct slash_names sn = { 0 };
    fs_list(JOIN_POINT, count_slash, &sn);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = at >= 0 && fs_check_at(JOIN_POINT, 0, &r) == 0;
    join_unmount();

    KTEST_ASSERT(found && at >= 0 && checked);
    KTEST_ASSERT_EQ(sn.n, 0);
    KTEST_ASSERT(r.dir_errors >= 1);
}

// Writes and flushes in order: what reached the device after what.
static volatile uint32_t g_ord_seq, g_ord_hdr, g_ord_flush;
static volatile int g_ord_fail_after = -1;   // flushes that succeed before they fail
// A cleared header (byte 4 zero) can be made to fail, or to vanish as a
// device cache would lose it; two blocks' writes are stamped with the
// sequence number, and with the last flush before them.
static volatile int g_ord_fail_clear, g_ord_drop_clear;
static volatile uint64_t g_ord_watch[2] = { ~0ull, ~0ull };
static volatile uint32_t g_ord_wseq[2], g_ord_wflush[2];
static int ord_write(const struct block_device *self, uint64_t lba, int n, const void *b) {
    (void)self;
    g_ord_seq++;
    if (lba == (uint64_t)T3_JH_BLOCK * T3_SPB) {
        int clear = ((const uint8_t *)b)[4] == 0;
        if (clear && g_ord_fail_clear) { g_ord_fail_clear = 0; return 0; }
        if (clear && g_ord_drop_clear) { g_ord_drop_clear = 0; g_ord_hdr = g_ord_seq; return 1; }
        g_ord_hdr = g_ord_seq;
    }
    for (int w = 0; w < 2; w++)
        if (g_ord_watch[w] >= lba && g_ord_watch[w] < lba + (uint64_t)n) {
            g_ord_wseq[w] = g_ord_seq;
            g_ord_wflush[w] = g_ord_flush;
        }
    return vol_rw(0, lba, n, 0, b);
}
static int ord_flush(const struct block_device *self) {
    (void)self;
    if (g_ord_fail_after == 0) return 0;
    if (g_ord_fail_after > 0) g_ord_fail_after--;
    g_ord_flush = ++g_ord_seq;
    return 1;
}
static const struct block_device SEQ_DEV = {
    .name = "t3seq", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = ord_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_FLUSH, .flush = ord_flush,
};

// NO REVOKE RECORDS: a table the last commit journaled, freed by a delete in
// that commit, could be reused while the cleared header sat in the device's
// cache -- and a crash replayed the old table over its new owner.
KTEST("tfs3", "a block the last commit journaled is not freed before its cleared header is flushed") {
    struct t3_state *sbi = join_mount_on(&SEQ_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write_range(a, 0, g_del_buf, 13 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t ino = 0;
    struct t3_inode node;
    int found = wrote && join_inode(sbi, "/a", &ino, &node) && node.ptrs[12];
    const struct mount *m = join_m();
    fs_exclusive_begin();   // nothing commits but the call below
    int staged = found && fs_write_range(a, 13 * T3_BLOCK, g_del_buf, T3_BLOCK) == 1 && fs_delete(a);
    mount_lock(m);
    int committed = staged && t3_txn_flush_deferred(sbi);
    // Freed by that commit: was a flush issued after its last header write?
    int fenced = g_ord_flush > g_ord_hdr;
    mount_unlock(m);
    fs_exclusive_end();
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(found && staged && committed);
    KTEST_ASSERT(fenced);
}

KTEST("tfs3", "a commit whose barrier fails after its header stops the volume") {
    struct t3_state *sbi = join_mount_on(&SEQ_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    del_fill();
    g_ord_fail_after = 1;   // barrier 0 holds, barrier 1 does not
    int wrote = fs_write_range(JOIN_POINT "/a", 0, g_del_buf, 2 * T3_BLOCK) == 1;
    g_ord_fail_after = -1;
    int committed_on_disk = g_vol[0][(uint64_t)T3_JH_BLOCK * T3_BLOCK + 4];
    int ro = sbi->readonly;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(!wrote);
    KTEST_ASSERT(!committed_on_disk || ro);
}

KTEST("tfs3", "a commit that aborts re-arms the writeback timer") {
    struct t3_state *sbi = join_mount_on(&SEQ_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    fs_exclusive_begin();
    int wrote = fs_write_range(JOIN_POINT "/a", 0, g_del_buf, 2 * T3_BLOCK) == 1;
    const struct mount *m = join_m();
    mount_lock(m);
    sbi->txn_staged_tick = 0;   // long due
    g_ord_fail_after = 0;
    int failed = wrote && sbi->txn_deferred && !t3_txn_flush_deferred(sbi);
    g_ord_fail_after = -1;
    int due = t3_commit_due(sbi);
    mount_unlock(m);
    fs_exclusive_end();
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && failed);
    KTEST_ASSERT(!due);
}

// ---- the third whole-TFS3 review (2026-11-04) ---------------------------

static int join_set_size(struct t3_state *sbi, uint64_t ino, uint64_t size) {
    const struct mount *m = join_m();
    mount_lock(m);
    struct t3_inode node;
    int ok = t3_read_inode(sbi, ino, &node);
    node.size = size;
    ok = ok && t3_txn_begin(sbi, 1) && t3_txn_stage_inode(sbi, ino, &node) && t3_txn_commit(sbi);
    join_drop_caches(sbi);
    mount_unlock(m);
    return ok;
}

static int block_is(const char *path, uint32_t blk_index, const void *want) {
    static char back[T3_BLOCK];
    return fs_read_range(path, (uint64_t)blk_index * T3_BLOCK, back, T3_BLOCK) == T3_BLOCK &&
           k_memcmp(back, want, T3_BLOCK) == 0;
}

// A TABLE ALREADY MARKED IS NOT WALKED AGAIN, so what it maps was marked by
// nobody: one file's pointer naming another's table hid that file's blocks,
// and a repair reclaimed them. b comes first in the walk.
KTEST("tfs3", "fsck that meets a pointer table shared with another file reclaims nothing beneath it") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    const char *a = JOIN_POINT "/a";
    int made = fs_write_range(JOIN_POINT "/b", 0, g_del_buf, T3_BLOCK) == 1 &&
               fs_write_range(a, 0, g_del_buf, 13 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t aino = 0, bino = 0;
    struct t3_inode an, bn;
    int found = made && join_inode(sbi, "/a", &aino, &an) && an.ptrs[12] && join_inode(sbi, "/b", &bino, &bn);
    uint32_t real = found ? join_set_ptr(sbi, bino, 0, an.ptrs[12]) : 0;
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    int repaired = real && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    if (real) join_set_ptr(sbi, bino, 0, real);
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    int same = block_is(a, 12, g_del_buf + 12 * T3_BLOCK);
    join_unmount();

    KTEST_ASSERT(made && found && real && repaired);
    KTEST_ASSERT(r1.double_allocated >= 1);
    KTEST_ASSERT_EQ(r1.reclaimed, 0u);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
    KTEST_ASSERT(same);
}

// NO DOUBLES PROVES NOTHING WHEN PART OF THE TREE WENT UNREAD: b's data is
// a's "table", b sits in a folder whose block will not read, and the
// repair zeroed b's bytes.
KTEST("tfs3", "fsck that could not read part of the tree clears no word of a pointer table") {
    struct t3_state *sbi = join_mount_on(&FAULT_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    static uint8_t junk[T3_BLOCK];
    for (uint32_t i = 0; i < T3_BLOCK; i += 4) wr32(junk + i, 0xfffff000u);
    int made = fs_mkdir(JOIN_POINT "/d") && fs_write_range(JOIN_POINT "/d/b", 0, junk, T3_BLOCK) == 1 &&
               fs_write_range(JOIN_POINT "/a", 0, g_del_buf, 13 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t aino = 0, bino = 0, dino = 0;
    struct t3_inode an, bn, dn;
    int found = made && join_inode(sbi, "/a", &aino, &an) && an.ptrs[12] &&
                join_inode(sbi, "/d/b", &bino, &bn) && bn.ptrs[0] && join_inode(sbi, "/d", &dino, &dn) && dn.ptrs[0];
    int linked = found && join_set_ptr(sbi, aino, 12, bn.ptrs[0]) == an.ptrs[12];
    struct fs_check_result r1;
    k_memset(&r1, 0, sizeof r1);
    if (linked) g_bad_lba = sbi->vol.base_lba + (uint64_t)dn.ptrs[0] * T3_SPB;
    int repaired = linked && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    g_bad_lba = ~0ull;
    int intact = found && k_memcmp(vol_block(bn.ptrs[0]), junk, T3_BLOCK) == 0;
    if (linked) join_set_ptr(sbi, aino, 12, an.ptrs[12]);
    join_unmount();

    KTEST_ASSERT(made && found && linked && repaired);
    KTEST_ASSERT_EQ(r1.pointers_cleared, 0u);
    KTEST_ASSERT(intact);
}

// An inode lock appears while fsck's flush has the lock dropped -- as one
// let in by a commit wait would. The pass must see it before walking.
static struct t3_state *volatile g_hold_sbi;
static volatile int g_hold_arm, g_hold_live;
static volatile uint32_t g_hold_reads;
static int hold_read(const struct block_device *self, uint64_t lba, int n, void *b) {
    (void)self;
    if (g_hold_live) g_hold_reads++;
    return vol_rw(0, lba, n, b, 0);
}
static int hold_flush(const struct block_device *self) {
    (void)self;
    struct t3_state *sbi = g_hold_sbi;
    if (g_hold_arm && sbi) {
        g_hold_arm = 0;
        unsigned last = sizeof sbi->holds / sizeof sbi->holds[0] - 1;
        sbi->holds[last].ino = 999999;
        sbi->holds[last].pid = 0x7ffffff0;
        sbi->holds[last].depth = 1;
        sbi->holds[last].excl = 1;
        sbi->holds[last].used = 1;
        g_hold_live = 1;
    }
    return 1;
}
static const struct block_device HOLD_DEV = {
    .name = "t3hold", .sector_count = vol_count, .read_sectors = hold_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_FLUSH, .flush = hold_flush,
};
static void hold_release(void *arg) {
    volatile int *done = arg;
    uint64_t t0 = clocksource_now_ns();
    while (clocksource_now_ns() - t0 < 100000000ull)
        if (!scheduler_block_kernel_until(&g_slow_chan, SCHED_WAIT_DISK, t0 + 100000000ull)) cpu_relax();
    const struct mount *m = join_m();
    struct t3_state *sbi = g_hold_sbi;
    mount_lock(m);
    sbi->holds[sizeof sbi->holds / sizeof sbi->holds[0] - 1].used = 0;
    g_hold_live = 0;
    scheduler_wake(&sbi->ilock_chan, 0);
    mount_unlock(m);
    *done = 1;
}

KTEST("tfs3", "fsck waits out an op that took an inode while its flush waited") {
    struct t3_state *sbi = join_mount_on(&HOLD_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    int staged = fs_write(JOIN_POINT "/a", "a", 1) && sbi->txn_deferred;
    g_hold_sbi = sbi;
    g_hold_reads = 0;
    g_hold_arm = staged;
    static volatile int released;
    released = 0;
    int started = staged && kthread_start("ktest-hold", hold_release, (void *)&released) > 0;
    const struct mount *m = join_m();
    mount_lock(m);
    join_drop_caches(sbi);   // the walk reads the disk
    mount_unlock(m);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = started && fs_check_at(JOIN_POINT, 0, &r) == 0;
    int done = started && wait_flag(&released, 2000000000ull);
    uint32_t reads_while_held = g_hold_reads;
    g_hold_arm = 0;
    g_hold_sbi = 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(staged && started && checked && done);
    KTEST_ASSERT_EQ(reads_while_held, 0u);
}

static void side_rename(void *arg) {
    struct t3_side_op *o = arg;
    o->pid = scheduler_current_pid();
    o->ok = fs_rename(o->path, (const char *)o->buf);
    o->done = 1;
}

// THE ANCESTRY CHECK READS THE TREE BEFORE THE OP'S GAPS: a move let in by
// one put the destination under the source, and the two committed a loop
// cut off from `/`. The running transaction is filled so the first
// rename's t3_txn_begin() commits it -- with the lock dropped, on a slow
// flush -- and the second runs there.
KTEST("tfs3", "two folder moves at once cannot make a loop") {
    struct t3_state *sbi = join_mount_on(&SLOW_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    int made = fs_mkdir(JOIN_POINT "/p") && fs_mkdir(JOIN_POINT "/p/d") && fs_mkdir(JOIN_POINT "/p/d/sub") &&
               fs_mkdir(JOIN_POINT "/q") && fs_mkdir(JOIN_POINT "/q/r") && fs_sync_path(JOIN_POINT);
    const struct mount *m = join_m();
    int filled = 0;
    if (made) {
        mount_lock(m);
        int want = (int)sbi->jslots - 3;
        if (t3_txn_begin(sbi, want)) {
            for (uint32_t i = sbi->meta_off; i < T3_BPG && filled < want; i++)
                if (!t3_bbm_test(sbi, 0, i) && t3_txn_stage(sbi, t3_group_base(sbi, 0) + i)) filled++;
            t3_txn_stop(sbi);
        }
        mount_unlock(m);
    }
    static struct t3_side_op op;
    k_memset((void *)&op, 0, sizeof op);
    op.path = JOIN_POINT "/p/d";
    op.buf = JOIN_POINT "/q/r/d";
    int started = filled && kthread_start("ktest-mv", side_rename, &op) > 0;
    uint64_t t0 = clocksource_now_ns();
    int in_gap = 0;
    while (started && clocksource_now_ns() - t0 < 2000000000ull)
        if (__atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) == 1) { in_gap = 1; break; }
    int second = in_gap && fs_rename(JOIN_POINT "/q", JOIN_POINT "/p/d/sub/q");
    int done = started && wait_flag(&op.done, 5000000000ull);
    int synced = fs_sync_path(JOIN_POINT);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = fs_check_at(JOIN_POINT, 0, &r) == 0;
    int moved = fs_is_dir(JOIN_POINT "/q/r/d/sub");
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(made && filled && started);
    KTEST_ASSERT(in_gap);
    KTEST_ASSERT(done && op.ok);
    KTEST_ASSERT(!second);
    KTEST_ASSERT(synced && checked && moved);
    KTEST_ASSERT_EQ(fs_check_problems(&r), 0u);
}

// A RECLAIM IS A FREE: a leaked block the last commit wrote went back to
// the bitmap with that commit's cleared header still unflushed.
KTEST("tfs3", "fsck reclaims a block the last commit wrote only behind a flush") {
    struct t3_state *sbi = join_mount_on(&SEQ_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    const struct mount *m = join_m();
    mount_lock(m);
    uint32_t x = t3_alloc_block(sbi, 0, 0);
    int committed = x && t3_flush_alloc_state(sbi) && t3_txn_begin(sbi, 1) && t3_txn_stage(sbi, x) &&
                    t3_txn_commit(sbi);
    uint32_t hdr = g_ord_hdr;
    int unfenced = committed && g_ord_flush < hdr;
    mount_unlock(m);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int repaired = committed && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r) == 0;
    int fenced = g_ord_flush > hdr;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(committed && unfenced && repaired);
    KTEST_ASSERT(r.reclaimed >= 1);
    KTEST_ASSERT(fenced);
}

// A replayed transaction's targets are free to be reused once the mount is
// up, so its cleared header must be on the media first.
KTEST("tfs3", "a mount that replays the journal flushes its cleared header") {
    struct t3_state *sbi = join_mount_on(&SEQ_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    int made = fs_write(JOIN_POINT "/a", "", 0);
    g_ord_drop_clear = 1;   // the write's one commit: as a power cut would lose its clear
    int wrote = made && fs_write_range(JOIN_POINT "/a", 0, "a", 1) == 1;
    g_ord_drop_clear = 0;
    int committed = g_vol[0][(uint64_t)T3_JH_BLOCK * T3_BLOCK + 4] == 1;
    const char *why = 0;
    int down = mount_remove(JOIN_POINT, &why);
    int still = g_vol[0][(uint64_t)T3_JH_BLOCK * T3_BLOCK + 4] == 1;
    int up = down && mount_add(&SEQ_DEV, "tfs3", JOIN_POINT, 0, 0, &why);
    int cleared = g_vol[0][(uint64_t)T3_JH_BLOCK * T3_BLOCK + 4] == 0;
    int fenced = g_ord_flush > g_ord_hdr;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(wrote && committed && down && still && up && cleared);
    KTEST_ASSERT(fenced);
}

// THE CLEAR THAT DID NOT WRITE: the media says committed, so the commit's
// targets are kept -- a flush cannot put the missing header there.
KTEST("tfs3", "a block the last commit journaled is kept when its cleared header fails to write") {
    struct t3_state *sbi = join_mount_on(&SEQ_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    del_fill();
    const char *a = JOIN_POINT "/a";
    int wrote = fs_write_range(a, 0, g_del_buf, 13 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t ino = 0;
    struct t3_inode node;
    int found = wrote && join_inode(sbi, "/a", &ino, &node) && node.ptrs[12];
    uint32_t t = found ? node.ptrs[12] : 0;
    const struct mount *m = join_m();
    fs_exclusive_begin();
    int staged = found && fs_write_range(a, 13 * T3_BLOCK, g_del_buf, T3_BLOCK) == 1 && fs_delete(a);
    mount_lock(m);
    g_ord_fail_clear = 1;
    int committed = staged && t3_txn_flush_deferred(sbi);
    int failed = !g_ord_fail_clear;
    g_ord_fail_clear = 0;
    uint32_t g = t ? (t - sbi->group0) / T3_BPG : 0, i = t ? (t - sbi->group0) % T3_BPG : 0;
    int kept = t && t3_bbm_test(sbi, g, i);
    mount_unlock(m);
    fs_exclusive_end();
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(found && staged && committed && failed);
    KTEST_ASSERT(kept);
}

// `lazy` issues no barriers, and counted no clears: a switch to `strict`
// freed the last lazy commit's targets with its clear never flushed.
KTEST("tfs3", "after a switch from lazy to strict, the last lazy commit's blocks are freed behind a flush") {
    struct t3_state *sbi = join_mount_on(&SEQ_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(0, 0);
    const struct mount *m = join_m();
    mount_lock(m);
    uint32_t x = t3_alloc_block(sbi, 0, 0);
    int committed = x && t3_flush_alloc_state(sbi) && t3_txn_begin(sbi, 1) && t3_txn_stage(sbi, x) &&
                    t3_txn_commit(sbi);
    storage_config_set_mode_for_test(1, 0);
    uint32_t hdr = g_ord_hdr;
    if (committed) t3_free_block_bit(sbi, x);
    int fenced = g_ord_flush > hdr;
    t3_flush_alloc_state(sbi);
    mount_unlock(m);
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(committed);
    KTEST_ASSERT(fenced);
}

// Held inode numbers, read in the commit's gap.
static struct t3_state *volatile g_rec_sbi;
static volatile uint64_t g_rec_ino[64];
static volatile int g_rec_n;
static int rec_flush(const struct block_device *self) {
    (void)self;
    struct t3_state *sbi = g_rec_sbi;
    if (sbi && !g_rec_n)
        for (unsigned i = 0; i < sizeof sbi->holds / sizeof sbi->holds[0]; i++)
            if (sbi->holds[i].used && sbi->holds[i].excl) g_rec_ino[g_rec_n++] = sbi->holds[i].ino;
    return 1;
}
static const struct block_device REC_DEV = {
    .name = "t3rec", .sector_count = vol_count, .read_sectors = v0_read,
    .write_sectors = v0_write, .max_sectors_per_xfer = vol_xfer,
    .caps = BLK_CAP_FLUSH, .flush = rec_flush,
};

// An op let in by a mkdir's commit gap found the new folder through the
// committing overlay and staged into its block -- which a failed commit
// then freed. The new inode is held across the gap.
KTEST("tfs3", "a folder being made is locked across its commit") {
    struct t3_state *sbi = join_mount_on(&REC_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    g_rec_n = 0;
    g_rec_sbi = sbi;
    int made = fs_mkdir(JOIN_POINT "/n");
    g_rec_sbi = 0;
    uint64_t ino = 0;
    struct t3_inode node;
    int found = made && join_inode(sbi, "/n", &ino, &node);
    int held = 0;
    for (int i = 0; i < g_rec_n; i++) if (g_rec_ino[i] == ino) held = 1;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(made && found);
    KTEST_ASSERT(g_rec_n > 0);   // the flush ran in the gap, with the op's locks held
    KTEST_ASSERT(held);
}

// A rollback zeroes an existing table's pointers in place, then frees the
// blocks: without a flush between, the bitmap could land first.
KTEST("tfs3", "a rollback's cleared table reaches the disk before the bitmap that frees its blocks") {
    struct t3_state *sbi = join_mount_on(&SEQ_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    const struct mount *m = join_m();
    fs_exclusive_begin();
    mount_lock(m);
    uint32_t t = t3_alloc_block(sbi, 0, 0);
    int ok = t && t3_flush_alloc_state(sbi);
    t3_alog_begin(sbi);
    uint32_t b = ok ? t3_alloc_block(sbi, 0, 0) : 0;
    static uint8_t img[T3_BLOCK];
    k_memset(img, 0, sizeof img);
    wr32(img, b);
    ok = ok && b && t3_write_block(sbi, t, img);
    if (ok) t3_alog_note_table(sbi, t);
    g_ord_watch[0] = sbi->vol.base_lba + (uint64_t)t * T3_SPB;
    g_ord_watch[1] = sbi->vol.base_lba + (uint64_t)(t3_group_base(sbi, 0) + T3_BMAP_BLOCK) * T3_SPB;
    g_ord_wseq[0] = g_ord_wseq[1] = 0;
    t3_alog_rollback(sbi);
    uint32_t tseq = g_ord_wseq[0], bmseq = g_ord_wseq[1], bmflush = g_ord_wflush[1];
    g_ord_watch[0] = g_ord_watch[1] = ~0ull;
    int scrubbed = ok && rd32(vol_block(t)) == 0;
    if (t) t3_free_block_bit(sbi, t);
    t3_flush_alloc_state(sbi);
    mount_unlock(m);
    fs_exclusive_end();
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(ok && scrubbed);
    KTEST_ASSERT(tseq && bmseq > tseq);
    KTEST_ASSERT(bmflush > tseq);
}

// A FRESH TABLE ON A CACHE HIT kept the cached image of the block's last
// life: a table freed behind the cache's back (as fsck's reclaim did) and
// handed out again came back naming the old file's blocks.
KTEST("tfs3", "a new pointer table starts empty even when its block's old table is cached") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    int wrote = fs_write_range(JOIN_POINT "/x", 0, g_del_buf, 13 * T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    uint64_t xino = 0;
    struct t3_inode xn;
    int found = wrote && join_inode(sbi, "/x", &xino, &xn) && xn.ptrs[12];
    uint32_t l = found ? xn.ptrs[12] : 0, b12 = found ? rd32(vol_block(l)) : 0;
    const struct mount *m = join_m();
    fs_exclusive_begin();
    mount_lock(m);
    t3_pcache_drop(sbi);
    uint32_t leaf = 0, slot = 0, exist = 0;
    int loaded = found && t3_map_get_or_alloc_tables(sbi, &xn, 12, 0, &leaf, &slot, &exist) &&
                 leaf == l && exist == b12 && b12;
    t3_pcache_flush(sbi);
    uint32_t held = loaded ? hold_all_but(sbi, 0) : 0;
    uint32_t g = l ? (l - sbi->group0) / T3_BPG : 0, i = l ? (l - sbi->group0) % T3_BPG : 0;
    if (loaded) { t3_bbm_set(sbi, g, i, 0); sbi->gd[g].free_blocks++; }   // freed, still cached
    struct t3_inode y;
    k_memset(&y, 0, sizeof y);
    y.type = T3_TYPE_FILE;
    uint32_t yleaf = 0, yslot = 0, yexist = ~0u;
    t3_alog_begin(sbi);
    int got = loaded && t3_map_get_or_alloc_tables(sbi, &y, 12, 0, &yleaf, &yslot, &yexist);
    int reused = got && y.ptrs[12] == l;
    t3_alog_rollback(sbi);
    if (loaded && !t3_bbm_test(sbi, g, i)) { t3_bbm_set(sbi, g, i, 1); sbi->gd[g].free_blocks--; }   // x's again
    t3_pcache_drop(sbi);
    release_held(sbi, held);
    mount_unlock(m);
    fs_exclusive_end();
    join_unmount();

    KTEST_ASSERT(found && loaded && got && reused);
    KTEST_ASSERT_EQ(yexist, 0u);
}

// A STREAM'S INODE NUMBER IS NOT HANDED OUT: a delete and a create in the
// same second made a new empty file the stream took for its own.
KTEST("tfs3", "a stepped write does not finish into a new file given the deleted one's number") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    static uint8_t pa[2 * T3_BLOCK];
    k_memset(pa, 'a', sizeof pa);
    const char *a = JOIN_POINT "/a", *b = JOIN_POINT "/b";
    void *h = fs_write_range_begin(a, 0, pa, sizeof pa);
    enum fs_step_result first = h ? fs_write_range_step(h) : FS_STEP_FAILED;
    uint64_t aino = 0, bino = 0;
    struct t3_inode node;
    int swapped = join_inode(sbi, "/a", &aino, &node) && fs_delete(a) && fs_write(b, "", 0) &&
                  join_inode(sbi, "/b", &bino, &node);
    enum fs_step_result last = first;
    while (h && last == FS_STEP_PENDING) last = fs_write_range_step(h);
    uint64_t bsize = fs_size(b);
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(h && swapped);
    KTEST_ASSERT_EQ((int)first, (int)FS_STEP_PENDING);
    KTEST_ASSERT(bino != aino);
    KTEST_ASSERT_EQ((int)last, (int)FS_STEP_FAILED);
    KTEST_ASSERT_EQ(bsize, 0ull);
}

// AN ABANDONED STREAM'S BLOCKS STAY MAPPED PAST END-OF-FILE (its table rode
// another commit); a grow that does not write them read them back.
KTEST("tfs3", "an abandoned stepped write's blocks do not come back on a grow past them") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    del_fill();
    static uint8_t z[2 * T3_BLOCK], zero[T3_BLOCK];
    k_memset(z, 'z', sizeof z);
    k_memset(zero, 0, sizeof zero);
    const char *f = JOIN_POINT "/f", *g = JOIN_POINT "/g";
    int ok = 1;
    enum fs_step_result rf = FS_STEP_FAILED, rg = FS_STEP_FAILED, ef = FS_STEP_PENDING, eg = FS_STEP_PENDING;
    for (int k = 0; k < 2 && ok; k++) {
        const char *p = k ? g : f;
        ok = fs_write_range(p, 0, g_del_buf, 16 * T3_BLOCK) == 1;
        void *h = ok ? fs_write_range_begin(p, 16 * T3_BLOCK, z, sizeof z) : 0;
        enum fs_step_result r = h ? fs_write_range_step(h) : FS_STEP_FAILED;
        ok = ok && fs_sync_path(JOIN_POINT);   // its table lands
        // The grow -- a truncate, or a write past a gap -- then the stream again.
        ok = ok && (k ? fs_write_range(p, 30 * T3_BLOCK, "x", 1) == 1 : fs_truncate(p, 32 * T3_BLOCK) == 1);
        enum fs_step_result e = r;
        while (h && e == FS_STEP_PENDING) e = fs_write_range_step(h);
        if (k) { rg = r; eg = e; } else { rf = r; ef = e; }
    }
    int f_zero = ok && block_is(f, 16, zero), g_zero = ok && block_is(g, 16, zero);
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    int checked = fs_check_at(JOIN_POINT, 0, &r) == 0;
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ((int)rf, (int)FS_STEP_PENDING);
    KTEST_ASSERT_EQ((int)rg, (int)FS_STEP_PENDING);
    KTEST_ASSERT_EQ((int)ef, (int)FS_STEP_FAILED);
    KTEST_ASSERT_EQ((int)eg, (int)FS_STEP_FAILED);
    KTEST_ASSERT(f_zero);
    KTEST_ASSERT(g_zero);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(r.double_allocated + r.referenced_but_free, 0u);
}

// The mount went between two steps: every later step found a stale
// generation, and its 0 read as PENDING -- a loop on it spun forever.
KTEST("tfs3", "a stepped read on a volume unmounted between its steps fails") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    static char back[2 * T3_BLOCK];
    int wrote = fs_write_range(JOIN_POINT "/a", 0, g_del_buf, 2 * T3_BLOCK) == 1;
    void *h = wrote ? fs_read_range_begin(JOIN_POINT "/a", 0, back, sizeof back) : 0;
    uint32_t total = 0;
    enum fs_step_result first = h ? fs_read_range_step(h, &total) : FS_STEP_FAILED;
    const char *why = 0;
    int down = h && mount_remove(JOIN_POINT, &why);
    enum fs_step_result after = FS_STEP_PENDING;
    for (int i = 0; down && i < 3 && after == FS_STEP_PENDING; i++) after = fs_read_range_step(h, &total);
    join_unmount();

    KTEST_ASSERT(wrote && h && down);
    KTEST_ASSERT_EQ((int)first, (int)FS_STEP_PENDING);
    KTEST_ASSERT_EQ((int)after, (int)FS_STEP_FAILED);
}

// A begin with every stream slot taken was refused AFTER the backend began
// it: the stream stayed in tfs3's list for the life of the mount, and its
// inode number with it.
KTEST("tfs3", "a stepped read refused for want of a slot leaves nothing open") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    static char back[16][T3_BLOCK];
    int wrote = fs_write_range(JOIN_POINT "/a", 0, g_del_buf, 2 * T3_BLOCK) == 1;
    void *hs[16];
    int n = 0, refused = 0;
    for (int i = 0; wrote && i < 16 && !refused; i++) {
        hs[n] = fs_read_range_begin(JOIN_POINT "/a", 0, back[i], T3_BLOCK);
        if (hs[n]) n++; else refused = 1;
    }
    for (int i = 0; i < n; i++) {
        uint32_t total = 0;
        enum fs_step_result r = FS_STEP_PENDING;
        while (r == FS_STEP_PENDING) r = fs_read_range_step(hs[i], &total);
    }
    uint64_t ino = 0;
    struct t3_inode node;
    int found = join_inode(sbi, "/a", &ino, &node);
    const struct mount *m = join_m();
    mount_lock(m);
    int busy = found && t3_ino_busy(sbi, ino);
    mount_unlock(m);
    join_unmount();

    KTEST_ASSERT(wrote && refused && n > 0 && found);
    KTEST_ASSERT(!busy);
}

KTEST("tfs3", "a group counted full gives no block next to another, and its count does not wrap") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    const struct mount *m = join_m();
    fs_exclusive_begin();
    mount_lock(m);
    uint32_t x = t3_alloc_block(sbi, 0, 0);
    uint32_t saved = sbi->gd[0].free_blocks;
    sbi->gd[0].free_blocks = 0;   // as a torn descriptor mounts
    uint32_t y = x ? t3_alloc_block(sbi, 0, x) : 0;
    uint32_t after = sbi->gd[0].free_blocks;
    if (y) t3_bbm_set(sbi, (y - sbi->group0) / T3_BPG, (y - sbi->group0) % T3_BPG, 0);
    sbi->gd[0].free_blocks = saved;
    if (x) t3_free_block_bit(sbi, x);
    t3_flush_alloc_state(sbi);
    mount_unlock(m);
    fs_exclusive_end();
    join_unmount();

    KTEST_ASSERT(x != 0);
    KTEST_ASSERT_EQ(y, 0u);
    KTEST_ASSERT_EQ(after, 0u);
}

// A `.` naming a file counted as one of that file's names, and a repair
// "fixed" its link count to two: its blocks then outlived its last name.
KTEST("tfs3", "fsck repair takes no link count from a `.` that points away") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int made = fs_mkdir(JOIN_POINT "/d") && fs_write(JOIN_POINT "/f", "f", 1) && fs_sync_path(JOIN_POINT);
    uint64_t dino = 0, fino = 0;
    struct t3_inode dn, fn;
    int found = made && join_inode(sbi, "/d", &dino, &dn) && dn.ptrs[0] && join_inode(sbi, "/f", &fino, &fn);
    const struct mount *m = join_m();
    uint8_t *blk = found ? vol_block(dn.ptrs[0]) : 0;
    int at = blk ? dirent_at(blk, ".") : -1;
    if (at >= 0) {
        mount_lock(m);
        wr32(blk + at, (uint32_t)fino);
        join_drop_caches(sbi);
        mount_unlock(m);
    }
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    int repaired = at >= 0 && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    int read = found && join_inode(sbi, "/f", &fino, &fn);
    if (at >= 0) {
        mount_lock(m);
        wr32(blk + at, (uint32_t)dino);
        join_drop_caches(sbi);
        mount_unlock(m);
    }
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    join_unmount();

    KTEST_ASSERT(made && found && at >= 0 && repaired && read);
    KTEST_ASSERT(r1.dir_errors >= 1);
    KTEST_ASSERT_EQ((int)fn.links, 1);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
}

// A folder's block past what its size claims was marked, its names never
// read, and the pass not partial: a repair freed the files they named.
KTEST("tfs3", "fsck reclaims nothing a folder's blocks past its size name") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    del_fill();
    int made = fs_mkdir(JOIN_POINT "/d");
    uint64_t dino = 0;
    struct t3_inode dn;
    char path[96];
    for (int i = 0; made && i < 200; i++) {
        if (join_inode(sbi, "/d", &dino, &dn) && dn.size > T3_BLOCK) break;
        k_snprintf(path, sizeof path, JOIN_POINT "/d/a-rather-long-name-to-fill-the-block-%03d", i);
        made = fs_write(path, "", 0);
    }
    const char *g = JOIN_POINT "/d/g";
    made = made && dn.size > T3_BLOCK && fs_write_range(g, 0, g_del_buf, T3_BLOCK) == 1 && fs_sync_path(JOIN_POINT);
    int cut = made && join_inode(sbi, "/d", &dino, &dn) && join_set_size(sbi, dino, T3_BLOCK);
    struct fs_check_result r1, r2;
    k_memset(&r1, 0, sizeof r1);
    k_memset(&r2, 0, sizeof r2);
    int repaired = cut && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    if (cut) join_set_size(sbi, dino, dn.size);
    int checked = fs_check_at(JOIN_POINT, 0, &r2) == 0;
    int same = block_is(g, 0, g_del_buf);
    join_unmount();

    KTEST_ASSERT(made && cut && repaired);
    KTEST_ASSERT(r1.dir_errors >= 1);
    KTEST_ASSERT_EQ(r1.reclaimed, 0u);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r2), 0u);
    KTEST_ASSERT(same);
}

// Two folders sharing a block: repairing one's `..` rewrote the other's.
KTEST("tfs3", "fsck repair writes no `..` into a block another folder shares") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int made = fs_mkdir(JOIN_POINT "/d") && fs_mkdir(JOIN_POINT "/x") && fs_mkdir(JOIN_POINT "/x/e") &&
               fs_sync_path(JOIN_POINT);
    uint64_t dino = 0, eino = 0;
    struct t3_inode dn, en;
    int found = made && join_inode(sbi, "/d", &dino, &dn) && dn.ptrs[0] && join_inode(sbi, "/x/e", &eino, &en);
    uint32_t real = found ? join_set_ptr(sbi, eino, 0, dn.ptrs[0]) : 0;
    struct fs_check_result r1;
    k_memset(&r1, 0, sizeof r1);
    int repaired = real && fs_check_at(JOIN_POINT, FSCK_REPAIR, &r1) == 0;
    uint8_t *blk = found ? vol_block(dn.ptrs[0]) : 0;
    int at = blk ? dirent_at(blk, "..") : -1;
    uint32_t dotdot = at >= 0 ? rd32(blk + at) : 0;
    if (real) join_set_ptr(sbi, eino, 0, real);
    join_unmount();

    KTEST_ASSERT(made && found && real && repaired && at >= 0);
    KTEST_ASSERT(r1.double_allocated >= 1);
    KTEST_ASSERT_EQ(dotdot, (uint32_t)T3_INO_ROOT);
}

// The delete's own resolve left its name in the path cache, and an op let
// in by the commit's gap was told the name it was removing still existed.
KTEST("tfs3", "a name a strict delete is committing away can be made again meanwhile") {
    struct t3_state *sbi = join_mount_on(&SLOW_DEV);
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 0);
    const char *b = JOIN_POINT "/b";
    int setup = fs_write(b, "gone soon", 0);
    static struct t3_side_op op;
    k_memset((void *)&op, 0, sizeof op);
    op.path = b;
    int started = setup && kthread_start("ktest-rm", side_delete, &op) > 0;
    uint64_t t0 = clocksource_now_ns();
    int in_flight = 0;
    while (started && clocksource_now_ns() - t0 < 2000000000ull)
        if (__atomic_load_n(&sbi->committing, __ATOMIC_ACQUIRE) == 1) { in_flight = 1; break; }
    int made = in_flight && fs_mkdir(b);
    int done = started && wait_flag(&op.done, 2000000000ull);
    int is_dir = fs_is_dir(b);
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(setup && in_flight);
    KTEST_ASSERT(done && op.ok);
    KTEST_ASSERT(made && is_dir);
}

KTEST("tfs3", "a read-only volume's staged work is not due for a commit") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);
    fs_exclusive_begin();
    int wrote = fs_write(JOIN_POINT "/a", "a", 1);
    const struct mount *m = join_m();
    mount_lock(m);
    sbi->txn_staged_tick = 0;   // long due
    int due_rw = wrote && t3_commit_due(sbi);
    sbi->readonly = 1;
    int due_ro = t3_commit_due(sbi);
    sbi->readonly = 0;
    t3_txn_flush_deferred(sbi);
    mount_unlock(m);
    fs_exclusive_end();
    storage_config_set_mode_for_test(was_strict, was_batched);
    join_unmount();

    KTEST_ASSERT(due_rw);
    KTEST_ASSERT(!due_ro);
}

KTEST("tfs3", "a pointer naming the volume's own bitmap frees nothing") {
    struct t3_state *sbi = join_mount();
    if (!sbi) { join_unmount(); KTEST_SKIP("could not mount a 4 MiB volume"); }
    const struct mount *m = join_m();
    fs_exclusive_begin();
    mount_lock(m);
    uint32_t before = sbi->gd[0].free_blocks;
    t3_free_block_bit(sbi, t3_group_base(sbi, 0) + T3_BMAP_BLOCK);
    uint32_t after = sbi->gd[0].free_blocks;
    int still = t3_bbm_test(sbi, 0, T3_BMAP_BLOCK);
    if (!still) t3_bbm_set(sbi, 0, T3_BMAP_BLOCK, 1);
    sbi->gd[0].free_blocks = before;
    t3_flush_alloc_state(sbi);
    mount_unlock(m);
    fs_exclusive_end();
    join_unmount();

    KTEST_ASSERT_EQ(after, before);
    KTEST_ASSERT(still);
}
