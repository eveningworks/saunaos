// KTESTs for the fd lock (syscall_fd.c), SMP stage 7: the descriptor
// layer excludes other CORES on its own lock, not on the kernel lock.
// In the "smp" suite, which ktest_run.py boots on four vCPUs.
//
// The hammers run on the other cores through smp_call(), WITHOUT the
// kernel lock -- the boot core holds it throughout, as every fd path but
// SYS_LSEEK's lock-free one does. A table that is any process's here is a
// made-up ODD "address space", which no real (page-aligned) CR3 can be.
#include "syscalls.h"
#include "syscall_abi.h"
#include "lockstat.h"
#include "smp.h"
#include "clocksource.h" // a hammer burst's bound
#include "barrier.h"
#include "kmutex.h"
#include "ktest.h"
#include "string.h"

static const struct fd_ops g_bare_ops = { .name = "test" };   // no open, release or seek

static int fd_lock_index(void) {
    for (int i = 0; i < lockstat_count(); i++)
        if (k_strcmp(lockstat_at(i)->name, "fd") == 0) return i;
    return -1;
}

static int on_every_core(void (*fn)(void *)) {
    int cores = 1;
    for (int i = 1; i < smp_cpu_count(); i++)
        cores += smp_call(i, fn, (void *)(uintptr_t)i, 0);
    fn((void *)0);
    for (int i = 1; i < smp_cpu_count(); i++) smp_call_wait(i, 60000);
    return cores;
}

#define CLAIMS 3000
static volatile uint32_t g_claim_bad, g_claim_oom;

// Each claim is tagged with its core and round and checked a moment
// later: a description two cores both took carries the other's tag.
static void claim_hammer(void *arg) {
    uint32_t me = (uint32_t)(uintptr_t)arg;
    for (uint32_t r = 0; r < CLAIMS; r++) {
        int di = fd_desc_alloc(&g_bare_ops, -1);
        if (di < 0) { __atomic_fetch_add(&g_claim_oom, 1, __ATOMIC_RELAXED); continue; }
        struct open_file *f = fd_desc_at(di);
        int tag = (int)(me << 20 | r);
        __atomic_store_n(&f->pipe.idx, tag, __ATOMIC_RELAXED);
        for (int k = 0; k < 20; k++) cpu_relax();
        if (__atomic_load_n(&f->pipe.idx, __ATOMIC_RELAXED) != tag || f->refs != 1)
            __atomic_fetch_add(&g_claim_bad, 1, __ATOMIC_RELAXED);
        fd_desc_unref(di);
    }
}

KTEST("smp", "the fd lock is a counted lock") {
    KTEST_ASSERT(fd_lock_index() >= 0);
}

KTEST("smp", "every core claiming descriptions at once hands none to two") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    int lock = fd_lock_index();
    KTEST_ASSERT(lock >= 0);
    g_claim_bad = g_claim_oom = 0;
    uint64_t cont = lockstat_at(lock)->contended;
    KTEST_ASSERT(on_every_core(claim_hammer) > 1);
    KTEST_ASSERT_EQ(g_claim_oom, 0u);
    KTEST_ASSERT_EQ(g_claim_bad, 0u);
    // Really contended, or it proved nothing.
    KTEST_ASSERT(lockstat_at(lock)->contended > cont);
}

#define SPACE ((uint64_t)0x5eed1)   // odd: never a real CR3

// A file description at the lowest free descriptor of SPACE, or -1.
static int open_file_at(uint64_t space, int *di) {
    *di = fd_desc_alloc(&file_fd_ops, -1);
    if (*di < 0) return -1;
    kmutex_name(&fd_desc_at(*di)->file.pos_lock, "file position");   // as sys_open names it
    int fd = fd_install(space, *di);
    if (fd < 0) fd_desc_unref(*di);
    return fd;
}

KTEST("smp", "a lock-free lseek answers SET, CUR and no-position, and declines the rest") {
    fd_release_all(SPACE);   // whatever an earlier failed test left
    KTEST_ASSERT_EQ(fd_space_open(SPACE), 0);
    int di, fd = open_file_at(SPACE, &di);
    KTEST_ASSERT(fd >= 0);
    int64_t r = 0;
    KTEST_ASSERT_EQ(fd_seek_lockless(SPACE, fd, 100, SYS_SEEK_SET, &r), 1);
    KTEST_ASSERT_EQ(r, 100ll);
    KTEST_ASSERT_EQ(fd_seek_lockless(SPACE, fd, 5, SYS_SEEK_CUR, &r), 1);
    KTEST_ASSERT_EQ(r, 105ll);
    KTEST_ASSERT_EQ(fd_seek_lockless(SPACE, fd, -200, SYS_SEEK_CUR, &r), 1);
    KTEST_ASSERT_EQ(r, (int64_t)-EINVAL);
    KTEST_ASSERT_EQ(fd_desc_at(di)->file.pos, 105ull);   // an error moves nothing
    // The size is the filesystem's, behind a sleeping lock.
    KTEST_ASSERT_EQ(fd_seek_lockless(SPACE, fd, 0, SYS_SEEK_END, &r), 0);
    // A holder of the position may be mid-read at it.
    kmutex_lock(&fd_desc_at(di)->file.pos_lock);
    int busy = fd_seek_lockless(SPACE, fd, 7, SYS_SEEK_SET, &r);
    kmutex_unlock(&fd_desc_at(di)->file.pos_lock);
    KTEST_ASSERT_EQ(busy, 0);
    KTEST_ASSERT_EQ(fd_desc_at(di)->file.pos, 105ull);
    // fd 0 is the console: no position, answered here.
    KTEST_ASSERT_EQ(fd_seek_lockless(SPACE, 0, 0, SYS_SEEK_SET, &r), 1);
    KTEST_ASSERT_EQ(r, (int64_t)-ESPIPE);
    KTEST_ASSERT_EQ(fd_seek_lockless(SPACE, 99, 0, SYS_SEEK_SET, &r), 0);   // not open
    KTEST_ASSERT_EQ(fd_seek_lockless(SPACE + 2, 0, 0, SYS_SEEK_SET, &r), 0);   // no table
    fd_release_all(SPACE);
}

// Other cores seek SEEK_FD of SPACE without the kernel lock while this
// one closes it and reuses its description at another descriptor. The
// reused one must start at 0: a seek that found the old description and
// moved it after the close would have moved the new one.
#define SEEK_FD_ROUNDS 2000
#define SEEK_TAG 0x7777
#define SETTLE_PAUSES 4000
#define BURST_NS 2000000ull   // well under the 1 s a lockup is reported at
static volatile int g_seek_fd = -1, g_burst_stop;
static volatile uint32_t g_seeks;

// **A BURST, NEVER A LOOP UNTIL THIS CORE SAYS STOP**: a spinner answers
// calls (smp_poll_ipis()), so a hammer can run nested inside an AP's own
// wait for the kernel lock, holding its ticket -- and one that waited for
// the closer deadlocked the moment the closer queued for that lock behind
// it. Bounded in time, it delays the ticket by one burst at most (Linux's
// rule for an smp_call_function() callback: it never waits on a CPU).
static void seek_hammer(void *arg) {
    if (!arg) return;   // the boot core is the closer
    int64_t r;
    uint64_t end = clocksource_now_ns() + BURST_NS;
    while (!__atomic_load_n(&g_burst_stop, __ATOMIC_ACQUIRE) && clocksource_now_ns() < end) {
        int fd = __atomic_load_n(&g_seek_fd, __ATOMIC_ACQUIRE);
        if (fd >= 0 && fd_seek_lockless(SPACE, fd, SEEK_TAG, SYS_SEEK_SET, &r) && r == SEEK_TAG)
            __atomic_fetch_add(&g_seeks, 1, __ATOMIC_RELAXED);
        cpu_relax();
    }
}

KTEST("smp", "a lock-free lseek never moves a description closed under it") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    fd_release_all(SPACE);   // whatever an earlier failed test left
    KTEST_ASSERT_EQ(fd_space_open(SPACE), 0);
    int di, fd = open_file_at(SPACE, &di);
    KTEST_ASSERT(fd >= 0);
    g_seek_fd = fd;
    g_seeks = 0;
    unsigned moved = 0, rounds = 0, bursts = 0;
    for (int r = 0; r < SEEK_FD_ROUNDS; r++) {
        g_burst_stop = 0;
        for (int i = 1; i < smp_cpu_count(); i++)
            bursts += smp_call(i, seek_hammer, (void *)(uintptr_t)i, 0);
        // Close it, and put a stream with no position at its number at once.
        fd_close(SPACE, fd);
        if (fd_dup2(SPACE, 0, fd) != fd) break;
        int di2, fd2 = open_file_at(SPACE, &di2);   // likely the same description
        if (fd2 < 0) break;
        // Long enough for a seek that found the old one to land.
        for (int k = 0; k < SETTLE_PAUSES; k++) cpu_relax();
        if (fd_desc_at(di2)->file.pos != 0) moved++;
        fd_close(SPACE, fd2);
        fd_close(SPACE, fd);
        __atomic_store_n(&g_burst_stop, 1, __ATOMIC_RELEASE);
        for (int i = 1; i < smp_cpu_count(); i++) smp_call_wait(i, 60000);
        if ((fd = open_file_at(SPACE, &di)) < 0) break;
        __atomic_store_n(&g_seek_fd, fd, __ATOMIC_RELEASE);
        rounds++;
    }
    __atomic_store_n(&g_burst_stop, 1, __ATOMIC_RELEASE);
    for (int i = 1; i < smp_cpu_count(); i++) smp_call_wait(i, 60000);
    g_seek_fd = -1;
    fd_release_all(SPACE);
    KTEST_ASSERT(bursts > 0);
    KTEST_ASSERT_EQ(rounds, (unsigned)SEEK_FD_ROUNDS);
    KTEST_ASSERT(g_seeks > 0);   // the hammers reached the seek
    KTEST_ASSERT_EQ(moved, 0u);
}
