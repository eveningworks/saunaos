// The disk cache's own rules, on a FAKE disk: its address is the key, and
// nothing real is ever registered there, so these never touch a real
// disk's pages. The live cache is shrunk to 4 MiB for the scan test and
// put back to what it was -- it holds nothing that is not on the disk.
#include "ktest.h"
#include "block.h"
#include "block_cache.h"
#include "heap.h"
#include "string.h"

static const struct block_device g_fake;   // never registered; only an address
#define D (&g_fake)

static uint8_t g_a[16 * 512], g_b[16 * 512];

static void pattern(uint8_t *p, int sectors, uint8_t seed) {
    for (int i = 0; i < sectors * 512; i++) p[i] = (uint8_t)(seed + i * 7 + i / 512);
}

static int off(void) {
    struct bcache_stats st;
    bcache_stats(&st);
    return !st.on;
}
#define NEED_CACHE() do { if (off()) KTEST_SKIP("nodiskcache: the cache is off"); } while (0)

static void fill(uint64_t lba, int n, const void *buf) {
    bcache_fill(D, lba, n, buf, bcache_ticket(D, lba, n));
}

KTEST("blkcache", "a filled range is read back from memory, byte for byte") {
    NEED_CACHE();
    bcache_forget(D);
    pattern(g_a, 16, 1);
    fill(1000, 16, g_a);
    k_memset(g_b, 0, sizeof g_b);
    KTEST_ASSERT(bcache_read(D, 1000, 16, g_b));
    KTEST_ASSERT_EQ(k_memcmp(g_a, g_b, sizeof g_a), 0);
    // A sub-range, starting mid-page.
    k_memset(g_b, 0, sizeof g_b);
    KTEST_ASSERT(bcache_read(D, 1003, 6, g_b));
    KTEST_ASSERT_EQ(k_memcmp(g_a + 3 * 512, g_b, 6 * 512), 0);
    bcache_forget(D);
}

KTEST("blkcache", "a range with ONE sector never read is a miss") {
    NEED_CACHE();
    bcache_forget(D);
    pattern(g_a, 16, 2);
    fill(2003, 5, g_a);                        // sectors 3..7 of page 250
    KTEST_ASSERT(bcache_read(D, 2003, 5, g_b));
    KTEST_ASSERT(!bcache_read(D, 2002, 2, g_b));  // sector 2 was never read
    KTEST_ASSERT(!bcache_read(D, 2007, 2, g_b));  // sector 8 is the next page
    bcache_forget(D);
}

KTEST("blkcache", "a write is what the next read returns; a failed one drops the range") {
    NEED_CACHE();
    bcache_forget(D);
    pattern(g_a, 8, 3);
    fill(3000, 8, g_a);
    pattern(g_b, 8, 99);
    bcache_write_begin(D, 3002, 2);
    bcache_write_end(D, 3002, 2, g_b, 1);
    static uint8_t got[8 * 512];
    KTEST_ASSERT(bcache_read(D, 3000, 8, got));
    KTEST_ASSERT_EQ(k_memcmp(got, g_a, 2 * 512), 0);              // untouched
    KTEST_ASSERT_EQ(k_memcmp(got + 2 * 512, g_b, 2 * 512), 0);    // the write
    KTEST_ASSERT_EQ(k_memcmp(got + 4 * 512, g_a + 4 * 512, 4 * 512), 0);

    bcache_write_begin(D, 3005, 1);
    bcache_write_end(D, 3005, 1, g_b, 0);                          // the device refused
    KTEST_ASSERT(!bcache_read(D, 3005, 1, got));
    KTEST_ASSERT(bcache_read(D, 3000, 5, got));                    // the rest stays
    bcache_forget(D);
}

KTEST("blkcache", "a read that raced a write is NOT cached over it") {
    NEED_CACHE();
    bcache_forget(D);
    pattern(g_a, 8, 4);    // what the device returned to a read issued BEFORE the write
    pattern(g_b, 8, 5);    // what the write put there
    uint64_t ticket = bcache_ticket(D, 4000, 8);
    bcache_write_begin(D, 4000, 8);
    bcache_write_end(D, 4000, 8, g_b, 1);
    bcache_fill(D, 4000, 8, g_a, ticket);                          // late: must be dropped
    static uint8_t got[8 * 512];
    KTEST_ASSERT(bcache_read(D, 4000, 8, got));
    KTEST_ASSERT_EQ(k_memcmp(got, g_b, sizeof got), 0);
    bcache_forget(D);
}

KTEST("blkcache", "a trim and a forget leave nothing to read") {
    NEED_CACHE();
    bcache_forget(D);
    pattern(g_a, 8, 6);
    fill(5000, 8, g_a);
    bcache_write_begin(D, 5000, 8);
    bcache_write_end(D, 5000, 8, 0, 0);                            // a trim
    KTEST_ASSERT(!bcache_read(D, 5000, 1, g_b));
    fill(5000, 8, g_a);
    bcache_forget(D);
    KTEST_ASSERT(!bcache_read(D, 5000, 8, g_b));
}

KTEST("blkcache", "a page read twice survives a scan bigger than the cache") {
    NEED_CACHE();
    uint32_t was = bcache_budget_mib();
    bcache_set_budget_mib(4);                    // 1024 pages
    pattern(g_a, 8, 7);
    fill(0, 8, g_a);
    KTEST_ASSERT(bcache_read(D, 0, 8, g_b));     // its second use: now active
    struct bcache_stats st;
    bcache_stats(&st);
    uint32_t cap = st.cap_pages;
    for (uint64_t p = 1; p <= 2 * (uint64_t)cap; p++) fill(p * 8, 8, g_a);   // a one-off scan
    int kept = bcache_read(D, 0, 8, g_b);
    int scan_head_gone = !bcache_read(D, 8, 8, g_b);
    bcache_forget(D);
    bcache_set_budget_mib(was);
    KTEST_ASSERT(cap > 0);
    KTEST_ASSERT(kept);
    KTEST_ASSERT(scan_head_gone);
}

// ---- through block.c: a fake PERSISTENT disk and a partition on it ----------
//
// The cases above call the cache directly; these go through the block
// layer's own paths -- single writes, a batch, a trim, and a partition
// window -- and compare every read with the disk's backing memory, which
// is the truth the cache must never contradict. The window starts at
// sector 13, so its pages straddle the disk's.

#define FD_SECT 256
#define PART_BASE 13
static uint8_t g_mem[FD_SECT * 512];

static uint64_t fd_count(const struct block_device *self) { (void)self; return FD_SECT; }
static int fd_xfer(const struct block_device *self) { (void)self; return 64; }
static int fd_read(const struct block_device *self, uint64_t lba, int n, void *buf) {
    (void)self;
    if (lba + (uint64_t)n > FD_SECT) return 0;
    k_memcpy(buf, g_mem + lba * 512, (size_t)n * 512);
    return 1;
}
static int fd_write(const struct block_device *self, uint64_t lba, int n, const void *buf) {
    (void)self;
    if (lba + (uint64_t)n > FD_SECT) return 0;
    k_memcpy(g_mem + lba * 512, buf, (size_t)n * 512);
    return 1;
}
static int fd_trim(const struct block_device *self, uint64_t lba, uint32_t n) {
    (void)self;
    k_memset(g_mem + lba * 512, 0xEE, (size_t)n * 512);
    return 1;
}
static const struct block_device g_fd = {
    .name = "ktestdisk", .sector_count = fd_count, .read_sectors = fd_read,
    .write_sectors = fd_write, .max_sectors_per_xfer = fd_xfer,
    .persistent = 1, .caps = BLK_CAP_TRIM, .trim = fd_trim,
};

static uint64_t fp_count(const struct block_device *self) { (void)self; return FD_SECT - PART_BASE; }
static int fp_read(const struct block_device *self, uint64_t lba, int n, void *buf) {
    (void)self;
    return fd_read(&g_fd, lba + PART_BASE, n, buf);
}
static int fp_write(const struct block_device *self, uint64_t lba, int n, const void *buf) {
    (void)self;
    return fd_write(&g_fd, lba + PART_BASE, n, buf);
}
static int fp_trim(const struct block_device *self, uint64_t lba, uint32_t n) {
    (void)self;
    return fd_trim(&g_fd, lba + PART_BASE, n);
}
static const struct block_device g_fp = {
    .name = "ktestdiskp1", .sector_count = fp_count, .read_sectors = fp_read,
    .write_sectors = fp_write, .max_sectors_per_xfer = fd_xfer, .persistent = 1,
    .caps = BLK_CAP_TRIM, .trim = fp_trim,
};

static uint8_t g_rd[64 * 512], g_wr[64 * 512];

// Read [lba, lba+n) of `dev` twice -- the second must be a HIT -- and
// compare both with the backing memory at `base` + lba.
static int reads_true(const struct block_device *dev, uint64_t base, uint64_t lba, int n,
                      int *was_hit) {
    struct bcache_stats a, b;
    if (!blkdev_read_sectors(dev, lba, n, g_rd)) return 0;
    if (k_memcmp(g_rd, g_mem + (base + lba) * 512, (size_t)n * 512)) return 0;
    bcache_stats(&a);
    if (!blkdev_read_sectors(dev, lba, n, g_rd)) return 0;
    bcache_stats(&b);
    *was_hit = b.hits > a.hits;
    return k_memcmp(g_rd, g_mem + (base + lba) * 512, (size_t)n * 512) == 0;
}

KTEST("blkcache", "through the block layer: writes, a batch, a raw-disk write and trims never leave a stale read") {
    NEED_CACHE();
    pattern(g_mem, FD_SECT, 9);
    KTEST_ASSERT(blk_track(&g_fd, &g_fd, 0));
    KTEST_ASSERT(blk_track(&g_fp, &g_fd, PART_BASE));
    int hit = 0, all_hits = 1, ok = 1;

    // A plain write through the partition, then its read back.
    pattern(g_wr, 20, 50);
    ok &= blkdev_write_sectors(&g_fp, 5, 20, g_wr);
    ok &= reads_true(&g_fp, PART_BASE, 0, 40, &hit); all_hits &= hit;

    // The raw disk rewrites sectors the partition has cached.
    pattern(g_wr, 6, 60);
    ok &= blkdev_write_sectors(&g_fd, PART_BASE + 10, 6, g_wr);
    ok &= reads_true(&g_fp, PART_BASE, 0, 40, &hit); all_hits &= hit;

    // A batch of three through the partition -- the journal's path.
    static uint8_t b0[8 * 512], b1[3 * 512], b2[16 * 512];
    pattern(b0, 8, 70); pattern(b1, 3, 80); pattern(b2, 16, 90);
    struct blk_io io[3] = {
        { .lba = 2,  .count = 8,  .write = 1, .buf = b0 },
        { .lba = 30, .count = 3,  .write = 1, .buf = b1 },
        { .lba = 60, .count = 16, .write = 1, .buf = b2 },
    };
    ok &= blkdev_submit_batch(&g_fp, io, 3);
    ok &= reads_true(&g_fp, PART_BASE, 0, 64, &hit); all_hits &= hit;
    ok &= reads_true(&g_fp, PART_BASE, 60, 16, &hit); all_hits &= hit;

    // Trims, one and several.
    ok &= blkdev_trim(&g_fp, 3, 4);
    struct blk_range r[2] = { { .lba = 31, .count = 2 }, { .lba = 62, .count = 9 } };
    ok &= blkdev_trim_ranges(&g_fp, r, 2);
    ok &= reads_true(&g_fp, PART_BASE, 0, 64, &hit); all_hits &= hit;
    ok &= reads_true(&g_fd, 0, 0, 64, &hit); all_hits &= hit;

    blk_untrack(&g_fp);
    blk_untrack(&g_fd);   // a disk leaving: its pages are forgotten
    KTEST_ASSERT(ok);
    KTEST_ASSERT(all_hits);   // the second read of each was the CACHE's answer
}
