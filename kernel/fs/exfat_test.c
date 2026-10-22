// KTESTs for the exFAT backend, on a RAM volume this file formats with
// the backend's own format(). What a broken version would still pass is
// the reason for each: a round trip that crosses clusters and a sector
// boundary, a name outside ASCII (UTF-8 here, UTF-16 on the disk), the
// zeros past ValidDataLength, a NoFatChain file forced onto a chain, and
// the backend's own check finding no cluster leaked or claimed twice
// after all of it. The host's fsck.exfat judges the same code on a real
// image: tools/exfat_hostcheck.py.
#include "ktest.h"
#include "exfat.h"
#include "fs.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "mount.h"

#define EX_SECTORS (64u * 2048u)   // 64 MiB: 4 KiB clusters
static uint8_t *g_img;

static uint64_t ex_count(const struct block_device *self) { (void)self; return EX_SECTORS; }
static int ex_read(const struct block_device *self, uint64_t lba, int count, void *buf) {
    (void)self;
    if (!g_img || count <= 0 || lba + (uint64_t)count > EX_SECTORS) return 0;
    k_memcpy(buf, g_img + lba * 512, (uint32_t)count * 512);
    return 1;
}
static int ex_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    (void)self;
    if (!g_img || count <= 0 || lba + (uint64_t)count > EX_SECTORS) return 0;
    k_memcpy(g_img + lba * 512, buf, (uint32_t)count * 512);
    return 1;
}
static int ex_xfer(const struct block_device *self) { (void)self; return 128; }

static const struct block_device EX_DEV = {
    .name = "extest",
    .sector_count = ex_count,
    .read_sectors = ex_read,
    .write_sectors = ex_write,
    .max_sectors_per_xfer = ex_xfer,
};

static const struct fs_ops *F(void) { return &exfat_ops; }
static struct fs_scratch g_sc;
#define ST g_sc.st

// Formatted and mounted, or 0 (a SKIP: no 64 MiB to spare).
static int fresh(void) {
    if (!g_img) g_img = kmalloc(EX_SECTORS * 512u);
    if (!g_img) return 0;
    k_memset(g_img, 0xA5, EX_SECTORS * 512u);   // not zeros: an unwritten byte must not pass for one
    if (!mount_scratch_begin(F(), &g_sc)) return 0;
    if (!F()->format(ST, &EX_DEV) || F()->probe(ST, &EX_DEV) != 1 || F()->init(ST, &EX_DEV, 0) != 1) {
        mount_scratch_end(&g_sc);
        return 0;
    }
    return 1;
}

static void done(void) {
    F()->umount(ST, &EX_DEV);
    mount_scratch_end(&g_sc);
}

static int clean(void) {
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    return F()->check(ST, 0, &r, 0) == 1 && r.leaked == 0 && r.double_allocated == 0;
}

struct listing { int n, saw_dir, saw_name; };
static void count_cb(void *ctx, const char *name, uint32_t size, int is_dir) {
    struct listing *l = ctx;
    (void)size;
    l->n++;
    if (is_dir && k_strcmp(name, "Kansio") == 0) l->saw_dir = 1;
    if (k_strcmp(name, "Käyttäjä ÄÖ ž.txt") == 0) l->saw_name = 1;
}

KTEST("exfat", "formats, mounts empty, and its own check is clean") {
    if (!fresh()) KTEST_SKIP("no 64 MiB for a test volume");
    struct listing l = { 0, 0, 0 };
    F()->list(ST, "/", count_cb, &l);
    uint64_t used = 0, total = 0;
    int du = F()->disk_usage(ST, &used, &total);
    int ok = clean();
    done();
    KTEST_ASSERT_EQ(l.n, 0);
    KTEST_ASSERT(du && total > 60ull << 20 && used < (1u << 20));
    KTEST_ASSERT(ok);
}

KTEST("exfat", "a file spanning clusters and sector edges reads back, and so does its name") {
    if (!fresh()) KTEST_SKIP("no 64 MiB for a test volume");
    enum { N = 3 * 4096 + 777 };
    static uint8_t data[N], back[N];
    for (int i = 0; i < N; i++) data[i] = (uint8_t)(i * 7 + (i >> 9));
    const char *name = "/Käyttäjä ÄÖ ž.txt";
    int wrote = F()->write_range(ST, name, 0, data, N);
    // A second write at an unaligned offset across a sector and a cluster.
    static uint8_t patch[600];
    for (int i = 0; i < 600; i++) patch[i] = (uint8_t)(0xC0 + i);
    int patched = F()->write_range(ST, name, 4096 - 300, patch, 600);
    k_memcpy(data + 4096 - 300, patch, 600);
    uint32_t got = F()->read_range(ST, name, 0, back, N);
    uint64_t size = F()->size(ST, name);
    struct listing l = { 0, 0, 0 };
    F()->list(ST, "/", count_cb, &l);
    // Case-insensitively, through the volume's up-case table.
    int upper = F()->exists(ST, "/KÄYTTÄJÄ äö Ž.TXT");
    int ok = clean();
    done();
    KTEST_ASSERT(wrote && patched);
    KTEST_ASSERT_EQ(size, (uint64_t)N);
    KTEST_ASSERT_EQ(got, (uint32_t)N);
    KTEST_ASSERT(k_memcmp(data, back, N) == 0);
    KTEST_ASSERT(l.saw_name);
    KTEST_ASSERT(upper);
    KTEST_ASSERT(ok);
}

KTEST("exfat", "a write past the end leaves zeros in the gap, and truncate frees clusters") {
    if (!fresh()) KTEST_SKIP("no 64 MiB for a test volume");
    static uint8_t buf[9000];
    int w1 = F()->write_range(ST, "/gap.bin", 0, "head", 4);
    int w2 = F()->write_range(ST, "/gap.bin", 8000, "tail", 4);
    uint32_t got = F()->read_range(ST, "/gap.bin", 0, buf, sizeof buf);
    int zeros = 1;
    for (int i = 4; i < 8000; i++) if (buf[i]) { zeros = 0; break; }
    uint64_t used_before = 0, used_after = 0;
    F()->disk_usage(ST, &used_before, 0);
    int t = F()->truncate(ST, "/gap.bin", 2);
    F()->disk_usage(ST, &used_after, 0);
    // Growing again: allocated, not written -- reads as zeros past the
    // valid length whatever the disk holds there.
    int g = F()->truncate(ST, "/gap.bin", 6000);
    uint32_t got2 = F()->read_range(ST, "/gap.bin", 0, buf, sizeof buf);
    int zeros2 = buf[0] == 'h' && buf[1] == 'e';
    for (int i = 2; i < 6000; i++) if (buf[i]) { zeros2 = 0; break; }
    int ok = clean();
    done();
    KTEST_ASSERT(w1 && w2);
    KTEST_ASSERT_EQ(got, 8004u);
    KTEST_ASSERT(k_memcmp(buf, "he", 2) == 0);
    KTEST_ASSERT(zeros);
    KTEST_ASSERT(t && used_after < used_before);
    KTEST_ASSERT(g && got2 == 6000u && zeros2);
    KTEST_ASSERT(ok);
}

KTEST("exfat", "directories nest, rename moves, delete frees, and a contiguous file can chain") {
    if (!fresh()) KTEST_SKIP("no 64 MiB for a test volume");
    int m1 = F()->mkdir(ST, "/Kansio");
    int m2 = F()->mkdir(ST, "/Kansio/sub");
    int w = F()->write(ST, "/Kansio/sub/a.txt", "alpha", 0);
    // A contiguous file that cannot grow in place: another file is
    // written straight after it, so growing it forces a FAT chain.
    static uint8_t blk[4096];
    k_memset(blk, 'x', sizeof blk);
    int c1 = F()->write_range(ST, "/run.bin", 0, blk, sizeof blk);
    int c2 = F()->write_range(ST, "/after.bin", 0, blk, sizeof blk);
    k_memset(blk, 'y', sizeof blk);
    int c3 = F()->write_range(ST, "/run.bin", 4096, blk, sizeof blk);
    static uint8_t back[8192];
    uint32_t got = F()->read_range(ST, "/run.bin", 0, back, sizeof back);
    int run_ok = got == 8192 && back[0] == 'x' && back[4095] == 'x' && back[4096] == 'y' && back[8191] == 'y';
    int r = F()->rename(ST, "/Kansio/sub/a.txt", "/b.txt");
    char text[8] = { 0 };
    uint32_t tn = F()->read_range(ST, "/b.txt", 0, text, 7);
    int gone = !F()->exists(ST, "/Kansio/sub/a.txt");
    int rm_full = F()->del(ST, "/Kansio");            // not empty: refused
    int rm_sub = F()->del(ST, "/Kansio/sub");
    struct listing l = { 0, 0, 0 };
    F()->list(ST, "/", count_cb, &l);
    int ok = clean();
    done();
    KTEST_ASSERT(m1 && m2 && w);
    KTEST_ASSERT(c1 && c2 && c3 && run_ok);
    KTEST_ASSERT(r && tn == 5 && k_strcmp(text, "alpha") == 0 && gone);
    KTEST_ASSERT(!rm_full && rm_sub);
    KTEST_ASSERT(l.saw_dir);
    KTEST_ASSERT(ok);
}

KTEST("exfat", "a directory that outgrows its cluster keeps every name") {
    if (!fresh()) KTEST_SKIP("no 64 MiB for a test volume");
    // 4 KiB holds 128 entries; a 30-letter name takes 4, so 60 files is two
    // clusters' worth -- the directory has to grow.
    int made = 0, found = 0;
    char p[64];
    F()->mkdir(ST, "/many");
    for (int i = 0; i < 60; i++) {
        k_strlcpy(p, "/many/a-rather-long-file-name-", sizeof p);
        int n = (int)k_strlen(p);
        p[n] = (char)('0' + i / 10); p[n + 1] = (char)('0' + i % 10); p[n + 2] = 0;
        made += F()->touch(ST, p);
    }
    for (int i = 0; i < 60; i++) {
        k_strlcpy(p, "/many/a-rather-long-file-name-", sizeof p);
        int n = (int)k_strlen(p);
        p[n] = (char)('0' + i / 10); p[n + 1] = (char)('0' + i % 10); p[n + 2] = 0;
        found += F()->exists(ST, p);
    }
    int ok = clean();
    done();
    KTEST_ASSERT_EQ(made, 60);
    KTEST_ASSERT_EQ(found, 60);
    KTEST_ASSERT(ok);
}
