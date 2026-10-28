// The whole trace path, end to end: /tests/trace_test runs /bin/strace
// with its stderr on a pty and reads the master, driven here by its exit
// code -- the arrangement kernel/tty/tty_test.c gives /tests/pty_test.
// Decoding is ring 3's and tested there (/tests/utrace_test); the ring
// is /tests/tracering_test's.
#include "ktest.h"
#include "strace.h"
#include "syscall_abi.h"
#include "kapi.h"
#include "scheduler.h"
#include "fs.h"
#include "timer.h" // coarse_ticks() -- the spawn's timeout
#include "shm.h"
#include "trace_abi.h"
#include "klimit.h"
#include "klog.h"
#include "kfmt.h"
#include "errno.h"

// --- where the trace comes out ----------------------------------------

#define TRACE_TEST_PATH "/tests/trace_test"
#define TRACE_TIMEOUT_TICKS 600 // 6s at 100Hz

KTEST("strace", "/bin/strace's trace reaches its stderr, and a trace with no ring is refused") {
    if (!fs_exists(TRACE_TEST_PATH)) KTEST_SKIP("no " TRACE_TEST_PATH " on this boot");
    if (!fs_exists("/bin/hello")) KTEST_SKIP("no /bin/hello on this boot");

    int pid = scheduler_spawn(TRACE_TEST_PATH, 0);
    KTEST_ASSERT(pid != 0);

    int code = -1;
    int exited = 0;
    uint64_t start = coarse_ticks();
    while (coarse_ticks() - start < TRACE_TIMEOUT_TICKS) {
        if (scheduler_poll(pid, &code) == SCHED_POLL_EXITED) { exited = 1; break; }
    }
    KTEST_ASSERT(exited);
    // 0 = every phase worked. The codes are distinct so this reports
    // WHICH one broke: 5/6 are the untraced control finding trace text,
    // 8/9 strace's run finding none on its stderr, 11 strace's exit
    // status, 12 a trace with no ring accepted, 10 an unknown spawn flag
    // accepted. See userland/tests/trace_test.c.
    KTEST_ASSERT_EQ(code, 0);
}

// --- how many at once ----------------------------------------------------

#define RINGS 12   // past the 8 traces the fixed table held
#define FAKE_PML4(i) (0xFDA00000ull + (uint64_t)(i) * 0x1000)

KTEST("strace", "more than eight traces run at once, and the ring ceiling refuses, logged") {
    int ring[RINGS], started = 0, traced = 0;
    char name[32];
    for (int i = 0; i < RINGS; i++) {
        k_snprintf(name, sizeof name, "ktest.trace.%d", i);
        ring[i] = shm_create_named(name, 2, 0x7ff1);
        if (ring[i] < 0) continue;
        struct trace_ring_hdr *h = (struct trace_ring_hdr *)(uintptr_t)shm_frame(ring[i], 0);
        h->magic = TRACE_RING_MAGIC;
        h->nrec = (2 * 4096 - TRACE_HDR_SIZE) / TRACE_REC_SIZE;
        if (strace_test_start(ring[i], 0x7ff1, FAKE_PML4(i)) >= 0) started++;
        traced += strace_test_traced(FAKE_PML4(i));
    }
    uint32_t was = klimit_test_set(&strace_ring_limit, 1);   // 1 KiB: the next ring is too much
    uint64_t mark = klog_total_bytes();
    int past = ring[0] >= 0 ? strace_test_start(ring[0], 0x7ff1, FAKE_PML4(RINGS)) : 0;
    int logged = klimit_test_logged(&strace_ring_limit, mark);
    klimit_test_set(&strace_ring_limit, was);

    for (int i = 0; i < RINGS; i++) strace_release(FAKE_PML4(i));
    int left = 0;
    for (int i = 0; i < RINGS; i++) left += strace_test_traced(FAKE_PML4(i));
    for (int i = 0; i < RINGS; i++) {
        if (ring[i] < 0) continue;
        k_snprintf(name, sizeof name, "ktest.trace.%d", i);
        shm_unlink_named(name);
        shm_put(ring[i]);
    }

    KTEST_ASSERT_EQ(started, RINGS);
    KTEST_ASSERT_EQ(traced, RINGS);
    KTEST_ASSERT_EQ(past, -EBUSY);
    KTEST_ASSERT(logged);
    KTEST_ASSERT_EQ(left, 0);
}
