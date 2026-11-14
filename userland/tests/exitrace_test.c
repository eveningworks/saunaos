// AN EXIT IS NOT PREEMPTIBLE FROM "ZOMBIE" TO THE SWITCH AWAY.
//
// scheduler_on_exit(), scheduler_on_thread_exit() and a deferred group
// exit mark the slot ZOMBIE (or free it) and then switch away
// (kernel/proc/sched_exit.c). A tick between the two runs the rotation,
// which marks the slot READY over the ZOMBIE; resumed later, the exit
// finishes with the slot RUNNING and nothing current. The parent then
// waits for good on a child that never becomes a zombie, a join on a
// thread that never does.
//
// So: children that exit at once, reaped one by one, then threads that
// exit at once, joined one by one. A watchdog thread reports a loop that
// stopped moving.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: every run with interrupts
// off in syscalls** (`syscall_irq=off`), and nearly every run with
// them on -- the window is a few instructions. Evidence at the default
// boot, red there with the window widened by hand (docs/roadmap-details.md, "Interruptible syscalls").
//
// It must be SPAWNED, not `run` -- it has children and threads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/exitrace_test"
#define CHILD_CODE 42
#define STALL_MS 3000

static volatile unsigned g_loops;
static volatile const char *g_phase = "start";
static volatile int g_done;

static unsigned long long now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

static void *watchdog(void *arg) {
    (void)arg;
    unsigned seen = g_loops;
    unsigned long long since = now_ms();
    while (!g_done) {
        sys_sleep_ms(100);
        if (g_loops != seen) { seen = g_loops; since = now_ms(); continue; }
        if (now_ms() - since < STALL_MS) continue;
        utest_checkf(0, "an exit is reaped: stuck %llu ms in %s after %u loops",
                     now_ms() - since, (const char *)g_phase, seen);
        exit(utest_end());
    }
    return 0;
}

static void *quit(void *arg) { return arg; }

int main(int argc, char **argv) {
    int secs = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--child")) return CHILD_CODE;
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    }
    utest_begin("exitrace_test", "an exit cannot be preempted half-done", UTEST_VERDICT_FILE);

    pthread_t dog;
    int have_dog = pthread_create(&dog, NULL, watchdog, NULL) == 0;
    utest_check(have_dog, "started the watchdog");

    // Processes.
    g_phase = "waitpid";
    unsigned long long end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    unsigned children = 0, bad_child = 0;
    while (sys_monotonic_ns() < end && bad_child < 8) {
        int pid = sys_spawn(SELF, "--child", -1);
        int code = -1;
        if (pid <= 0 || sys_waitpid(pid, &code) != pid || code != CHILD_CODE) bad_child++;
        else children++;
        g_loops++;
    }

    // Threads.
    g_phase = "pthread_join";
    end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    unsigned threads = 0, bad_thread = 0;
    while (sys_monotonic_ns() < end && bad_thread < 8) {
        pthread_t t;
        void *ret = 0;
        if (pthread_create(&t, NULL, quit, (void *)7) != 0 ||
            pthread_join(t, &ret) != 0 || ret != (void *)7) bad_thread++;
        else threads++;
        g_loops++;
    }
    g_done = 1;
    if (have_dog) pthread_join(dog, NULL);

    utest_checkf(children > 0 && bad_child == 0,
                 "every child is reaped with its code: %u reaped, %u wrong", children, bad_child);
    utest_checkf(threads > 0 && bad_thread == 0,
                 "every thread is joined with its value: %u joined, %u wrong", threads, bad_thread);
    return utest_end();
}
