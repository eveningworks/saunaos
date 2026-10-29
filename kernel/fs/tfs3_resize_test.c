// Growing tfs3 (tfs3_resize.c) on a RAM volume whose size a test sets --
// the partition growing under it -- and which can LOSE POWER after any
// block it is handed: writes past the cut are dropped and reported done.
//
// WHAT A BROKEN GROW WOULD STILL PASS, and the check that stops it:
//  * the size changing and nothing else -- the old last group's tail and
//    old backup left marked in use: fsck after the grow (leaks, counts);
//  * a grow that moved or lost blocks: every file is re-read by content;
//  * new groups the allocator cannot reach: the volume is FILLED past
//    its old size, then checked again;
//  * state that only lived in memory: a remount, and a second grow;
//  * new backups never written, or the old one left to claim the disk:
//    the primary is wiped and the volume must mount from a backup at
//    the NEW size, and a device cut back to the OLD size must find none;
//  * a wrong write order: the grow is cut short after each of its
//    writes in turn, and every cut must mount and check clean at the old
//    size or the new -- and both outcomes must occur, or the cut never
//    crossed the commit.
#include "ktest.h"
#include "tfs3.h"
#include "tfs3_internal.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "mount.h"
#include "fs.h"
#include "kfmt.h"
#include "errno.h"

#define GROW_POINT "/var/tmp/.ktest_grow"
#define MIB_SECTORS 2048ull
#define STORE_SLOTS 16384u     // written 4 KiB blocks it can hold (64 MiB)

// ---- the volume: a hash of written blocks ----------------------------------

struct gblk { uint64_t blk; uint8_t *data; };
static struct gblk *g_tab;          // open addressing, STORE_SLOTS, blk+1 in use
static uint32_t g_used;
static int g_full;
static uint64_t g_sectors;          // what the "partition" holds now
static int64_t g_cut = -1;          // blocks written before the power goes; -1 never
static uint64_t g_written;          // blocks written since the counter was reset

static struct gblk *slot_of(uint64_t blk, int make) {
    uint32_t h = (uint32_t)(blk * 2654435761u) & (STORE_SLOTS - 1);
    for (uint32_t n = 0; n < STORE_SLOTS; n++, h = (h + 1) & (STORE_SLOTS - 1)) {
        if (g_tab[h].data && g_tab[h].blk == blk) return &g_tab[h];
        if (!g_tab[h].data) {
            if (!make) return 0;
            if (g_used + 1 >= STORE_SLOTS) { g_full = 1; return 0; }
            uint8_t *d = kmalloc(4096);
            if (!d) { g_full = 1; return 0; }
            k_memset(d, 0xA5, 4096);   // a previous filesystem's bytes
            g_tab[h].blk = blk;
            g_tab[h].data = d;
            g_used++;
            return &g_tab[h];
        }
    }
    g_full = 1;
    return 0;
}

static uint64_t gv_count(const struct block_device *self) { (void)self; return g_sectors; }

static int gv_read(const struct block_device *self, uint64_t lba, int count, void *buf) {
    (void)self;
    if (count <= 0 || lba + (uint64_t)count > g_sectors) return 0;
    uint8_t *out = buf;
    for (int k = 0; k < count; k++) {
        uint64_t s = lba + (uint64_t)k;
        struct gblk *b = slot_of(s / 8, 0);
        if (b) k_memcpy(out + (uint64_t)k * 512, b->data + (s % 8) * 512, 512);
        else k_memset(out + (uint64_t)k * 512, 0xA5, 512);
    }
    return 1;
}

static int gv_write(const struct block_device *self, uint64_t lba, int count, const void *buf) {
    (void)self;
    if (count <= 0 || lba + (uint64_t)count > g_sectors) return 0;
    const uint8_t *in = buf;
    uint64_t last = ~0ull;
    for (int k = 0; k < count; k++) {
        uint64_t s = lba + (uint64_t)k;
        if (s / 8 != last) {          // a block begins: the unit the power is cut at
            last = s / 8;
            g_written++;
        }
        if (g_cut >= 0 && (int64_t)g_written > g_cut) continue;   // the power is gone
        struct gblk *b = slot_of(s / 8, 1);
        if (!b) return 0;
        k_memcpy(b->data + (s % 8) * 512, in + (uint64_t)k * 512, 512);
    }
    return 1;
}

static int gv_xfer(const struct block_device *self) { (void)self; return 64; }

static const struct block_device GROW_DEV = {
    .name = "t3grow", .sector_count = gv_count, .read_sectors = gv_read,
    .write_sectors = gv_write, .max_sectors_per_xfer = gv_xfer,
};

static int store_open(void) {
    g_tab = kzalloc(sizeof *g_tab * STORE_SLOTS);
    g_used = 0;
    g_full = 0;
    g_cut = -1;
    return g_tab != 0;
}

static void store_clear(struct gblk *tab) {
    for (uint32_t i = 0; i < STORE_SLOTS; i++) kfree(tab[i].data);
}

static void store_close(void) {
    if (!g_tab) return;
    store_clear(g_tab);
    kfree(g_tab);
    g_tab = 0;
}

// A copy of everything written, to put back before each cut.
static struct gblk *store_snapshot(void) {
    struct gblk *s = kzalloc(sizeof *s * STORE_SLOTS);
    if (!s) return 0;
    for (uint32_t i = 0; i < STORE_SLOTS; i++) {
        if (!g_tab[i].data) continue;
        s[i].blk = g_tab[i].blk;
        s[i].data = kmalloc(4096);
        if (!s[i].data) { store_clear(s); kfree(s); return 0; }
        k_memcpy(s[i].data, g_tab[i].data, 4096);
    }
    return s;
}

static int store_restore(const struct gblk *s) {
    store_clear(g_tab);
    k_memset(g_tab, 0, sizeof *g_tab * STORE_SLOTS);
    g_used = 0;
    for (uint32_t i = 0; i < STORE_SLOTS; i++) {
        if (!s[i].data) continue;
        g_tab[i].blk = s[i].blk;
        g_tab[i].data = kmalloc(4096);
        if (!g_tab[i].data) return 0;
        k_memcpy(g_tab[i].data, s[i].data, 4096);
        g_used++;
    }
    return 1;
}

// ---- files whose bytes say which file they are ------------------------------

#define FILE_BYTES 6000u
static uint8_t g_buf[FILE_BYTES];

static void fill_pattern(uint32_t i) {
    for (uint32_t k = 0; k < FILE_BYTES; k++) g_buf[k] = (uint8_t)(i * 131u + k * 7u + (k >> 9));
}

static void file_path(char *out, size_t cap, uint32_t i) { k_snprintf(out, cap, GROW_POINT "/d%u/f%u", i / 64, i); }

// Writes files from `from` until the volume is full (or `upto`); how many.
static uint32_t fill(uint32_t from, uint32_t upto) {
    char path[64];
    uint32_t i = from;
    for (; i < upto; i++) {
        if (i % 64 == 0) {
            k_snprintf(path, sizeof path, GROW_POINT "/d%u", i / 64);
            if (!fs_mkdir(path) && !fs_is_dir(path)) break;
        }
        file_path(path, sizeof path, i);
        fill_pattern(i);
        if (!fs_write_range(path, 0, g_buf, FILE_BYTES)) { fs_delete(path); break; }
    }
    return i;
}

// Every file 0..n-1 holds its own bytes.
static int verify(uint32_t n) {
    static uint8_t got[FILE_BYTES + 1];
    char path[64];
    for (uint32_t i = 0; i < n; i++) {
        file_path(path, sizeof path, i);
        fill_pattern(i);
        if (fs_read_into(path, got, sizeof got) != (int)FILE_BYTES) return 0;
        if (k_memcmp(got, g_buf, FILE_BYTES)) return 0;
    }
    return 1;
}

static int mount_it(void) {
    const char *why = 0;
    return mount_add(&GROW_DEV, "tfs3", GROW_POINT, 0, 0, &why);
}
static void unmount_it(void) {
    const char *why = 0;
    mount_remove(GROW_POINT, &why);
}

// Mounted: fsck finds nothing, and the volume's total is `total`.
static int clean_at(uint64_t *total) {
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    if (fs_check_at(GROW_POINT, 0, &r) != 0 || fs_check_problems(&r)) return 0;
    const struct mount *m = mount_resolve(GROW_POINT "/x", 0);
    uint64_t used = 0;
    return m && m->fs->disk_usage && m->fs->disk_usage(m->state, &used, total);
}

static int grow_to(uint64_t sectors, struct fs_resize_info *info) {
    g_sectors = sectors;
    return fs_resize_device(&GROW_DEV, 0, 0, info);
}

// Is the superblock at block `blk` a tfs3 one?
static int sb_at(uint64_t blk) {
    struct gblk *b = slot_of(blk, 0);
    return b && b->data[0] == 'T' && b->data[1] == 'F' && b->data[2] == 'S' && b->data[3] == '3';
}

static int prepare(void) {
    if (!fs_is_persistent()) return 0;
    fs_mkdir("/var");
    fs_mkdir("/var/tmp");
    fs_mkdir(GROW_POINT);
    return fs_is_dir(GROW_POINT);
}

KTEST("tfs3", "grow: a filled volume grows, fills, remounts and grows again") {
    if (!prepare()) KTEST_SKIP("no persistent root to mount on");
    if (!store_open()) KTEST_SKIP("could not allocate the store");
    struct fs_resize_info info;
    uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;
    int ok = 1, step = 0;
    uint32_t n0 = 0, n1 = 0, n2 = 0;

    // 4 MiB, one short group, filled to the last block.
    g_sectors = 4 * MIB_SECTORS;
    int formatted = fs_format_device(&GROW_DEV, "tfs3");
    if (formatted && mount_it()) {
        n0 = fill(0, 100000);
        ok = clean_at(&t0) && verify(n0);
        unmount_it();
    } else ok = 0;
    step = ok ? 1 : step;

    // 1. Within the group: 4 -> 6 MiB. The backup moves along it.
    int rc1 = ok ? grow_to(6 * MIB_SECTORS, &info) : -1;
    int old_backup_gone = !sb_at(4 * MIB_SECTORS / 8 - 1);
    if (ok && !rc1 && mount_it()) {
        ok = clean_at(&t1) && verify(n0);
        n1 = fill(n0, 100000);
        ok = ok && n1 > n0 && clean_at(&t1) && verify(n1);
        unmount_it();
        ok = ok && mount_it() && verify(n1) && clean_at(&t1);
        unmount_it();
    } else ok = 0;
    step = ok ? 2 : step;

    // 2. Past the group: 6 -> 300 MiB, three groups. Group 0 is whole now.
    int rc2 = ok ? grow_to(300 * MIB_SECTORS, &info) : -1;
    if (ok && !rc2 && mount_it()) {
        ok = clean_at(&t2) && verify(n1);
        n2 = fill(n1, n1 + 600);       // 3.5 MiB more: past the old size, not to the end
        ok = ok && n2 == n1 + 600 && clean_at(&t2) && verify(n2);
        unmount_it();
    } else ok = 0;
    step = ok ? 3 : step;

    // 3. Again: 300 -> 700 MiB. Group 1's backup lay inside the old volume.
    int rc3 = ok ? grow_to(700 * MIB_SECTORS, &info) : -1;
    if (ok && !rc3 && mount_it()) {
        ok = clean_at(&t3) && verify(n2);
        unmount_it();
        ok = ok && mount_it() && verify(n2) && clean_at(&t3);
        unmount_it();
    } else ok = 0;
    step = ok ? 4 : step;

    // 4. The backups: no primary, so the mount must find one at the NEW size.
    int from_backup = 0;
    uint64_t tb = 0;
    if (ok) {
        struct gblk *sb = slot_of(8, 0);
        uint8_t keep[512];
        if (sb) {
            k_memcpy(keep, sb->data, 512);
            k_memset(sb->data, 0, 512);
            from_backup = mount_it() && clean_at(&tb) && verify(n2);
            unmount_it();
            k_memcpy(sb->data, keep, 512);
        }
    }
    // ...and cut back to the size before the last grow, with no primary,
    // nothing may claim it: the old backup was erased.
    int stale = 1;
    if (ok) {
        struct gblk *sb = slot_of(8, 0);
        uint8_t keep[512];
        if (sb) {
            k_memcpy(keep, sb->data, 512);
            k_memset(sb->data, 0, 512);
            g_sectors = 300 * MIB_SECTORS;
            stale = mount_it();
            if (stale) unmount_it();
            g_sectors = 700 * MIB_SECTORS;
            k_memcpy(sb->data, keep, 512);
        }
    }
    int full = g_full;
    store_close();
    fs_delete(GROW_POINT);

    KTEST_ASSERT(!full);
    KTEST_ASSERT(formatted);
    KTEST_ASSERT(n0 > 100);              // the first volume really was filled
    KTEST_ASSERT_EQ(rc1, 0);
    KTEST_ASSERT(old_backup_gone);
    KTEST_ASSERT_EQ(rc2, 0);
    KTEST_ASSERT_EQ(rc3, 0);
    KTEST_ASSERT_EQ(step, 4);
    KTEST_ASSERT(t1 > t0 && t2 > t1 && t3 > t2);
    KTEST_ASSERT((n1 - n0) * 2ull * 4096 > (t1 - t0) * 3 / 4);   // the new 2 MiB was filled
    KTEST_ASSERT(from_backup);
    KTEST_ASSERT_EQ(tb, t3);
    KTEST_ASSERT(!stale);
}

// One grow, cut short after each of its block writes in turn. Every cut
// must leave a volume that mounts clean at `old_total` or `new_total`
// with every file intact; returns how many landed on each.
struct cut_result { int iterations, at_old, at_new, bad, writes; };

static void cut_every_write(uint64_t from_sectors, uint64_t to_sectors, uint32_t files,
                            struct cut_result *res) {
    k_memset(res, 0, sizeof *res);
    res->bad = -1;
    g_sectors = from_sectors;
    if (!fs_format_device(&GROW_DEV, "tfs3") || !mount_it()) return;
    uint32_t n = fill(0, files);
    uint64_t old_total = 0, new_total = 0;
    int ok = n == files && clean_at(&old_total);
    unmount_it();
    if (!ok) return;
    struct gblk *snap = store_snapshot();
    if (!snap) return;

    // The whole grow once, to count its writes and learn the new size.
    struct fs_resize_info info;
    g_written = 0;
    int rc = grow_to(to_sectors, &info);
    res->writes = (int)g_written;
    if (rc || !mount_it()) { store_clear(snap); kfree(snap); return; }
    clean_at(&new_total);
    unmount_it();

    res->bad = 0;
    for (int cut = 0; cut <= res->writes; cut++) {
        g_sectors = from_sectors;
        if (!store_restore(snap)) { res->bad++; break; }
        g_written = 0;
        g_cut = cut;
        grow_to(to_sectors, &info);
        g_cut = -1;
        uint64_t total = 0;
        int good = mount_it() && clean_at(&total) && verify(n);
        unmount_it();
        res->iterations++;
        if (good && total == old_total) res->at_old++;
        else if (good && total == new_total) res->at_new++;
        else {
            if (!res->bad) klog_printf("ktest: grow cut after %d of %d writes: %s, total %llu (old %llu, new %llu)\n",
                                       cut, res->writes, good ? "clean" : "NOT clean", (unsigned long long)total,
                                       (unsigned long long)old_total, (unsigned long long)new_total);
            res->bad++;
        }
    }
    store_clear(snap);
    kfree(snap);
}

KTEST("tfs3", "grow: cut short after any write, it mounts clean at the old size or the new") {
    if (!prepare()) KTEST_SKIP("no persistent root to mount on");
    if (!store_open()) KTEST_SKIP("could not allocate the store");
    // Within one group, the last group's bitmap on disk (it holds the root).
    struct cut_result a, b, c;
    cut_every_write(4 * MIB_SECTORS, 6 * MIB_SECTORS, 40, &a);
    // By less than a backup region: the new one overlaps the old.
    cut_every_write(4 * MIB_SECTORS, 4 * MIB_SECTORS + 64, 40, &b);
    // Three groups to six: group 1's backup is rewritten after the switch.
    cut_every_write(300 * MIB_SECTORS, 700 * MIB_SECTORS, 40, &c);
    int full = g_full;
    store_close();
    fs_delete(GROW_POINT);

    KTEST_ASSERT(!full);
    KTEST_ASSERT_EQ(a.bad, 0);
    KTEST_ASSERT_EQ(b.bad, 0);
    KTEST_ASSERT_EQ(c.bad, 0);
    KTEST_ASSERT(a.writes > 5 && b.writes > 5 && c.writes > 5);
    KTEST_ASSERT_EQ(a.iterations, a.writes + 1);
    KTEST_ASSERT(a.at_old > 0 && a.at_new > 0);
    KTEST_ASSERT(b.at_old > 0 && b.at_new > 0);
    KTEST_ASSERT(c.at_old > 0 && c.at_new > 0);
}

KTEST("tfs3", "grow: refusals -- shrinking, past the table, a mounted volume") {
    if (!prepare()) KTEST_SKIP("no persistent root to mount on");
    if (!store_open()) KTEST_SKIP("could not allocate the store");
    struct fs_resize_info info;
    g_sectors = 8 * MIB_SECTORS;
    int formatted = fs_format_device(&GROW_DEV, "tfs3");
    int info_rc = fs_resize_device(&GROW_DEV, 0, 1, &info);
    int can_grow = (info.flags & FS_RESIZE_CAN_GROW) != 0, can_shrink = (info.flags & FS_RESIZE_CAN_SHRINK) != 0;
    uint64_t max = info.max_bytes, size = info.size_bytes;
    int shrink = fs_resize_device(&GROW_DEV, 6 * MIB_SECTORS, 0, &info);
    // Past 512 GiB the descriptor table would grow: refused, untouched.
    g_sectors = 600ull * 1024 * MIB_SECTORS;
    int past = fs_resize_device(&GROW_DEV, 0, 0, &info);
    int still = mount_it();
    uint64_t total = 0;
    int clean = still && clean_at(&total);
    int busy = still ? fs_resize_device(&GROW_DEV, 0, 0, &info) : 0;
    if (still) unmount_it();
    int full = g_full;
    store_close();
    fs_delete(GROW_POINT);

    KTEST_ASSERT(!full);
    KTEST_ASSERT(formatted);
    KTEST_ASSERT_EQ(info_rc, 0);
    KTEST_ASSERT(can_grow && !can_shrink);
    KTEST_ASSERT_EQ(size, 8ull << 20);
    KTEST_ASSERT_EQ(max, (uint64_t)(T3_V3_GDT + 16u * 256u * T3_BPG) * T3_BLOCK);
    KTEST_ASSERT_EQ(shrink, -ENOTSUP);
    KTEST_ASSERT_EQ(past, -ENOSPC);
    KTEST_ASSERT(clean);
    KTEST_ASSERT(total > 0 && total < (16ull << 20));   // mounted, and still the old size
    KTEST_ASSERT_EQ(busy, -EBUSY);
}
