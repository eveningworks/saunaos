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
#include "signal_abi.h"   // SIGSTOP

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
// scheduler_ap_event_lockless() takes with this core's queue lock alone.
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

// docs/smp-design.md, stage 7 (2b): work piled on ONE core's queue
// spreads to the idle ones -- an idle core's steal, or the balancer on core
// 0's tick, each enough alone. The positive control is both off
// (`config set kernel.sched.steal off`, `... kernel.sched.balance off`):
// every spinner stays on core 1's queue and this goes red.
#define NPILE 3
KTEST("smp", "smp=run: work piled on one core's queue spreads to the idle cores") {
    if (!smp_scheduling()) KTEST_SKIP("the other cores are parked (smp=park)");
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");
    if (smp_cpu_count() < 4) KTEST_SKIP("fewer than four cores: no two idle to spread to");
    uint64_t moved0 = scheduler_rq_steals() + scheduler_rq_pulls();
    int pid[NPILE];
    scheduler_test_force_core(1);   // every placement onto core 1's queue
    for (int k = 0; k < NPILE; k++) pid[k] = scheduler_spawn(SPIN_PATH, "600");
    scheduler_test_force_core(-1);
    uint64_t t0 = coarse_ticks();
    while (coarse_ticks() - t0 < 100) { }

    int ncpu = smp_cpu_count(), cores = 0;
    int seen[ncpu];
    for (int c = 0; c < ncpu; c++) seen[c] = 0;
    for (int k = 0; k < NPILE; k++) {
        int c = pid[k] > 0 ? scheduler_test_slot_core(pid[k]) : -1;
        if (c >= 0 && c < ncpu && !seen[c]) { seen[c] = 1; cores++; }
    }
    uint64_t moved = scheduler_rq_steals() + scheduler_rq_pulls() - moved0;
    for (int k = 0; k < NPILE; k++) {
        int code;
        if (pid[k] <= 0) continue;
        scheduler_kill(pid[k], 0);
        t0 = coarse_ticks();
        while (scheduler_poll(pid[k], &code) == SCHED_POLL_RUNNING && coarse_ticks() - t0 < 300) { }
    }
    for (int k = 0; k < NPILE; k++) KTEST_ASSERT(pid[k] > 0);
    KTEST_ASSERT(moved > 0);     // something moved them...
    KTEST_ASSERT(cores >= 2);    // ...off the one queue they were all put on
}

// The debug build's check at every switch (sched_rq.c's sched_rq_check()):
// each core's list names that core, no slot claims it unlisted, the counts
// agree. Its own churn, so it does not depend on the order tests run in:
// spinners piled on one queue and moved off it, one stopped and continued
// while it waits there (the movable count), then killed. A failure is
// logged where it is found; the positive control is that count left
// unadjusted in slot_set_stopped(), which this turns red.
#define NCHURN 4
KTEST("smp", "the run-queue invariants hold at every switch while work moves") {
    if (!scheduler_rq_checks()) KTEST_SKIP("not a debug build (option lockdep or debug_preempt)");
    if (!smp_scheduling()) KTEST_SKIP("the other cores are parked (smp=park)");
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");
    unsigned checks0 = scheduler_rq_checks();
    int pid[NCHURN];
    scheduler_test_force_core(1);
    for (int k = 0; k < NCHURN; k++) pid[k] = scheduler_spawn(SPIN_PATH, "600");
    scheduler_test_force_core(-1);
    for (int r = 0; r < 10; r++) {
        uint64_t t0 = coarse_ticks();
        while (coarse_ticks() - t0 < 3) { }
        for (int k = 0; k < NCHURN; k++) if (pid[k] > 0) scheduler_stop(pid[k], SIGSTOP);
        t0 = coarse_ticks();
        while (coarse_ticks() - t0 < 2) { }
        for (int k = 0; k < NCHURN; k++) if (pid[k] > 0) scheduler_continue(pid[k]);
    }
    for (int k = 0; k < NCHURN; k++) {
        int code;
        if (pid[k] <= 0) continue;
        scheduler_kill(pid[k], 0);
        uint64_t t0 = coarse_ticks();
        while (scheduler_poll(pid[k], &code) == SCHED_POLL_RUNNING && coarse_ticks() - t0 < 300) { }
    }
    for (int k = 0; k < NCHURN; k++) KTEST_ASSERT(pid[k] > 0);
    KTEST_ASSERT(scheduler_rq_checks() > checks0);
    KTEST_ASSERT_EQ(scheduler_rq_check_failures(), 0u);
}

// A CALL TO A CORE IN RING 3 IS ANSWERED, and that core keeps ticking: the
// call vector there went the kernel-lock way into a body with no case for
// it, and its EOI never came -- the core took no call, shootdown or tick
// again. A spinner put on core 1's queue is what keeps it in ring 3.
static volatile int g_call_hit;
static void call_hit(void *arg) { (void)arg; g_call_hit = 1; }

KTEST("smp", "a call reaches a core running a process in ring 3, which keeps ticking") {
    if (!smp_scheduling()) KTEST_SKIP("the other cores are parked (smp=park)");
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");
    const struct cpu *c = smp_cpu_at(1);
    if (!c || c->state != CPU_RUNNING) KTEST_SKIP("core 1 is not scheduling");
    // Kept on core 1: with something else running there, an idle core
    // steals a READY spinner, or the balancer moves it, and it never runs on 1.
    int steal = scheduler_steal_enabled(), balance = scheduler_balance_enabled();
    scheduler_steal_set(0);
    scheduler_balance_set(0);
    scheduler_test_force_core(1);
    int pid = scheduler_spawn(SPIN_PATH, "600");
    scheduler_test_force_core(-1);
    KTEST_ASSERT(pid > 0);
    uint64_t t0 = coarse_ticks();
    int on1 = 0;
    while (coarse_ticks() - t0 < 300 && !(on1 = scheduler_test_slot_core(pid) == 1 &&
                                          __atomic_load_n(&c->sched_cur, __ATOMIC_ACQUIRE) >= 0)) { }
    int calls = 0;
    for (int k = 0; k < 20; k++) {
        g_call_hit = 0;
        calls += smp_call(1, call_hit, 0, 1) && g_call_hit;
    }
    uint64_t ev0 = c->sched_events;
    t0 = coarse_ticks();
    while (coarse_ticks() - t0 < 20) { }
    uint64_t ev1 = c->sched_events;
    int code;
    scheduler_kill(pid, 0);
    t0 = coarse_ticks();
    while (scheduler_poll(pid, &code) == SCHED_POLL_RUNNING && coarse_ticks() - t0 < 300) { }
    scheduler_steal_set(steal);
    scheduler_balance_set(balance);
    KTEST_ASSERT(on1);
    KTEST_ASSERT_EQ(calls, 20);
    KTEST_ASSERT(ev1 > ev0);   // its tick still comes
}
