// The descriptor tables grow past the sizes they used to be fixed at
// (256 descriptors per process, 1024 descriptions system-wide), and at
// their RAM-sized ceiling refuse cleanly with one log line.
//
// Driven on a made-up address-space key: a descriptor table is keyed by
// CR3 but never dereferences it, so these touch no real process.
#include "ktest.h"
#include "syscalls.h"
#include "klimit.h"
#include "klog.h"
#include "heap.h"
#include "errno.h"
#include "kfmt.h"

#define FAKE_PML4 0xFDE00000ull

static const struct fd_ops test_ops = { .name = "ktest" };

KTEST("fd", "a process's descriptor table grows past 256 and refuses at its ceiling, logged") {
    fd_release_all(FAKE_PML4);
    KTEST_ASSERT_EQ(fd_space_open(FAKE_PML4), 0);
    int di = fd_desc_index(FAKE_PML4, 0);
    KTEST_ASSERT(di >= 0);

    // Past the old fixed 256.
    int top = -1;
    for (int i = 0; i < 400; i++) {
        fd_desc_at(di)->refs++;
        int fd = fd_install(FAKE_PML4, di);
        if (fd < 0) { fd_desc_unref(di); break; }
        top = fd;
    }
    int ceiling = fd_proc_ceiling();

    // Then the ceiling: lowered so the test reaches it in a few hundred.
    uint32_t was = klimit_test_set(&fd_proc_limit, 600);
    uint64_t mark = klog_total_bytes();
    int last = -1, refused = 0;
    for (int i = 0; i < 700; i++) {
        fd_desc_at(di)->refs++;
        int fd = fd_install(FAKE_PML4, di);
        if (fd < 0) { fd_desc_unref(di); refused = 1; break; }
        last = fd;
    }
    int dup_past = fd_dup_from(FAKE_PML4, 0, 0);
    int logged = klimit_test_logged(&fd_proc_limit, mark);
    // dup2 onto a number past the ceiling is a bad number, not a table
    // that filled: refused without growing.
    int dup2_past = fd_dup2(FAKE_PML4, 0, 600);
    klimit_test_set(&fd_proc_limit, was);
    fd_release_all(FAKE_PML4);

    klog_printf("fd ktest: ceiling %d per process; reached fd %d before lowering it\n",
                ceiling, top);
    KTEST_ASSERT(ceiling > 256);
    KTEST_ASSERT_EQ(top, 3 + 400 - 1);
    KTEST_ASSERT(refused);
    KTEST_ASSERT_EQ(last, 599);
    KTEST_ASSERT_EQ(dup_past, -EMFILE);
    KTEST_ASSERT(logged);
    KTEST_ASSERT_EQ(dup2_past, -1);
}

KTEST("fd", "a dup2 far past the table's size grows it, and a fork copies all of it") {
    const uint64_t child = FAKE_PML4 + 0x1000;
    fd_release_all(FAKE_PML4);
    fd_release_all(child);
    KTEST_ASSERT_EQ(fd_space_open(FAKE_PML4), 0);
    int hi = FD_TABLE_FIRST * 4 + 5;
    int got = fd_dup2(FAKE_PML4, 1, hi);
    int cx = fd_cloexec(FAKE_PML4, hi, 1);
    fd_clone(child, FAKE_PML4);
    int child_di = fd_desc_index(child, hi);
    int child_cx = fd_cloexec(child, hi, -1);
    int parent_di = fd_desc_index(FAKE_PML4, hi);
    // The lowest free is still found below the grown number.
    fd_desc_at(parent_di)->refs++;
    int low = fd_install(FAKE_PML4, parent_di);
    fd_release_all(child);
    fd_release_all(FAKE_PML4);

    KTEST_ASSERT_EQ(got, hi);
    KTEST_ASSERT_EQ(cx, 0);
    KTEST_ASSERT(parent_di >= 0);
    KTEST_ASSERT_EQ(child_di, parent_di);
    KTEST_ASSERT_EQ(child_cx, 1);
    KTEST_ASSERT_EQ(low, 3);
}

KTEST("fd", "open-file descriptions grow past 1024 and refuse at their ceiling, logged") {
    enum { N = 1100 };
    int *held = kmalloc(N * sizeof(int));
    if (!held) KTEST_SKIP("no memory for the list");
    int n = 0;
    while (n < N) {
        int d = fd_desc_alloc(&test_ops, -1);
        if (d < 0) break;
        held[n++] = d;
    }
    int ceiling = fd_desc_ceiling();
    int count_after = fd_desc_count();

    // The ceiling: what the table already has, so every free slot is
    // taken and the next one is refused -- a moment's shortage for
    // everything else, which is why it is given straight back.
    uint32_t was = klimit_test_set(&fd_desc_limit, (uint32_t)fd_desc_count());
    uint64_t mark = klog_total_bytes();
    int *more = kmalloc((uint32_t)fd_desc_count() * sizeof(int));
    int m = 0, refused = 0;
    while (more) {
        int d = fd_desc_alloc(&test_ops, -1);
        if (d < 0) { refused = 1; break; }
        more[m++] = d;
    }
    int logged = klimit_test_logged(&fd_desc_limit, mark);
    for (int i = 0; i < m; i++) fd_desc_unref(more[i]);
    klimit_test_set(&fd_desc_limit, was);
    kfree(more);
    for (int i = 0; i < n; i++) fd_desc_unref(held[i]);
    kfree(held);

    klog_printf("fd ktest: ceiling %d descriptions\n", ceiling);
    KTEST_ASSERT(ceiling > 1024);
    KTEST_ASSERT_EQ(n, N);
    KTEST_ASSERT(count_after > 1024);
    KTEST_ASSERT(refused);
    KTEST_ASSERT(logged);
}
