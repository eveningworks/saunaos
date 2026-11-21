// KTESTs for processes on more than one core (smp=run, docs/smp-design.md
// stage 5). Skipped on a boot that parks the other cores, which is the
// default -- ktest_run.py's image built KCMDLINE=smp=run runs them.
#include "ktest.h"
#include "smp.h"
#include "percpu.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"
#include "bkl.h"

#define SPIN_PATH "/tests/spin_test"
#define NSPIN 3

KTEST("smp", "smp=run: busy processes run on the other cores, not only this one") {
    if (!smp_scheduling()) KTEST_SKIP("the other cores are parked (smp=park)");
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");
    int n = smp_cpu_count();
    uint64_t before[n];
    for (int i = 0; i < n; i++) before[i] = smp_cpu_at(i)->busy_ns;

    int pid[NSPIN];
    for (int k = 0; k < NSPIN; k++) {
        pid[k] = scheduler_spawn(SPIN_PATH, "600");
        KTEST_ASSERT(pid[k] > 0);
    }
    // This core busy-waits in the kernel context meanwhile, so whatever
    // they run, they run elsewhere.
    uint64_t t0 = coarse_ticks();
    while (coarse_ticks() - t0 < 50) { }

    int aps = 0;
    for (int i = 1; i < n; i++)
        if (smp_cpu_at(i)->busy_ns > before[i]) aps++;
    for (int k = 0; k < NSPIN; k++) {
        int code;
        scheduler_kill(pid[k], 0);
        t0 = coarse_ticks();
        while (scheduler_poll(pid[k], &code) == SCHED_POLL_RUNNING && coarse_ticks() - t0 < 200) { }
    }
    // Three spinners and at least two idle APs: two or more must have run one.
    KTEST_ASSERT(aps >= 2);
}

// docs/smp-design.md, stage 7: more spinners than cores, so every AP's
// slice ends in a switch between two ring-3 contexts -- the case
// scheduler_ap_event_lockless() takes with the scheduler lock alone.
// Each must still run, die when killed, and never be resumed on two
// cores at once (the live mark held until the stack switch). The
// positive control is kernel.lockless_sched off: no lock-free events.
#define NOVER 8
KTEST("smp", "smp=run: oversubscribed spinners switch on the APs without the kernel lock") {
    if (!smp_scheduling()) KTEST_SKIP("the other cores are parked (smp=park)");
    if (!scheduler_lockless_sched_enabled()) KTEST_SKIP("kernel.lockless_sched is off");
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");
    int n = smp_cpu_count();
    if (n < 2) KTEST_SKIP("one core");
    uint64_t free0 = 0;
    for (int i = 1; i < n; i++) free0 += smp_cpu_at(i)->sched_lockless;
    unsigned dbl0 = scheduler_double_resumes(), bklf0 = bkl_faults(), slf0 = scheduler_lock_faults();

    int pid[NOVER];
    for (int k = 0; k < NOVER; k++) {
        pid[k] = scheduler_spawn(SPIN_PATH, "600");
        KTEST_ASSERT(pid[k] > 0);
    }
    uint64_t t0 = coarse_ticks();
    while (coarse_ticks() - t0 < 100) { }

    uint64_t free1 = 0;
    for (int i = 1; i < n; i++) free1 += smp_cpu_at(i)->sched_lockless;
    int ran = 0;
    for (int k = 0; k < NOVER; k++) {
        struct proc_info pi;
        if (scheduler_proc_info_pid(pid[k], &pi) && pi.cpu_ns > 0) ran++;
    }
    int gone = 0;
    for (int k = 0; k < NOVER; k++) {
        int code;
        scheduler_kill(pid[k], 0);
        t0 = coarse_ticks();
        while (scheduler_poll(pid[k], &code) == SCHED_POLL_RUNNING && coarse_ticks() - t0 < 300) { }
        if (scheduler_poll(pid[k], &code) != SCHED_POLL_RUNNING) gone++;
    }
    KTEST_ASSERT(free1 > free0);              // the APs took events without the kernel lock
    KTEST_ASSERT_EQ(ran, NOVER);              // every spinner ran
    KTEST_ASSERT_EQ(gone, NOVER);             // and every one died when killed
    KTEST_ASSERT_EQ(scheduler_double_resumes(), dbl0);
    KTEST_ASSERT_EQ(bkl_faults(), bklf0);
    KTEST_ASSERT_EQ(scheduler_lock_faults(), slf0);
}
