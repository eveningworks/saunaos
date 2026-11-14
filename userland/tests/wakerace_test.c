// A WAKE THAT LANDS WHILE THE WAITER IS ON ITS WAY TO SLEEP MUST NOT BE LOST.
//
// A waiter arms, tests its condition, and parks; a wake that finds it
// armed but not yet parked is recorded, and the park declines
// (kernel/proc/scheduler.c, block_common() and
// scheduler_block_kernel_until()). The park's own "was I woken?" test
// and the moment it stops being runnable must be ONE step: a wake
// between the two finds a process that is neither armed nor blocked, and
// is dropped.
//
// The wait here is waitpid() on a child that exits at once. A lost wake
// leaves the parent parked beside a zombie for good, so a watchdog
// thread reports a loop that stopped moving and ends the test. The
// mid-call park has the same gap, and no check here: a wake lost in a
// disk wait is made up a fraction of a second later, which is too short
// to tell from a slow host.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: every run with interrupts
// off in syscalls** (`syscall_irq=off`), where nothing can land
// between the test and the park -- and nearly every run with them on,
// because the gap is a few instructions. It is evidence at the default
// boot, and it went red there only with the gap widened by hand (docs/roadmap-details.md, "Interruptible syscalls").
//
// It must be SPAWNED, not `run` -- a child needs a parent with a pid.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rt/sys.h"
#include "base/utest.h"

#define CHILD "/tests/exit_test"
#define CHILD_CODE 42
#define STALL_MS 3000        // no child reaped for this long: the wake is gone

static volatile unsigned g_loops;
static volatile int g_waiting_on;   // the pid the parent is in waitpid() for
static volatile int g_done;

static unsigned long long now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

// Runs beside the parent's loop; the only thread that can speak once
// the parent is parked for good.
static void *watchdog(void *arg) {
    (void)arg;
    unsigned seen = g_loops;
    unsigned long long since = now_ms();
    while (!g_done) {
        sys_sleep_ms(100);
        if (g_loops != seen) { seen = g_loops; since = now_ms(); continue; }
        if (now_ms() - since < STALL_MS) continue;
        utest_checkf(0, "waitpid() returns when the child exits: parked %llu ms on pid %d "
                     "after %u loops", now_ms() - since, g_waiting_on, seen);
        exit(utest_end());
    }
    return 0;
}

int main(int argc, char **argv) {
    int secs = 2;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    utest_begin("wakerace_test", "a wake on the way to sleep is not lost", UTEST_VERDICT_FILE);

    pthread_t dog;
    int have_dog = pthread_create(&dog, NULL, watchdog, NULL) == 0;
    utest_check(have_dog, "started the watchdog");

    unsigned long long end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    unsigned bad_spawn = 0, bad_wait = 0;
    while (sys_monotonic_ns() < end && bad_spawn + bad_wait < 8) {
        int pid = sys_spawn(CHILD, 0, -1);
        if (pid <= 0) { bad_spawn++; continue; }
        g_waiting_on = pid;
        int code = -1;
        if (sys_waitpid(pid, &code) != pid || code != CHILD_CODE) bad_wait++;
        g_loops++;
    }
    g_done = 1;
    if (have_dog) pthread_join(dog, NULL);

    // The fixture reached the code: children were started and reaped.
    utest_checkf(g_loops > 0, "%u children started and reaped", g_loops);
    utest_checkf(bad_spawn == 0 && bad_wait == 0,
                 "each wait answers its own child: %u spawns refused, %u wrong answers",
                 bad_spawn, bad_wait);
    return utest_end();
}
