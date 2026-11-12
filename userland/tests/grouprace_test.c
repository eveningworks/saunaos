// A PROCESS THAT EXITS WITH A THREAD STILL RUNNING NEVER RUNS ANYTHING
// ON THE ADDRESS SPACE IT IS DESTROYING.
//
// The last thread out of a process tears its address space down, then
// becomes a zombie (scheduler_exit_group(), kernel/proc/sched_exit.c).
// A switch in between loaded the tables being freed: a sibling still
// running in ring 3 was chosen and resumed on them, and the exiting
// thread's own slot resumed onto them too. switch_to() now refuses to
// load a freed page table, so the break is a kernel stop rather than a
// silent use of memory somebody else may already own.
//
// So: children that start a thread which spins in ring 3, then exit
// from the main thread, reaped one by one.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: every run with interrupts
// off in syscalls**, and nearly every one with them on. Evidence on an
// image built with KCMDLINE="syscall_irq=on", red there with the window
// widened by hand (docs/roadmap-details.md, "Interruptible syscalls").
//
// It must be SPAWNED, not `run` -- it has children.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/grouprace_test"
#define CHILD_CODE 7

static volatile unsigned long g_spins;

static void *spin(void *arg) {
    (void)arg;
    for (;;) g_spins++;
    return 0;
}

static int child(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, spin, NULL) != 0) return 1;
    while (g_spins < 1000) sys_yield();   // the sibling is running
    exit(CHILD_CODE);
}

int main(int argc, char **argv) {
    int secs = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--child")) return child();
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    }
    utest_begin("grouprace_test", "an exit with a thread running frees its tables last",
                UTEST_VERDICT_FILE);

    unsigned long long end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    unsigned reaped = 0, bad = 0;
    while (sys_monotonic_ns() < end && bad < 8) {
        int pid = sys_spawn(SELF, "--child", -1);
        int code = -1;
        if (pid <= 0 || sys_waitpid(pid, &code) != pid || code != CHILD_CODE) bad++;
        else reaped++;
    }
    utest_checkf(reaped > 0 && bad == 0,
                 "every child is reaped with its code: %u reaped, %u wrong", reaped, bad);
    return utest_end();
}
