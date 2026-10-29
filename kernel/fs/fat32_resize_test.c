// Growing FAT32 (fat32_resize.c) on a RAM volume whose size a test sets --
// the partition growing under it -- driven through the backend directly,
// as fat32_test.c drives it, on scratch states of its own.
//
// WHAT A BROKEN GROW WOULD STILL PASS, and the check that stops it:
//  * the size changing and the FATs not -- a FAT too short for the new
//    clusters: probe() refuses it, and the new space is FILLED;
//  * a cluster left behind by the shift, or moved front to back over a
//    neighbour: every file is re-read by content, and the volume is
//    written with HOLES (files deleted between others) so the used
//    clusters are several runs, not one;
//  * a FAT copy left stale, or entries past the old end left as junk:
//    the copies are compared byte for byte, and junk is planted there;
//  * FSInfo's free count left at the old size: it is compared with a
//    count of the FAT;
//  * the backup boot sector left naming the old layout: compared with
//    the primary;
//  * state that only lived in memory: a remount, and a second grow;
//  * 512-byte arithmetic on a 4K-sector disk: the shift is run on one.
//
// No power-cut test, unlike tfs3's: a shift cut short is corrupt by
// design (fat32_resize.c says where), so there is nothing to assert.
#include "ktest.h"
#include "fat32.h"
#include "fat32_internal.h"
#include "fs.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "kfmt.h"
#include "mount.h"
#include "errno.h"

#define MAX_SECTORS 65536u          // 32 MiB, the largest a test grows to
#define PAGES (MAX_SECTORS / 8u)

// ---- the volume: 4 KiB pages, allocated as they are written ---------------------
//
// One never written reads as a previous filesystem's bytes, as a
// partition's free space would.

static uint8_t **g_pages;
static uint32_t g_sectors;          // what the "partition" holds now
static uint32_t g_block;            // the device's block size; 0 = 512

static uint8_t *page(uint32_t pg, int make) {
    if (!g_pages[pg] && make) {
        g_pages[pg] = kmalloc(4096);
        if (!g_pages[pg]) return 0;
        k_memset(g_pages[pg], 0xA5, 4096);
    }
    return g_pages[pg];
}

static int sectors_io(uint64_t lba, int count, void *buf, int write) {
    if (count <= 0 || lba + (uint64_t)count > MAX_SECTORS) return 0;
    for (int i = 0; i < count; i++) {
        uint64_t s = lba + (uint64_t)i;
        uint8_t *mem = (uint8_t *)buf + (uint64_t)i * 512;
        uint8_t *pg = page((uint32_t)(s / 8), write);
        if (write && !pg) return 0;
        if (write) k_memcpy(pg + (s % 8) * 512, mem, 512);
        else if (pg) k_memcpy(mem, pg + (s % 8) * 512, 512);
        else k_memset(mem, 0xA5, 512);
    }
    return 1;
}

static uint64_t gv_count(const struct block_device *self) { (void)self; return g_sectors; }

static int gv_read(const struct block_device *self, uint64_t lba, int count, void *buf) {
    (void)self;
    return lba + (uint64_t)count <= g_sectors && sectors_io(lba, count, buf, 0);
}

static int gv_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    (void)self;
    return lba + (uint64_t)count <= g_sectors && sectors_io(lba, count, (void *)buf, 1);
}

static int gv_xfer(const struct block_device *self) { (void)self; return 64; }

// Not const: block_size is set per test.
static struct block_device GROW_DEV = {
    .name = "fatgrow", .sector_count = gv_count, .read_sectors = gv_read,
    .write_sectors = gv_write, .max_sectors_per_xfer = gv_xfer,
};

// One byte of the volume, for planting what another system left.
static void poke(uint64_t byte, uint8_t v) {
    uint8_t *pg = page((uint32_t)(byte / 4096), 1);
    if (pg) pg[byte % 4096] = v;
}

static uint8_t peek(uint64_t byte) {
    uint8_t *pg = page((uint32_t)(byte / 4096), 0);
    return pg ? pg[byte % 4096] : 0xA5;
}

static uint32_t peek32(uint64_t byte) {
    return (uint32_t)peek(byte) | (uint32_t)peek(byte + 1) << 8 | (uint32_t)peek(byte + 2) << 16 |
           (uint32_t)peek(byte + 3) << 24;
}

static int same_sectors(uint64_t a, uint64_t b, uint32_t n) {
    static uint8_t x[512], y[512];
    for (uint32_t i = 0; i < n; i++)
        if (!sectors_io(a + i, 1, x, 0) || !sectors_io(b + i, 1, y, 0) || k_memcmp(x, y, 512)) return 0;
    return 1;
}

static const struct fs_ops *F(void) { return &fat32_ops; }

static void vol_close(void) {
    if (g_pages)
        for (uint32_t i = 0; i < PAGES; i++) kfree(g_pages[i]);
    kfree(g_pages);
    g_pages = 0;
    GROW_DEV.block_size = 0;
}

// A fresh volume of `sectors` on a device of `block` bytes, formatted.
static int vol_open(uint32_t sectors, uint32_t block) {
    vol_close();
    g_pages = kzalloc(sizeof *g_pages * PAGES);
    if (!g_pages) return 0;
    g_sectors = sectors;
    g_block = block;
    GROW_DEV.block_size = block;
    struct fs_scratch sc;
    if (!mount_scratch_begin(F(), &sc)) return 0;
    int ok = F()->format(sc.st, &GROW_DEV);
    mount_scratch_end(&sc);
    return ok;
}

// ---- the files ------------------------------------------------------------------

static uint8_t pat(uint32_t i, uint32_t o) { return (uint8_t)(i * 31u + o * 7u + (o >> 9)); }

static void name_of(char *out, uint32_t n, uint32_t i) {
    // Long names, some past ASCII, in a directory that outgrows a cluster.
    static const char *const stem[] = { "Käse", "日本語", "plain", "Ünïcödé name" };
    k_snprintf(out, n, "/files/%s %u.txt", stem[i % 4], i);
}

static uint32_t size_of(uint32_t i) { return 700u + (i * 1931u) % 6000u; }

#define IO_CHUNK 16384u
static uint8_t g_io[IO_CHUNK];

static int write_file(void *st, const char *path, uint32_t i, uint32_t size) {
    if (!F()->touch(st, path)) return 0;
    for (uint32_t o = 0; o < size; o += IO_CHUNK) {
        uint32_t n = size - o < IO_CHUNK ? size - o : IO_CHUNK;
        for (uint32_t j = 0; j < n; j++) g_io[j] = pat(i, o + j);
        if (!F()->write_range(st, path, o, g_io, n)) return 0;
    }
    return 1;
}

static int file_ok(void *st, const char *path, uint32_t i, uint32_t size) {
    if (F()->size(st, path) != size) return 0;
    for (uint32_t o = 0; o < size; o += IO_CHUNK) {
        uint32_t n = size - o < IO_CHUNK ? size - o : IO_CHUNK;
        if (F()->read_range(st, path, o, g_io, n) != n) return 0;
        for (uint32_t j = 0; j < n; j++)
            if (g_io[j] != pat(i, o + j)) return 0;
    }
    return 1;
}

// `count` files, every third deleted again so the used clusters are runs
// with holes between them. Returns how many files were kept.
static uint32_t fill(void *st, uint32_t count) {
    char path[96];
    if (!F()->mkdir(st, "/files")) return 0;
    for (uint32_t i = 0; i < count; i++) {
        name_of(path, sizeof path, i);
        if (!write_file(st, path, i, size_of(i))) return 0;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) {
        name_of(path, sizeof path, i);
        if (i % 3 == 1) { if (!F()->del(st, path)) return 0; }
        else kept++;
    }
    return kept;
}

static uint32_t verify(void *st, uint32_t count) {
    char path[96];
    uint32_t ok = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (i % 3 == 1) continue;
        name_of(path, sizeof path, i);
        ok += (uint32_t)file_ok(st, path, i, size_of(i));
    }
    return ok;
}

// ---- the volume's own consistency ---------------------------------------------------

struct facts {
    int probes, check_clean, fats_equal, backup_equal, fsinfo_right;
    uint32_t clusters, first_data, fat_sectors, total;
};

static int count_free(void *ctx, uint32_t c, uint32_t v) {
    (void)c;
    if (v == 0) ++*(uint32_t *)ctx;
    return 0;
}

static void facts_of(struct facts *f) {
    k_memset(f, 0, sizeof *f);
    struct fs_scratch sc;
    if (!mount_scratch_begin(F(), &sc)) return;
    struct fat32_state *s = sc.st;
    f->probes = fat32_parse_bpb(s, &GROW_DEV) == 1;
    if (f->probes) {
        f->clusters = s->v.cluster_count;
        f->first_data = s->v.first_data_sector;
        f->fat_sectors = s->v.fat_sectors;
        f->total = s->v.total_sectors;
        f->fats_equal = 1;
        for (uint32_t i = 1; i < s->v.num_fats; i++)
            if (!same_sectors(s->v.reserved, s->v.reserved + i * s->v.fat_sectors, s->v.fat_sectors))
                f->fats_equal = 0;
        f->backup_equal = same_sectors(0, 6ull * s->v.k, 1);
        uint32_t free = 0;
        f->fsinfo_right = s->v.fsinfo_sector && fat32_scan(s, count_free, &free) == 0 &&
                          peek32((uint64_t)s->v.fsinfo_sector * 512 + 488) == free;
        if (F()->init(s, &GROW_DEV, 0) == 1) {
            struct fs_check_result r;
            f->check_clean = F()->check(s, 0, &r, 0) == 1 && !r.double_allocated && !r.out_of_range;
            F()->umount(s, &GROW_DEV);
        }
    }
    mount_scratch_end(&sc);
}

static int resize_to(uint32_t sectors, struct fs_resize_info *info) {
    struct fs_scratch sc;
    k_memset(info, 0, sizeof *info);
    if (!mount_scratch_begin(F(), &sc)) return -ENOMEM;
    int rc = F()->resize(sc.st, &GROW_DEV, sectors, info);
    mount_scratch_end(&sc);
    return rc;
}

// Mounts the volume on a fresh state as `var`, runs `body`, unmounts.
#define WITH_MOUNT(var, body)                                                    \
    do {                                                                         \
        struct fs_scratch sc_;                                                   \
        if (mount_scratch_begin(F(), &sc_)) {                                    \
            if (F()->init(sc_.st, &GROW_DEV, 0) == 1) {                          \
                void *var = sc_.st;                                              \
                body;                                                            \
                F()->umount(sc_.st, &GROW_DEV);                                  \
            }                                                                    \
            mount_scratch_end(&sc_);                                             \
        }                                                                        \
    } while (0)

// ---- the tests ------------------------------------------------------------------------

KTEST("fat32", "grow in place: the FATs already cover the new clusters, nothing moves") {
    if (!vol_open(1024, 0)) KTEST_SKIP("could not allocate the test volume");
    uint32_t kept = 0, verified = 0, big_ok = 0;
    WITH_MOUNT(st, kept = fill(st, 30));
    struct facts a, b, c;
    facts_of(&a);
    // Junk where an entry past the old last cluster lies, in both FATs:
    // another formatter need not have zeroed them, and the grow must.
    {
        uint64_t at = 32u * 512u + (uint64_t)(a.clusters + 3) * 4u;
        poke(at, 0x77);
        poke(at + (uint64_t)a.fat_sectors * 512u, 0x77);
    }
    struct fat_grow_plan p;
    struct fat_vol v = { .k = 1, .sectors_per_cluster = 1, .reserved = 32, .num_fats = 2,
                         .fat_sectors = a.fat_sectors, .total_sectors = a.total,
                         .first_data_sector = a.first_data, .cluster_count = a.clusters };
    int planned = fat32_grow_plan(&v, a.total + 20, &p);
    g_sectors = a.total + 20;
    struct fs_resize_info info;
    int rc = resize_to(g_sectors, &info);
    facts_of(&b);
    WITH_MOUNT(st, {
        verified = verify(st, 30);
        big_ok = write_file(st, "/new.bin", 99, 18u * 512u) && file_ok(st, "/new.bin", 99, 18u * 512u);
    });
    facts_of(&c);
    vol_close();

    KTEST_ASSERT_EQ(kept, 20);
    KTEST_ASSERT(planned && !p.moves);              // the fixture takes the in-place path
    KTEST_ASSERT_EQ(rc, 0);
    KTEST_ASSERT(info.flags & FS_RESIZE_CAN_GROW);
    KTEST_ASSERT(b.probes);
    KTEST_ASSERT_EQ(b.first_data, a.first_data);    // nothing moved
    KTEST_ASSERT_EQ(b.fat_sectors, a.fat_sectors);
    KTEST_ASSERT_EQ(b.clusters, a.clusters + 20);
    KTEST_ASSERT_EQ(b.total, a.total + 20);
    KTEST_ASSERT(b.fats_equal);
    KTEST_ASSERT(b.backup_equal);
    KTEST_ASSERT(b.fsinfo_right);
    KTEST_ASSERT(b.check_clean);
    KTEST_ASSERT_EQ(verified, 20);
    KTEST_ASSERT(big_ok);
    KTEST_ASSERT(c.fsinfo_right && c.check_clean && c.fats_equal);
}

// A filled volume grown with a shift, the new space filled, remounted,
// and grown again.
static void shift_case(struct ktest_ctx *ctx, uint32_t block, uint32_t first, uint32_t second, uint32_t third) {
    if (!vol_open(first, block)) KTEST_SKIP("could not allocate the test volume");
    const uint32_t big = (second - first) * 512u / 2;   // more than the old volume held in all
    uint32_t kept = 0, v1 = 0, v2 = 0, v3 = 0, filled_ok = 0;
    WITH_MOUNT(st, kept = fill(st, 60));
    struct facts a, b, c, d;
    facts_of(&a);
    g_sectors = second;
    struct fs_resize_info info;
    int rc1 = resize_to(second, &info);
    facts_of(&b);
    WITH_MOUNT(st, {
        v1 = verify(st, 60);
        filled_ok = write_file(st, "/big.bin", 7, big) && file_ok(st, "/big.bin", 7, big);
    });
    WITH_MOUNT(st, v2 = verify(st, 60));            // a remount
    facts_of(&c);
    g_sectors = third;
    int rc2 = resize_to(third, &info);
    facts_of(&d);
    WITH_MOUNT(st, v3 = verify(st, 60) + (uint32_t)file_ok(st, "/big.bin", 7, big));
    vol_close();

    KTEST_ASSERT_EQ(kept, 40);
    KTEST_ASSERT_EQ(rc1, 0);
    KTEST_ASSERT(b.probes);
    KTEST_ASSERT(b.fat_sectors > a.fat_sectors);    // the FATs grew...
    KTEST_ASSERT(b.first_data > a.first_data);      // ...and the data moved
    KTEST_ASSERT_EQ(b.total, second);
    KTEST_ASSERT(b.clusters > a.clusters);
    KTEST_ASSERT(b.fats_equal);
    KTEST_ASSERT(b.backup_equal);
    KTEST_ASSERT(b.fsinfo_right);
    KTEST_ASSERT(b.check_clean);
    KTEST_ASSERT_EQ(v1, 40);
    KTEST_ASSERT(filled_ok);
    KTEST_ASSERT_EQ(v2, 40);
    KTEST_ASSERT(c.fsinfo_right && c.check_clean);
    KTEST_ASSERT_EQ(rc2, 0);
    KTEST_ASSERT(d.first_data > c.first_data);
    KTEST_ASSERT_EQ(d.total, third);
    KTEST_ASSERT(d.fats_equal && d.backup_equal && d.fsinfo_right && d.check_clean);
    KTEST_ASSERT_EQ(v3, 41);
}

KTEST("fat32", "grow with a shift: bigger FATs, every used cluster moved, grown twice") {
    shift_case(ctx, 0, 1024, 4096, 8192);
}

// A 4 KiB FAT sector holds 1022 clusters' entries, so the shift needs a
// volume past 4 MiB of 4 KiB clusters.
KTEST("fat32", "grow with a shift on a 4K-sector disk") {
    shift_case(ctx, 4096, 2048, 16384, 65536);
}

KTEST("fat32", "grow plan: the cluster limit, 2 TiB, and a grow too small to gain a cluster") {
    struct fat_grow_plan p;
    // 32 KiB clusters, 1 TiB: the 32-bit sector count stops it.
    struct fat_vol big = { .k = 1, .sectors_per_cluster = 64, .reserved = 32, .num_fats = 2 };
    big.cluster_count = 33554000;
    big.fat_sectors = (big.cluster_count + 2 + 127) / 128;
    big.first_data_sector = 32 + 2 * big.fat_sectors;
    big.total_sectors = big.first_data_sector + big.cluster_count * 64;
    KTEST_ASSERT(fat32_grow_plan(&big, 0xFFFFFFFFull, &p));
    KTEST_ASSERT(p.moves);
    KTEST_ASSERT(p.clusters < FAT32_MAX_CLUSTERS);
    KTEST_ASSERT((uint64_t)(p.clusters + 2) * 4 <= (uint64_t)p.fat_sectors * 512);
    KTEST_ASSERT((uint64_t)p.first_data + (uint64_t)p.clusters * 64 <= p.total);
    KTEST_ASSERT(0xFFFFFFFFu - p.total < 64);       // the whole 2 TiB, less a cluster's slack
    KTEST_ASSERT_EQ((2u * (p.fat_sectors - big.fat_sectors)) % 8u, 0);   // the 4 KiB alignment kept
    // 512-byte clusters: the cluster limit stops it, short of 2 TiB.
    struct fat_vol small = { .k = 1, .sectors_per_cluster = 1, .reserved = 32, .num_fats = 2 };
    small.cluster_count = 130000;
    small.fat_sectors = (small.cluster_count + 2 + 127) / 128;
    small.first_data_sector = 32 + 2 * small.fat_sectors;
    small.total_sectors = small.first_data_sector + small.cluster_count;
    KTEST_ASSERT(fat32_grow_plan(&small, 0xFFFFFFFFull, &p));
    KTEST_ASSERT_EQ(p.clusters, FAT32_MAX_CLUSTERS);
    KTEST_ASSERT_EQ(p.total, p.first_data + FAT32_MAX_CLUSTERS);
    // A FAT exactly full: one more cluster's room needs a FAT sector more
    // per copy, which costs two -- no cluster gained, no grow. Three more
    // gain one.
    struct fat_vol full = { .k = 1, .sectors_per_cluster = 1, .reserved = 32, .num_fats = 2, .fat_sectors = 8 };
    full.cluster_count = 8 * 128 - 2;
    full.first_data_sector = 32 + 16;
    full.total_sectors = full.first_data_sector + full.cluster_count;
    KTEST_ASSERT_EQ(fat32_grow_plan(&full, full.total_sectors + 1, &p), 0);
    KTEST_ASSERT(fat32_grow_plan(&full, full.total_sectors + 3, &p));
    KTEST_ASSERT(p.moves);
    KTEST_ASSERT_EQ(p.clusters, full.cluster_count + 1);
}

KTEST("fat32", "grow refusals: a shrink, unmirrored FATs, a dirty volume, bad clusters under a shift") {
    if (!vol_open(1024, 0)) KTEST_SKIP("could not allocate the test volume");
    struct fs_resize_info info;
    struct facts a, b, c;
    facts_of(&a);
    g_sectors = 4096;
    int shrink = resize_to(900, &info);
    int info_rc = resize_to(0, &info);
    int info_flags = (int)info.flags;
    uint64_t size = info.size_bytes;

    poke(40, peek(40) | 0x80);                       // BPB_ExtFlags: mirroring off
    int unmirrored = resize_to(4096, &info);
    int unmirrored_why = info.why[0] && !info.flags;
    poke(40, peek(40) & 0x7F);

    poke(65, peek(65) | 1);                          // Linux's dirty bit
    int dirty = resize_to(4096, &info);
    int dirty_why = info.why[0] != 0;
    poke(65, peek(65) & 0xFE);

    // The last cluster marked bad in both FATs: a shift is refused,
    // growing in place is not.
    for (uint32_t i = 0; i < 2; i++) {
        uint64_t at = 32u * 512u + (uint64_t)(a.clusters + 1) * 4u + (uint64_t)i * a.fat_sectors * 512u;
        poke(at, 0xF7); poke(at + 1, 0xFF); poke(at + 2, 0xFF); poke(at + 3, 0x0F);
    }
    int bad_shift = resize_to(4096, &info);
    facts_of(&b);
    int bad_in_place = resize_to(a.total + 10, &info);
    facts_of(&c);
    int same = resize_to(c.total, &info);
    vol_close();

    KTEST_ASSERT_EQ(shrink, -ENOTSUP);
    KTEST_ASSERT_EQ(info_rc, 0);
    KTEST_ASSERT_EQ(info_flags, FS_RESIZE_CAN_GROW | FS_RESIZE_UNSAFE);
    KTEST_ASSERT_EQ(size, (uint64_t)a.total * 512);
    KTEST_ASSERT_EQ(unmirrored, -ENOTSUP);
    KTEST_ASSERT(unmirrored_why);
    KTEST_ASSERT_EQ(dirty, -ENOTSUP);
    KTEST_ASSERT(dirty_why);
    KTEST_ASSERT_EQ(bad_shift, -ENOTSUP);
    KTEST_ASSERT_EQ(b.total, a.total);               // refused before anything was written
    KTEST_ASSERT_EQ(bad_in_place, 0);
    KTEST_ASSERT_EQ(c.total, a.total + 10);
    KTEST_ASSERT_EQ(same, 0);
}
