// tfs3 v3 on a volume far past v2's 512 GiB: a 4 TiB device that exists
// only where it was written (a sparse store of written blocks), so a KTEST
// can format and mount a disk no test machine has.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, and the check that stops it:
//  * v2's fixed descriptor table formats this "fine" -- 4096 groups, 512
//    GiB, the rest silently unused. So the mounted SIZE must be ~4 TiB.
//  * an eager format writes every group's bitmaps and tables -- here,
//    gigabytes. So the device COUNTS the blocks it was handed.
//  * a lazy table that was never zeroed holds whatever was there; the
//    unwritten blocks read back as a previous filesystem's bytes (0xA5),
//    so an inode slot read before it was zeroed fails loudly.
#include "ktest.h"
#include "tfs3.h"
#include "tfs3_internal.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "mount.h"
#include "fs.h"
#include "kfmt.h"

#define BIG_SECTORS   (4ull << 40 >> 9)   // 4 TiB
#define BIG_SLOTS     4096u               // written 4 KiB blocks it can hold
#define BIG_POINT     "/var/tmp/.ktest_v3big"

struct big_block { uint64_t blk; uint8_t *data; };
static struct big_block *g_big;
static uint32_t g_big_n, g_big_writes;
static int g_big_full;

static uint8_t *big_find(uint64_t blk, int make) {
    for (uint32_t i = 0; i < g_big_n; i++)
        if (g_big[i].blk == blk) return g_big[i].data;
    if (!make) return 0;
    if (g_big_n == BIG_SLOTS) { g_big_full = 1; return 0; }
    uint8_t *d = kmalloc(4096);
    if (!d) { g_big_full = 1; return 0; }
    k_memset(d, 0xA5, 4096);
    g_big[g_big_n].blk = blk;
    g_big[g_big_n++].data = d;
    return d;
}

static uint64_t big_count(const struct block_device *self) { (void)self; return BIG_SECTORS; }

static int big_rw(uint64_t lba, int count, uint8_t *rd, const uint8_t *wr) {
    if (count <= 0 || lba + (uint64_t)count > BIG_SECTORS) return 0;
    for (int k = 0; k < count; k++) {
        uint64_t s = lba + (uint64_t)k;
        uint8_t *d = big_find(s / 8, wr != 0);
        if (wr) {
            if (!d) return 0;
            k_memcpy(d + (s % 8) * 512, wr + (uint64_t)k * 512, 512);
        } else if (d) {
            k_memcpy(rd + (uint64_t)k * 512, d + (s % 8) * 512, 512);
        } else {
            k_memset(rd + (uint64_t)k * 512, 0xA5, 512);   // a previous filesystem's bytes
        }
    }
    return 1;
}
static int big_read(const struct block_device *self, uint64_t lba, int n, void *b) { (void)self; return big_rw(lba, n, b, 0); }
static int big_write(const struct block_device *self, uint64_t lba, int n, const void *b) {
    (void)self;
    g_big_writes += (uint32_t)((n + 7) / 8);
    return big_rw(lba, n, 0, b);
}
static int big_xfer(const struct block_device *self) { (void)self; return 8; }

static const struct block_device BIG_DEV = {
    .name = "t3big", .sector_count = big_count, .read_sectors = big_read,
    .write_sectors = big_write, .max_sectors_per_xfer = big_xfer,
};

static void big_free(void) {
    for (uint32_t i = 0; i < g_big_n; i++) kfree(g_big[i].data);
    kfree(g_big);
    g_big = 0;
    g_big_n = 0;
}

KTEST("tfs3", "v3 formats a 4 TiB volume lazily and mounts all of it") {
    g_big = kzalloc(sizeof *g_big * BIG_SLOTS);
    if (!g_big) KTEST_SKIP("could not allocate the sparse store");
    g_big_n = g_big_writes = 0;
    g_big_full = 0;

    struct fs_scratch sc;
    int formatted = 0;
    if (mount_scratch_begin(&tfs3_ops, &sc)) {
        formatted = tfs3_ops.format(sc.st, &BIG_DEV);
        mount_scratch_end(&sc);
    }
    uint32_t format_writes = g_big_writes;

    fs_mkdir(BIG_POINT);
    const char *why = 0;
    int mounted = formatted && mount_add(&BIG_DEV, "tfs3", BIG_POINT, 0, 0, &why);
    uint64_t used = 0, total = 0;
    int sized = 0, files = 1, reread = 1, checked = 0;
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    if (mounted) {
        const struct mount *m = mount_resolve(BIG_POINT "/x", 0);
        sized = m && m->fs->disk_usage && m->fs->disk_usage(m->state, &used, &total);
        // Enough files to cross several inode-table blocks (32 slots each),
        // each one zeroed on first use.
        char path[64], body[24];
        for (int i = 0; i < 100 && files; i++) {
            k_snprintf(path, sizeof path, BIG_POINT "/f%d", i);
            k_snprintf(body, sizeof body, "file %d", i);
            files = fs_write(path, body, 0);
        }
        char got[24];
        for (int i = 0; i < 100 && files && reread; i++) {
            k_snprintf(path, sizeof path, BIG_POINT "/f%d", i);
            k_snprintf(body, sizeof body, "file %d", i);
            k_memset(got, 0, sizeof got);
            reread = fs_read_into(path, got, sizeof got - 1) > 0 && !k_strcmp(got, body);
        }
        checked = fs_check_at(BIG_POINT, 0, &r) == 0;
        mount_remove(BIG_POINT, &why);
    }
    // ...and again from the disk alone: what the first mount wrote back.
    int remounted = mounted && mount_add(&BIG_DEV, "tfs3", BIG_POINT, 0, 0, &why);
    int survived = 0;
    if (remounted) {
        char got[24] = {0};
        survived = fs_read_into(BIG_POINT "/f99", got, sizeof got - 1) > 0 && !k_strcmp(got, "file 99");
        mount_remove(BIG_POINT, &why);
    }
    fs_delete(BIG_POINT);
    int full = g_big_full;
    big_free();

    KTEST_ASSERT(!full);
    KTEST_ASSERT(formatted);
    // A few MiB: the descriptor table three times over, the journal, group 0.
    KTEST_ASSERT(format_writes < 1024);
    KTEST_ASSERT(mounted);
    KTEST_ASSERT(sized);
    KTEST_ASSERT(total > (3ull << 40));      // not v2's 512 GiB
    KTEST_ASSERT(files && reread);
    KTEST_ASSERT(checked);
    KTEST_ASSERT_EQ(fs_check_problems(&r), 0);
    KTEST_ASSERT(remounted && survived);
}

// A BLOCK PAST 2 TiB IS READ AND OVERWRITTEN WHERE IT IS. The data paths
// that drop the mount's lock (vol_read_run(), t3_vol_write_run()) carried
// the sector number in 32 bits, which wraps at 2 TiB: a run past it went
// to the same offset below it. t3_write_block()/t3_read_block() do not
// truncate, so each is checked against the other.
KTEST("tfs3", "v3 reads and overwrites a run past 2 TiB in place, not 2 TiB lower") {
    g_big = kzalloc(sizeof *g_big * BIG_SLOTS);
    if (!g_big) KTEST_SKIP("could not allocate the sparse store");
    g_big_n = g_big_writes = 0;
    g_big_full = 0;
    struct fs_scratch sc;
    int opened = 0, read_ok = 0, write_ok = 0;
    static uint8_t a[4096], b[4096];
    if (mount_scratch_begin(&tfs3_ops, &sc)) {
        struct t3_state *sbi = sc.st;
        if (tfs3_ops.format(sc.st, &BIG_DEV) && t3_open_volume(sbi, &BIG_DEV)) {
            opened = 1;
            const uint32_t hi = (1u << 29) + 100000u;   // 2 TiB + 100000 blocks
            for (int i = 0; i < 4096; i++) a[i] = (uint8_t)(i * 7 + 1);
            k_memset(b, 0, sizeof b);
            read_ok = t3_write_block(sbi, hi, a) && t3_read_block_unlocked(sbi, hi, b) == 1 &&
                      !k_memcmp(a, b, sizeof a);
            for (int i = 0; i < 4096; i++) a[i] = (uint8_t)(i * 13 + 5);
            k_memset(b, 0, sizeof b);
            write_ok = t3_vol_write_run(sbi, (uint64_t)(hi + 1) * T3_SPB, (int)T3_SPB, a) == 1 &&
                       t3_read_block(sbi, hi + 1, b) && !k_memcmp(a, b, sizeof a);
            t3_close_volume(sbi);
        }
        mount_scratch_end(&sc);
    }
    big_free();
    KTEST_ASSERT(opened);
    KTEST_ASSERT(read_ok);
    KTEST_ASSERT(write_ok);
}
