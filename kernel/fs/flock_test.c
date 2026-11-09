// POSIX record locks (flock.c), driven on made-up owners: a lock's owner
// is a descriptor table's key, which the table never dereferences, so
// two numbers are two processes here. The file is real -- the key is
// its mount and inode -- and the descriptions are built by hand, as
// SYS_OPEN would leave them.
#include "ktest.h"
#include "flock.h"
#include "syscalls.h"
#include "fs.h"
#include "string.h"
#include "errno.h"

#define A 0xFDA00000ull
#define B 0xFDB00000ull
#define LPATH "/tmp/.ktest_flock"
// On the system disk, which has hard links (ramfs, /tmp, has none).
#define LROOT "/.ktest_flock"
#define LLINK "/.ktest_flock_link"

static struct open_file g_fa, g_fb;

static void desc(struct open_file *f, const char *path) {
    k_memset(f, 0, sizeof *f);
    f->refs = 1;
    f->ops = &file_fd_ops;
    k_strlcpy(f->file.name, path, sizeof f->file.name);
    f->file.mode = FD_MODE_RDWR;
}

static int lk(uint64_t owner, struct open_file *f, int cmd, int type, int64_t start, int64_t len) {
    struct sys_flock fl = { (int16_t)type, SYS_LK_SEEK_SET, 0, start, len };
    uint64_t regs[32] = { 0 };
    int parked = 0;
    return flock_op(owner, owner == A ? 100 : 200, f, cmd, &fl, regs, &parked);
}

static struct sys_flock get(uint64_t owner, struct open_file *f, int type, int64_t start, int64_t len) {
    struct sys_flock fl = { (int16_t)type, SYS_LK_SEEK_SET, 0, start, len };
    uint64_t regs[32] = { 0 };
    int parked = 0;
    flock_op(owner, owner == A ? 100 : 200, f, SYS_LK_GET, &fl, regs, &parked);
    return fl;
}

static int setup(void) {
    fs_delete(LPATH);
    if (!fs_write(LPATH, "0123456789", 0)) return 0;
    desc(&g_fa, LPATH);
    desc(&g_fb, LPATH);
    return 1;
}

static void teardown(void) {
    flock_release_owner(A);
    flock_release_owner(B);
    fs_delete(LPATH);
    fs_delete(LLINK);
}

KTEST("flock", "a write lock refuses another process's overlapping lock, and F_GETLK names it") {
    int before = flock_count();
    KTEST_ASSERT(setup());
    int a = lk(A, &g_fa, SYS_LK_SET, SYS_LK_WR, 0, 10);
    int b_in = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 5, 1);
    int b_rd = lk(B, &g_fb, SYS_LK_SET, SYS_LK_RD, 9, 1);
    int b_out = lk(B, &g_fb, SYS_LK_SET, SYS_LK_RD, 10, 20);
    int b_w = lk(B, &g_fb, SYS_LK_SETW, SYS_LK_WR, 0, 1);   // nowhere to park: as SET
    struct sys_flock g = get(B, &g_fb, SYS_LK_WR, 3, 1);
    struct sys_flock mine = get(A, &g_fa, SYS_LK_WR, 0, 10); // its own lock is no conflict
    teardown();
    KTEST_ASSERT_EQ(a, 0);
    KTEST_ASSERT_EQ(b_in, -EAGAIN);
    KTEST_ASSERT_EQ(b_rd, -EAGAIN);
    KTEST_ASSERT_EQ(b_out, 0);
    KTEST_ASSERT_EQ(b_w, -EAGAIN);
    KTEST_ASSERT_EQ(g.type, SYS_LK_WR);
    KTEST_ASSERT_EQ(g.start, 0);
    KTEST_ASSERT_EQ(g.len, 10);
    KTEST_ASSERT_EQ(g.pid, 100);
    KTEST_ASSERT_EQ(mine.type, SYS_LK_UN);
    KTEST_ASSERT_EQ(flock_count(), before);
}

KTEST("flock", "read locks share; a write lock waits for all of them") {
    KTEST_ASSERT(setup());
    int a = lk(A, &g_fa, SYS_LK_SET, SYS_LK_RD, 0, 0);
    int b = lk(B, &g_fb, SYS_LK_SET, SYS_LK_RD, 0, 0);
    int bw = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 0, 0);
    int a_up = lk(A, &g_fa, SYS_LK_SET, SYS_LK_WR, 4, 1);
    flock_release_owner(A);
    int bw2 = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 0, 0);   // an upgrade, A gone
    teardown();
    KTEST_ASSERT_EQ(a, 0);
    KTEST_ASSERT_EQ(b, 0);
    KTEST_ASSERT_EQ(bw, -EAGAIN);
    KTEST_ASSERT_EQ(a_up, -EAGAIN);
    KTEST_ASSERT_EQ(bw2, 0);
}

KTEST("flock", "unlocking the middle of a lock leaves both ends locked") {
    int before = flock_count();
    KTEST_ASSERT(setup());
    KTEST_ASSERT_EQ(lk(A, &g_fa, SYS_LK_SET, SYS_LK_WR, 0, 100), 0);
    KTEST_ASSERT_EQ(lk(A, &g_fa, SYS_LK_SET, SYS_LK_UN, 40, 20), 0);
    int mid = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 45, 10);
    int lo = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 39, 1);
    int hi = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 60, 1);
    int held = flock_count() - before;   // A's two ends and B's middle
    teardown();
    KTEST_ASSERT_EQ(mid, 0);
    KTEST_ASSERT_EQ(lo, -EAGAIN);
    KTEST_ASSERT_EQ(hi, -EAGAIN);
    KTEST_ASSERT_EQ(held, 3);
    KTEST_ASSERT_EQ(flock_count(), before);
}

KTEST("flock", "closing ANY descriptor on the file drops the process's locks on it (POSIX)") {
    KTEST_ASSERT(setup());
    static struct open_file other;
    desc(&other, LPATH);   // a second open of the same file, never locked through
    KTEST_ASSERT_EQ(lk(A, &g_fa, SYS_LK_SET, SYS_LK_WR, 0, 0), 0);
    int before = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 0, 1);
    flock_close(B, &other);   // ANOTHER process closing: A's locks stay
    int still = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 0, 1);
    flock_close(A, &other);
    int after = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 0, 1);
    teardown();
    KTEST_ASSERT_EQ(before, -EAGAIN);
    KTEST_ASSERT_EQ(still, -EAGAIN);
    KTEST_ASSERT_EQ(after, 0);
}

KTEST("flock", "an exec keeps the locks (rekeyed); the process's end drops them") {
    KTEST_ASSERT(setup());
    const uint64_t A2 = 0xFDA10000ull;
    KTEST_ASSERT_EQ(lk(A, &g_fa, SYS_LK_SET, SYS_LK_WR, 0, 0), 0);
    flock_rekey(A, A2);
    int held = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 0, 1);
    flock_release_owner(A);   // the OLD key holds nothing now
    int still = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 0, 1);
    flock_release_owner(A2);
    int freed = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 0, 1);
    teardown();
    KTEST_ASSERT_EQ(held, -EAGAIN);
    KTEST_ASSERT_EQ(still, -EAGAIN);
    KTEST_ASSERT_EQ(freed, 0);
}

KTEST("flock", "the range rules: len 0 runs past the end, a negative len is before start") {
    KTEST_ASSERT(setup());
    KTEST_ASSERT_EQ(lk(A, &g_fa, SYS_LK_SET, SYS_LK_WR, 100, 0), 0);        // [100, inf)
    int far = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 1000000, 1);
    int below = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 99, 1);
    KTEST_ASSERT_EQ(lk(A, &g_fa, SYS_LK_SET, SYS_LK_WR, 50, -10), 0);       // [40, 49]
    int neg_in = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 40, 1);
    int neg_out = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, 50, 1);
    int bad = lk(B, &g_fb, SYS_LK_SET, SYS_LK_WR, -1, 1);
    int badtype = lk(B, &g_fb, SYS_LK_SET, 7, 0, 1);
    teardown();
    KTEST_ASSERT_EQ(far, -EAGAIN);
    KTEST_ASSERT_EQ(below, 0);
    KTEST_ASSERT_EQ(neg_in, -EAGAIN);
    KTEST_ASSERT_EQ(neg_out, 0);
    KTEST_ASSERT_EQ(bad, -EINVAL);
    KTEST_ASSERT_EQ(badtype, -EINVAL);
}

KTEST("flock", "a read lock needs a reader, a write lock a writer (EBADF)") {
    KTEST_ASSERT(setup());
    g_fa.file.mode = FD_MODE_READ;
    g_fb.file.mode = FD_MODE_WRITE;
    int w_on_reader = lk(A, &g_fa, SYS_LK_SET, SYS_LK_WR, 0, 1);
    int r_on_writer = lk(B, &g_fb, SYS_LK_SET, SYS_LK_RD, 0, 1);
    int r_on_reader = lk(A, &g_fa, SYS_LK_SET, SYS_LK_RD, 0, 1);
    teardown();
    KTEST_ASSERT_EQ(w_on_reader, -EBADF);
    KTEST_ASSERT_EQ(r_on_writer, -EBADF);
    KTEST_ASSERT_EQ(r_on_reader, 0);
}

KTEST("flock", "a hard link is the same file: its locks are the file's") {
    if (!fs_path_has("/", FS_CAP_HARDLINKS)) KTEST_SKIP("no hard links on the root filesystem");
    fs_delete(LROOT);
    fs_delete(LLINK);
    KTEST_ASSERT(fs_write(LROOT, "x", 0));
    KTEST_ASSERT_EQ(fs_link(LROOT, LLINK), 1);
    static struct open_file by_name, via_link;
    desc(&by_name, LROOT);
    desc(&via_link, LLINK);
    int a = lk(A, &by_name, SYS_LK_SET, SYS_LK_WR, 0, 0);
    int b = lk(B, &via_link, SYS_LK_SET, SYS_LK_WR, 0, 1);
    teardown();
    fs_delete(LROOT);
    KTEST_ASSERT_EQ(a, 0);
    KTEST_ASSERT_EQ(b, -EAGAIN);
}
