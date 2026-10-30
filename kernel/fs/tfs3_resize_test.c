// Resizing tfs3 (tfs3_resize.c) on a RAM volume whose size a test sets --
// the partition growing under it, or cut back after a shrink -- and which
// can LOSE POWER after any block it is handed: writes past the cut are
// dropped and reported done.
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

static struct t3_state *mounted_sbi(void) {
    const struct mount *m = mount_resolve(GROW_POINT "/x", 0);
    return m ? m->state : 0;
}

// The mounted volume's size in bytes -- its superblock's, not the data
// area clean_at() reports.
static uint64_t vol_bytes(void) {
    struct t3_state *sbi = mounted_sbi();
    return sbi ? (uint64_t)sbi->sb.total_blocks * T3_BLOCK : 0;
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

KTEST("tfs3", "resize: refusals -- below the floor, past the table, errors, a mounted volume") {
    if (!prepare()) KTEST_SKIP("no persistent root to mount on");
    if (!store_open()) KTEST_SKIP("could not allocate the store");
    struct fs_resize_info info;
    g_sectors = 8 * MIB_SECTORS;
    int formatted = fs_format_device(&GROW_DEV, "tfs3");
    int info_rc = fs_resize_device(&GROW_DEV, 0, 1, &info);
    int can_grow = (info.flags & FS_RESIZE_CAN_GROW) != 0, can_shrink = (info.flags & FS_RESIZE_CAN_SHRINK) != 0;
    int unsafe = (info.flags & FS_RESIZE_UNSAFE) != 0;
    uint64_t max = info.max_bytes, size = info.size_bytes, min = info.min_bytes;
    // Below what its files and metadata need: refused, untouched.
    int below = fs_resize_device(&GROW_DEV, MIB_SECTORS, 0, &info);
    // Past 512 GiB the descriptor table would grow: refused, untouched.
    g_sectors = 600ull * 1024 * MIB_SECTORS;
    int past = fs_resize_device(&GROW_DEV, 0, 0, &info);
    g_sectors = 8 * MIB_SECTORS;
    int still = mount_it();
    uint64_t total = 0;
    int clean = still && clean_at(&total);
    total = vol_bytes();
    int busy = still ? fs_resize_device(&GROW_DEV, 0, 0, &info) : 0;
    // A volume with a problem is not shrunk: one data block marked in use
    // that nothing names -- a leak, the mildest thing fsck reports.
    int leaked = 0;
    if (still) {
        const struct mount *m = mount_resolve(GROW_POINT "/x", 0);
        struct t3_state *sbi = m ? m->state : 0;
        uint32_t i = sbi ? t3_group_data_end(sbi, 0) - 1 : 0;
        if (sbi && !t3_bbm_test(sbi, 0, i)) {
            t3_bbm_set(sbi, 0, i, 1);
            sbi->gd[0].free_blocks--;
            t3_mark_dirty(sbi->gdt_dirty, 0);
            leaked = t3_flush_alloc_state(sbi);
        }
        unmount_it();
    }
    int with_errors = leaked ? fs_resize_device(&GROW_DEV, 4 * MIB_SECTORS, 0, &info) : 0;
    int said = k_strstr(info.why, "check and repair") != 0;
    uint64_t after = 0;
    int unchanged = mount_it() && (clean_at(&after), 1);
    after = vol_bytes();
    unmount_it();
    int full = g_full;
    store_close();
    fs_delete(GROW_POINT);

    KTEST_ASSERT(!full);
    KTEST_ASSERT(formatted);
    KTEST_ASSERT_EQ(info_rc, 0);
    KTEST_ASSERT(can_grow && can_shrink && !unsafe);   // journalled both ways
    KTEST_ASSERT_EQ(size, 8ull << 20);
    KTEST_ASSERT(min > MIB_SECTORS * 512 && min < size);
    KTEST_ASSERT_EQ(max, (uint64_t)(T3_V3_GDT + 16u * 256u * T3_BPG) * T3_BLOCK);
    KTEST_ASSERT_EQ(below, -ENOSPC);
    KTEST_ASSERT_EQ(past, -ENOSPC);
    KTEST_ASSERT(clean);
    KTEST_ASSERT_EQ(total, size);   // mounted, and still the old size
    KTEST_ASSERT_EQ(busy, -EBUSY);
    KTEST_ASSERT(leaked);
    KTEST_ASSERT_EQ(with_errors, -EIO);
    KTEST_ASSERT(said);
    KTEST_ASSERT(unchanged);
    KTEST_ASSERT_EQ(after, size);
}

// ---- shrinking ----------------------------------------------------------------
//
// A 300 MiB volume is three groups. Files land in group 2 the way they
// would on a full disk -- their folder's inode is there, so theirs and
// their data follow it -- by moving that folder's inode with the shrink's
// own renumbering (t3_move_inode_to_group()); fsck checks the result
// before anything is shrunk, so the renumbering is not trusted to grade
// itself. Then 300 -> 140 MiB drops group 2 and cuts group 1 short.
//
// WHAT A BROKEN SHRINK WOULD STILL PASS, and the check that stops it:
//  * blocks copied but a pointer left behind, or a table's children not
//    followed: every file re-read by content, one big enough for a
//    double-indirect table;
//  * an inode moved but a name, `.` or `..` left naming the old slot:
//    fsck (names, `..`, link counts) and every path read again;
//  * the new size written but the moves not durable: a remount at the
//    partition's NEW size, where the old blocks no longer exist;
//  * a wrong order: the shrink cut short after each of its writes, every
//    cut clean at the old size or the new.

#define FAR_SMALL 6
#define BIG_BYTES (5u << 20)      // past 12 + 1024 blocks: a double-indirect table
#define MANY_DIRS 40              // more `..`s than one journal step holds
static uint8_t g_chunk[65536];

static void big_pattern(uint64_t off, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) g_chunk[k] = (uint8_t)((off + k) * 2654435761u >> 13);
}

// Move folder `rel` (volume-relative) to group g. 1 on success.
static int push_dir(const char *rel, uint32_t g) {
    struct t3_state *sbi = mounted_sbi();
    uint64_t ino = 0;
    static char norm[T3_PATH_BUF];
    return sbi && t3_normalize(sbi, rel, norm) && t3_resolve(sbi, norm, &ino) &&
           t3_move_inode_to_group(sbi, ino, g) == 0;
}

static uint32_t group_of(const char *rel) {
    struct t3_state *sbi = mounted_sbi();
    uint64_t ino = 0;
    static char norm[T3_PATH_BUF];
    if (!sbi || !t3_normalize(sbi, rel, norm) || !t3_resolve(sbi, norm, &ino)) return ~0u;
    return (uint32_t)(ino / sbi->sb.ipg);
}

// The files past the end: small ones, a hard link with names on both
// sides of the cut, a subfolder, a `big`-byte file (5 MiB reaches a
// double-indirect table, 64 KiB a single one); with `many`, a folder of
// MANY_DIRS folders. Mounted; 1 on success.
static int make_far(uint32_t big, int many) {
    char path[64];
    if (!fs_mkdir(GROW_POINT "/far") || !push_dir("/far", 2)) return 0;
    if (!fs_mkdir(GROW_POINT "/far/sub")) return 0;
    for (uint32_t i = 0; i < FAR_SMALL; i++) {
        k_snprintf(path, sizeof path, GROW_POINT "/far/%s%u", i & 1 ? "sub/" : "", i);
        fill_pattern(1000 + i);
        if (!fs_write_range(path, 0, g_buf, FILE_BYTES)) return 0;
    }
    if (!fs_link(GROW_POINT "/far/0", GROW_POINT "/d0/far0") || !fs_link(GROW_POINT "/far/0", GROW_POINT "/far/sub/again"))
        return 0;
    if (big) {
        for (uint32_t off = 0; off < big; off += sizeof g_chunk) {
            big_pattern(off, sizeof g_chunk);
            if (!fs_write_range(GROW_POINT "/far/big", off, g_chunk, sizeof g_chunk)) return 0;
        }
    }
    if (many) {
        if (!fs_mkdir(GROW_POINT "/far/many")) return 0;
        for (uint32_t i = 0; i < MANY_DIRS; i++) {
            k_snprintf(path, sizeof path, GROW_POINT "/far/many/m%u", i);
            if (!fs_mkdir(path)) return 0;
        }
    }
    return group_of("/far/2") == 2 && group_of("/far/sub") == 2;
}

static int verify_far(uint32_t big, int many) {
    static uint8_t got[FILE_BYTES + 1];
    char path[64];
    for (uint32_t i = 0; i < FAR_SMALL; i++) {
        k_snprintf(path, sizeof path, GROW_POINT "/far/%s%u", i & 1 ? "sub/" : "", i);
        fill_pattern(1000 + i);
        if (fs_read_into(path, got, sizeof got) != (int)FILE_BYTES || k_memcmp(got, g_buf, FILE_BYTES)) return 0;
    }
    fill_pattern(1000);
    if (fs_read_into(GROW_POINT "/d0/far0", got, sizeof got) != (int)FILE_BYTES || k_memcmp(got, g_buf, FILE_BYTES))
        return 0;
    if (fs_read_into(GROW_POINT "/far/sub/again", got, sizeof got) != (int)FILE_BYTES || k_memcmp(got, g_buf, FILE_BYTES))
        return 0;
    if (big) {
        static uint8_t want[sizeof g_chunk];
        for (uint32_t off = 0; off < big; off += sizeof g_chunk) {
            if (fs_read_range(GROW_POINT "/far/big", off, want, sizeof want) != sizeof want) return 0;
            big_pattern(off, sizeof g_chunk);
            if (k_memcmp(want, g_chunk, sizeof want)) return 0;
        }
    }
    for (uint32_t i = 0; many && i < MANY_DIRS; i++) {
        k_snprintf(path, sizeof path, GROW_POINT "/far/many/m%u", i);
        if (!fs_is_dir(path)) return 0;
    }
    return 1;
}

static int shrink_to(uint64_t sectors, struct fs_resize_info *info) {
    return fs_resize_device(&GROW_DEV, sectors, 0, info);
}

KTEST("tfs3", "shrink: files and inodes past the new end come back below it") {
    if (!prepare()) KTEST_SKIP("no persistent root to mount on");
    if (!store_open()) KTEST_SKIP("could not allocate the store");
    struct fs_resize_info info;
    int ok = 0, step = 0, made = 0;
    uint32_t n = 0;
    uint64_t t0 = 0, t1 = 0, t2 = 0, tb = 0, b1 = 0, b2 = 0;

    g_sectors = 300 * MIB_SECTORS;
    if (fs_format_device(&GROW_DEV, "tfs3") && mount_it()) {
        n = fill(0, 40);
        made = make_far(BIG_BYTES, 1);
        ok = n == 40 && made && clean_at(&t0) && verify(n) && verify_far(BIG_BYTES, 1);
        unmount_it();
    }
    step = ok ? 1 : 0;

    // 1. 300 -> 140 MiB: group 2 goes, group 1 is cut short. The
    // filesystem first, then the "partition".
    int rc1 = ok ? shrink_to(140 * MIB_SECTORS, &info) : -1;
    uint64_t want1 = info.size_bytes;
    g_sectors = 140 * MIB_SECTORS;
    if (ok && !rc1 && mount_it()) {
        ok = clean_at(&t1) && verify(n) && verify_far(BIG_BYTES, 1) && group_of("/far/2") < 2 && group_of("/far/many/m7") < 2;
        b1 = vol_bytes();
        unmount_it();
        ok = ok && mount_it() && verify_far(BIG_BYTES, 1) && clean_at(&t1);
        unmount_it();
    } else ok = 0;
    step = ok ? 2 : step;

    // 2. The backups: no primary, so the mount must find one at the NEW size.
    if (ok) {
        struct gblk *sb = slot_of(8, 0);
        uint8_t keep[512];
        if (sb) {
            k_memcpy(keep, sb->data, 512);
            k_memset(sb->data, 0, 512);
            ok = mount_it() && clean_at(&tb) && verify_far(BIG_BYTES, 1);
            unmount_it();
            k_memcpy(sb->data, keep, 512);
        } else ok = 0;
    }
    step = ok ? 3 : step;

    // 3. To the floor it reports, and no further.
    struct fs_resize_info at;
    fs_resize_device(&GROW_DEV, 0, 1, &at);
    uint64_t floor = at.min_bytes;
    int below = shrink_to((floor - T3_BLOCK) / 512 - 8, &info);
    int rc2 = ok ? shrink_to(floor / 512, &info) : -1;
    g_sectors = floor / 512;
    if (ok && !rc2 && mount_it()) {
        ok = clean_at(&t2) && verify(n) && verify_far(BIG_BYTES, 1);
        b2 = vol_bytes();
        unmount_it();
    } else ok = 0;
    step = ok ? 4 : step;

    // 4. And it grows again, the space past the old floor usable.
    g_sectors = 300 * MIB_SECTORS;
    int rc3 = ok ? shrink_to(300 * MIB_SECTORS, &info) : -1;
    uint32_t more = 0;
    if (ok && !rc3 && mount_it()) {
        more = fill(n, n + 300);
        ok = more == n + 300 && clean_at(&t0) && verify(more) && verify_far(BIG_BYTES, 1);
        unmount_it();
    } else ok = 0;
    step = ok ? 5 : step;
    int full = g_full;
    store_close();
    fs_delete(GROW_POINT);

    KTEST_ASSERT(!full);
    KTEST_ASSERT(made);
    KTEST_ASSERT_EQ(rc1, 0);
    KTEST_ASSERT_EQ(b1, want1);
    KTEST_ASSERT(b1 <= 140ull << 20 && b1 > 138ull << 20);
    KTEST_ASSERT_EQ(tb, t1);
    KTEST_ASSERT_EQ(below, -ENOSPC);
    KTEST_ASSERT_EQ(rc2, 0);
    KTEST_ASSERT_EQ(b2, floor);
    KTEST_ASSERT(t2 > 0);
    KTEST_ASSERT(floor < 20ull << 20);           // 5.3 MiB of files, and a group's metadata
    KTEST_ASSERT_EQ(rc3, 0);
    KTEST_ASSERT_EQ(step, 5);
}

// The shrink, cut short after every `stride`-th of its writes. With
// `many` (a folder whose `..`s take more than one journal step), a cut
// between those steps may leave `..`s that fsck repair must put right --
// and only that; otherwise every cut is clean at the old size or the new.
struct shrink_cut { int writes, at_old, at_new, repaired, bad; };

static void cut_every_shrink_write(int many, int stride, struct shrink_cut *res) {
    k_memset(res, 0, sizeof *res);
    res->bad = -1;
    g_sectors = 300 * MIB_SECTORS;
    if (!fs_format_device(&GROW_DEV, "tfs3") || !mount_it()) return;
    uint32_t n = fill(0, 20);
    uint64_t old_total = 0, new_total = 0;
    int ok = n == 20 && make_far(65536, many) && clean_at(&old_total);
    unmount_it();
    if (!ok) return;
    struct gblk *snap = store_snapshot();
    if (!snap) return;

    struct fs_resize_info info;
    g_written = 0;
    int rc = shrink_to(140 * MIB_SECTORS, &info);
    res->writes = (int)g_written;
    if (rc || !mount_it()) { store_clear(snap); kfree(snap); return; }
    clean_at(&new_total);
    unmount_it();

    res->bad = 0;
    for (int cut = 0; cut <= res->writes; cut += stride) {
        g_sectors = 300 * MIB_SECTORS;
        if (!store_restore(snap)) { res->bad++; break; }
        g_written = 0;
        g_cut = cut;
        shrink_to(140 * MIB_SECTORS, &info);
        g_cut = -1;
        uint64_t total = 0;
        int good = mount_it() && clean_at(&total);
        if (!good && many) {
            // Only `..` (and the link counts it feeds) may be wrong.
            struct fs_check_result r;
            k_memset(&r, 0, sizeof r);
            fs_check_at(GROW_POINT, 0, &r);
            int dotdot_only = r.dir_errors && !r.leaked && !r.referenced_but_free && !r.double_allocated &&
                              !r.out_of_range && !r.bad_names && !r.counts_wrong;
            k_memset(&r, 0, sizeof r);
            good = dotdot_only && fs_check_at(GROW_POINT, 1, &r) == 0 && clean_at(&total);
            res->repaired += good;
        }
        good = good && verify(n) && verify_far(65536, many);
        unmount_it();
        if (good && total == old_total) res->at_old++;
        else if (good && total == new_total) res->at_new++;
        else {
            if (!res->bad) klog_printf("ktest: shrink cut after %d of %d writes: %s, total %llu (old %llu, new %llu)\n",
                                       cut, res->writes, good ? "clean" : "NOT clean", (unsigned long long)total,
                                       (unsigned long long)old_total, (unsigned long long)new_total);
            res->bad++;
        }
    }
    store_clear(snap);
    kfree(snap);
}

KTEST("tfs3", "shrink: cut short after any write, it mounts clean at the old size or the new") {
    if (!prepare()) KTEST_SKIP("no persistent root to mount on");
    if (!store_open()) KTEST_SKIP("could not allocate the store");
    struct shrink_cut a;
    cut_every_shrink_write(0, 1, &a);
    int full = g_full;
    store_close();
    fs_delete(GROW_POINT);

    KTEST_ASSERT(!full);
    KTEST_ASSERT_EQ(a.bad, 0);
    KTEST_ASSERT(a.writes > 20);
    KTEST_ASSERT_EQ(a.repaired, 0);
    KTEST_ASSERT(a.at_old > 0 && a.at_new > 0);
}

KTEST("tfs3", "shrink: a big folder's `..`s over several steps -- a cut between them is fsck-repairable") {
    if (!prepare()) KTEST_SKIP("no persistent root to mount on");
    if (!store_open()) KTEST_SKIP("could not allocate the store");
    // Every 10th write: ~1500 of them, and the window between the `..`
    // steps is ~40 writes wide (a step's images, header and targets), so
    // a stride of 10 lands in it several times.
    struct shrink_cut a;
    cut_every_shrink_write(1, 10, &a);
    int full = g_full;
    store_close();
    fs_delete(GROW_POINT);

    KTEST_ASSERT(!full);
    KTEST_ASSERT_EQ(a.bad, 0);
    KTEST_ASSERT(a.repaired > 0);                // the window exists, and repair closes it
    KTEST_ASSERT(a.at_old > 0 && a.at_new > 0);
}
