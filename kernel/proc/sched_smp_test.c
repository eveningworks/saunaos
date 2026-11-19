// KTESTs for processes on more than one core (smp=run, docs/smp-design.md
// stage 5). Skipped on a boot that parks the other cores, which is the
// default -- ktest_run.py's image built KCMDLINE=smp=run runs them.
#include "ktest.h"
#include "smp.h"
#include "percpu.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"

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
