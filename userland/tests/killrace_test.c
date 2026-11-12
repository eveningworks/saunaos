// A PROCESS KILLED WHILE PREEMPTED INSIDE A SYSCALL DIES ON ITS WAY OUT.
//
// scheduler_kill() frees a victim where it stands only when it holds no
// kernel frames; one parked mid-call gets a pending SIGKILL instead and
// leaves the call first (kernel/proc/sched_exit.c, group_defer_death()).
// A thread PREEMPTED inside a syscall also holds kernel frames -- a
// mount's lock, for one -- and was not counted: the kill zombied it with
// the lock taken, and every later call on that mount waited for good.
//
// So: a child that stats a file in a loop, killed while it runs, then a
// stat on the same mount from here. A watchdog thread reports a loop
// that stopped moving.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: every run with interrupts
// off in syscalls**, and many with them on. Evidence on an image built
// with KCMDLINE="syscall_irq=on", red there with the lock held longer by
// hand (docs/roadmap-details.md, "Interruptible syscalls").
//
// It must be SPAWNED, not `run` -- it has children and a thread.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/killrace_test"
#define TARGET "/etc"
#define STALL_MS 3000

static volatile unsigned g_loops;
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
        utest_checkf(0, "a stat after a kill returns: stuck %llu ms after %u kills",
                     now_ms() - since, seen);
        exit(utest_end());
    }
    return 0;
}

static int child(void) {
    struct sys_stat st;
    for (;;) sys_stat(TARGET, &st);
}

int main(int argc, char **argv) {
    int secs = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--child")) return child();
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    }
    utest_begin("killrace_test", "a kill mid-call leaves no lock behind", UTEST_VERDICT_FILE);

    pthread_t dog;
    int have_dog = pthread_create(&dog, NULL, watchdog, NULL) == 0;
    utest_check(have_dog, "started the watchdog");

    unsigned long long end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    unsigned kills = 0, bad = 0;
    struct sys_stat st;
    while (sys_monotonic_ns() < end && bad < 8) {
        int pid = sys_spawn(SELF, "--child", -1);
        if (pid <= 0) { bad++; continue; }
        sys_sleep_ms(20);   // it is in its loop
        int code = -1;
        sys_kill(pid, SIGKILL);   // the reaped code below says whether it landed
        if (sys_waitpid(pid, &code) != pid || code != SIGNAL_EXIT_BASE + SIGKILL) bad++;
        if (sys_stat(TARGET, &st) != 0) bad++;
        kills++;
        g_loops++;
    }
    g_done = 1;
    if (have_dog) pthread_join(dog, NULL);

    utest_checkf(kills > 0 && bad == 0,
                 "each child is killed and reaped, and the mount still answers: "
                 "%u kills, %u wrong", kills, bad);
    return utest_end();
}
