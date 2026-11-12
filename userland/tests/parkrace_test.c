// A WAKE THAT LANDS BETWEEN A WAIT'S TEST AND ITS PARK IS NOT LOST.
//
// A ring-3 wait tests its condition and parks. A wake in between finds
// nobody parked and is dropped unless the waiter ARMED first
// (scheduler_wait_arm(), the way waitpid() does it); with interrupts on
// inside a syscall a preemption can put the waker there. A futex wait and
// a thread join tested and parked unarmed.
//
// Each wait runs in a child of its own, so one that hangs is reported by
// that child's watchdog and does not hide the next:
//   futex   two threads passing a turn back and forth with FUTEX_WAIT/WAKE
//   join    threads that exit at once, joined one by one
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: every run with interrupts
// off in syscalls**, and nearly every one with them on. Evidence on an
// image built with KCMDLINE="syscall_irq=on", red there with the gap
// widened by hand (docs/roadmap-details.md, "Interruptible syscalls").
//
// It must be SPAWNED, not `run` -- it has children and threads.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/parkrace_test"
#define STALL_MS 3000
#define STALLED 3   // a phase child's exit code when its watchdog fired

static volatile unsigned g_loops;
static volatile int g_done;
static unsigned long long g_end;

static unsigned long long now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

static void *watchdog(void *arg) {
    (void)arg;
    unsigned seen = g_loops;
    unsigned long long since = now_ms();
    while (!g_done) {
        sys_sleep_ms(100);
        if (g_loops != seen) { seen = g_loops; since = now_ms(); continue; }
        if (now_ms() - since >= STALL_MS) exit(STALLED);
    }
    return 0;
}

// --- futex: whose turn it is, 0 or 1 --------------------------------
static volatile uint32_t g_turn;

static void take_turns(int me) {
    while (sys_monotonic_ns() < g_end) {
        uint32_t t;
        while ((t = g_turn) != (uint32_t)me) sys_futex_wait(&g_turn, t, 0);
        g_turn = (uint32_t)!me;
        sys_futex_wake(&g_turn, 1);
        g_loops++;
    }
    // The other side may be waiting for a turn that will not come.
    g_turn = (uint32_t)!me;
    sys_futex_wake(&g_turn, 1);
}

static void *other_side(void *arg) { (void)arg; take_turns(1); return 0; }

static int phase_futex(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, other_side, NULL) != 0) return 1;
    take_turns(0);
    pthread_join(t, NULL);
    return g_loops ? 0 : 1;
}

// --- join ------------------------------------------------------------
static void *quit(void *arg) { return arg; }

static int phase_join(void) {
    while (sys_monotonic_ns() < g_end) {
        pthread_t t;
        void *ret = 0;
        if (pthread_create(&t, NULL, quit, (void *)5) != 0) return 1;
        if (pthread_join(t, &ret) != 0 || ret != (void *)5) return 1;
        g_loops++;
    }
    return g_loops ? 0 : 1;
}

static int run_phase(const char *name, int secs) {
    g_end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    pthread_t dog;
    if (pthread_create(&dog, NULL, watchdog, NULL) != 0) return 1;
    int rc = !strcmp(name, "futex") ? phase_futex() : !strcmp(name, "join") ? phase_join() : 2;
    g_done = 1;
    pthread_join(dog, NULL);
    return rc;
}

int main(int argc, char **argv) {
    int secs = 2;
    const char *phase = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--phase") && i + 1 < argc) phase = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    }
    if (phase) return run_phase(phase, secs);

    utest_begin("parkrace_test", "a wake between a wait's test and its park is kept",
                UTEST_VERDICT_FILE);
    static const char *const phases[] = { "futex", "join" };
    for (unsigned i = 0; i < sizeof phases / sizeof *phases; i++) {
        char args[48];
        snprintf(args, sizeof args, "--phase %s --secs %d", phases[i], secs);
        int pid = sys_spawn(SELF, args, -1);
        int code = -1;
        if (pid <= 0 || sys_waitpid(pid, &code) != pid) code = -1;
        utest_checkf(code == 0, "%s: the waits all end (child exit %d%s)", phases[i], code,
                     code == STALLED ? ", stalled" : "");
    }
    return utest_end();
}
