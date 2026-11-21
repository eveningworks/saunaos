// Syscalls that skip the kernel lock, and the clock page ring 3 reads the
// time from (docs/smp-design.md, stage 7; abi/clock_abi.h).
//
// What a broken version would still pass, and the check that catches it:
//   - the flag set but the fast path never taken: QUERY_BKLSTAT's getpid
//     row must count the calls as LOCKLESS, and with
//     kernel.lockless_syscalls off as TAKES instead (the positive control
//     runs in the same boot);
//   - a lock-free getpid/gettid reading another core's thread: four
//     threads each check their own tid, and the pid, on every call;
//   - a page read that disagrees with the kernel's: each page read must
//     land between two syscall reads, and never before a time another
//     thread already published;
//   - a page a process can write: a store to it must kill a forked child
//     with SIGSEGV, and a read() aimed at it must fail with EFAULT.
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "clock_abi.h"
#include "syscall_abi.h"
#include "base/utest.h"
#include "base/tunable.h"

#define TUNABLE "kernel.lockless_syscalls"
#define CALLS 20000

// The getpid row of QUERY_BKLSTAT: kernel-lock takes and lock-free calls,
// both 0 while there is no row (nothing has made the call either way).
static void getpid_row(uint64_t *takes, uint64_t *lockless) {
    struct query_bklstat r;
    *takes = *lockless = 0;
    QUERY_FOREACH(QUERY_BKLSTAT, r, i) {
        if (r.kind == QUERY_BKLSTAT_SYSCALL && r.id == SYS_GETPID) {
            *takes = r.takes;
            *lockless = r.lockless;
            return;
        }
    }
}

static void check_counted(void) {
    char was[16] = "on";
    tunable_get(TUNABLE, was, sizeof was);
    utest_check(tunable_set(TUNABLE, "on"), "kernel.lockless_syscalls can be turned on");

    uint64_t t0, l0, t1, l1;
    getpid_row(&t0, &l0);
    for (int i = 0; i < CALLS; i++) (void)sys_getpid();
    getpid_row(&t1, &l1);
    char d[96];
    snprintf(d, sizeof d, "lockless +%llu, takes +%llu", (unsigned long long)(l1 - l0),
             (unsigned long long)(t1 - t0));
    utest_check_detail(l1 - l0 >= CALLS, "getpid is answered without the kernel lock", d);
    utest_check_detail(t1 - t0 < CALLS / 10, "...and does not take it", d);

    // The positive control: the same calls through the lock.
    utest_check(tunable_set(TUNABLE, "off"), "kernel.lockless_syscalls can be turned off");
    getpid_row(&t0, &l0);
    for (int i = 0; i < CALLS; i++) (void)sys_getpid();
    getpid_row(&t1, &l1);
    tunable_set(TUNABLE, "on");
    snprintf(d, sizeof d, "lockless +%llu, takes +%llu", (unsigned long long)(l1 - l0),
             (unsigned long long)(t1 - t0));
    utest_check_detail(t1 - t0 >= CALLS, "turned off, getpid takes the kernel lock again", d);
    utest_check_detail(l1 - l0 < CALLS / 10, "...and is no longer counted lock-free", d);

    // A SANDBOXED process takes the lock: its sandbox may be swapped and
    // freed under the lock, so the lock-free path never reads it.
    getpid_row(&t0, &l0);
    int pid = fork();
    if (pid == 0) {
        if (pledge("stdio", NULL) != 0) _exit(2);
        for (int i = 0; i < CALLS; i++) (void)sys_getpid();
        _exit(0);
    }
    int code = -1;
    int reaped = pid > 0 && sys_waitpid(pid, &code) == pid && code == 0;
    getpid_row(&t1, &l1);
    snprintf(d, sizeof d, "exit %d, takes +%llu", code, (unsigned long long)(t1 - t0));
    utest_check_detail(reaped && t1 - t0 >= CALLS,
                       "a pledged process's getpid goes through the kernel lock", d);
    tunable_set(TUNABLE, was);
}

// ---- ids on every core ----------------------------------------------

#define THREADS 4
#define ID_LOOPS 200000

static int g_pid;
static int g_tids[THREADS];
static volatile int g_id_wrong[THREADS];

static void *id_thread(void *arg) {
    int me = (int)(intptr_t)arg;
    int tid = sys_gettid();
    g_tids[me] = tid;
    int wrong = 0;
    for (int i = 0; i < ID_LOOPS; i++) {
        if (sys_gettid() != tid) wrong++;
        if (sys_getpid() != g_pid) wrong++;
        if ((i & 1023) == 0) sched_yield();   // move cores now and then
    }
    g_id_wrong[me] = wrong;
    return 0;
}

static void check_ids(void) {
    g_pid = sys_getpid();
    pthread_t th[THREADS];
    int made = 0;
    for (int i = 0; i < THREADS; i++)
        made += pthread_create(&th[i], 0, id_thread, (void *)(intptr_t)i) == 0;
    utest_check(made == THREADS, "four threads start");
    for (int i = 0; i < made; i++) pthread_join(th[i], 0);
    int wrong = 0, distinct = 1;
    for (int i = 0; i < made; i++) {
        wrong += g_id_wrong[i];
        for (int j = 0; j < i; j++) distinct &= g_tids[i] != g_tids[j] && g_tids[i] > 0;
    }
    char d[64];
    snprintf(d, sizeof d, "%d wrong answers", wrong);
    utest_check_detail(wrong == 0, "every thread is always told its own tid and its process's pid", d);
    utest_check(distinct, "and the four tids are four different ids");
}

// ---- the clock page -------------------------------------------------

static const struct clock_page *g_page;

static int page_is_tsc(void) {
    return g_page && g_page->snap[g_page->seq & 1].mode == CLOCK_MODE_TSC;
}

static void check_page_mapped(void) {
    long va = sys_clock_page();
    char d[48];
    snprintf(d, sizeof d, "%#lx", (unsigned long)va);
    utest_check_detail(va > 0, "SYS_CLOCK_PAGE names a page", d);
    if (va <= 0) return;
    g_page = (const struct clock_page *)(uintptr_t)va;
    utest_check(g_page->magic == CLOCK_PAGE_MAGIC && g_page->version == CLOCK_PAGE_VERSION,
                "it carries the clock page's magic and version");
    utest_notef("clock page at %#lx, %s mode", (unsigned long)va,
                page_is_tsc() ? "TSC" : "syscall");
}

static void check_page_agrees(void) {
    if (!page_is_tsc()) {
        utest_notef("the clocksource is not the TSC: ring 3 reads it by syscall");
        unsigned long long a = sys_monotonic_ns_syscall(), b = sys_monotonic_ns();
        utest_check(b >= a, "sys_monotonic_ns() still answers, by syscall");
        return;
    }
    int outside = 0;
    for (int i = 0; i < 100000; i++) {
        unsigned long long a = sys_monotonic_ns_syscall();
        unsigned long long b = sys_monotonic_ns();
        unsigned long long c = sys_monotonic_ns_syscall();
        if (b < a || b > c) outside++;
    }
    char d[48];
    snprintf(d, sizeof d, "%d of 100000 outside", outside);
    utest_check_detail(outside == 0, "every page read lands between two syscall reads", d);

    // ticks from the page are the kernel's coarse clock: ns over the
    // page's own tick length.
    uint32_t coarse = g_page->snap[g_page->seq & 1].coarse_ns;
    if (!coarse) {
        utest_notef("the page has no tick length: sys_ticks() takes the syscall");
        return;
    }
    unsigned long t = sys_ticks();
    unsigned long long ns = sys_monotonic_ns_syscall();
    unsigned long want = (unsigned long)(ns / coarse);
    utest_check(t == want || t + 1 == want, "sys_ticks() from the page agrees with the clock");
}

// One thread publishes the time it read; another must never read an
// earlier one after seeing it.
static volatile uint64_t g_published;
static volatile int g_pub_stop;
static int g_pub_behind;

static void *publisher(void *arg) {
    (void)arg;
    while (!g_pub_stop) __atomic_store_n(&g_published, sys_monotonic_ns(), __ATOMIC_RELEASE);
    return 0;
}

static void check_page_across_threads(void) {
    if (!page_is_tsc()) return;
    pthread_t th;
    if (pthread_create(&th, 0, publisher, 0) != 0) {
        utest_check(0, "a publishing thread starts");
        return;
    }
    unsigned long long end = sys_monotonic_ns_syscall() + 1000000000ull;
    while (sys_monotonic_ns_syscall() < end) {
        for (int i = 0; i < 1000; i++) {
            uint64_t p = __atomic_load_n(&g_published, __ATOMIC_ACQUIRE);
            if (sys_monotonic_ns() < p) g_pub_behind++;
        }
    }
    g_pub_stop = 1;
    pthread_join(th, 0);
    char d[48];
    snprintf(d, sizeof d, "%d reads behind", g_pub_behind);
    utest_check_detail(g_pub_behind == 0, "no thread reads a time before one another thread published", d);
}

static void check_page_read_only(void) {
    if (!g_page) return;
    int pid = fork();
    if (pid == 0) {
        *(volatile uint32_t *)&g_page->pad = 1;
        _exit(0);
    }
    int code = -1;
    utest_check(pid > 0 && sys_waitpid(pid, &code) == pid, "a child forked to write the page is reaped");
    char d[32];
    snprintf(d, sizeof d, "exit %d", code);
    utest_check_detail(code == 128 + SIGSEGV, "a store to the clock page is a SIGSEGV", d);

    int p[2];
    if (pipe(p) != 0) { utest_check(0, "a pipe opens"); return; }
    (void)!write(p[1], "x", 1);
    errno = 0;
    ssize_t n = read(p[0], (void *)(uintptr_t)&g_page->pad, 1);
    utest_check(n < 0 && errno == EFAULT, "a read() aimed at the clock page fails with EFAULT");
    close(p[0]);
    close(p[1]);
    utest_check(g_page->magic == CLOCK_PAGE_MAGIC, "and the page is unchanged");
}

// ---- a signal still reaches a thread living on lock-free calls -------

static volatile int g_sig_seen, g_spin_stop;

static void on_usr1(int sig) { (void)sig; g_sig_seen = 1; }

static void *spinner(void *arg) {
    (void)arg;
    while (!g_spin_stop) (void)sys_getpid();
    return 0;
}

static void check_signal(void) {
    signal(SIGUSR1, on_usr1);
    pthread_t th;
    int ok = pthread_create(&th, 0, spinner, 0) == 0;
    kill(getpid(), SIGUSR1);
    unsigned long long end = sys_monotonic_ns_syscall() + 2000000000ull;
    while (!g_sig_seen && sys_monotonic_ns_syscall() < end) (void)sys_getpid();
    g_spin_stop = 1;
    if (ok) pthread_join(th, 0);
    utest_check(g_sig_seen, "a signal is delivered to a process making only lock-free calls");
}

int main(void) {
    utest_begin("lockless_test", "kernel/proc/syscall.c's lock-free path, abi/clock_abi.h", UTEST_VERDICT_FILE);
    check_counted();
    check_ids();
    check_page_mapped();
    check_page_agrees();
    check_page_across_threads();
    check_page_read_only();
    check_signal();
    return utest_end();
}
