// QUERY_CORELOAD: one record per online core, and the cores' busy time
// adds up to the machine's (QUERY_CPULOAD's proc_ns).
//
// THE SUM IS THE DISCRIMINATING CHECK. bill_current() adds every slice
// to both counters, so a core whose slices stop being billed per core --
// an AP's, the boot core's, a store dropped -- leaves the sum short of
// proc_ns by exactly that core's work while every single record still
// looks plausible. The spinners are what give the sum something to be
// short of; there is no affinity, so which cores run them is the
// scheduler's business and the test does not ask.
#include "ktest.h"
#include "query.h"
#include "smp.h"
#include "scheduler.h"
#include "timer.h"
#include "fs.h"
#include "kfmt.h"   // klog_printf

#define SPIN_PATH "/tests/spin_test"
#define NSPIN 2

KTEST("query", "coreload lists every online core once, the boot core first") {
    int n = smp_online_count();
    struct query_coreload r;
    int got = 0;
    uint64_t last_index = 0;
    for (int i = 0; query_read(QUERY_CORELOAD, i, &r, sizeof r) == (int)sizeof r; i++) {
        KTEST_ASSERT(r.flags & QUERY_CPU_ONLINE);
        if (i == 0) {
            KTEST_ASSERT_EQ(r.index, 0);
            KTEST_ASSERT(r.flags & QUERY_CPU_SCHEDULING);   // the boot core always runs processes
        } else {
            KTEST_ASSERT(r.index > last_index);
        }
        KTEST_ASSERT(r.now_ns > 0);
        last_index = r.index;
        got++;
    }
    KTEST_ASSERT_EQ(got, n);
    KTEST_ASSERT(query_read(QUERY_CORELOAD, n, &r, sizeof r) < 0);
}

// proc_ns and the sum of busy_ns, read with the kernel lock held and no
// tick able to hand it over: another core bills only inside the lock, so
// the two sides are one instant. Through the provider's own fill, NOT
// query_read(): that takes the registry's kmutex, which may not be taken
// under the preemption guard (it would spin behind a sleeping holder).
static void snapshot(uint64_t *proc, uint64_t *busy) {
    const struct query_provider *p = query_find(QUERY_CORELOAD);
    *busy = 0;
    scheduler_preempt_disable();
    scheduler_cpu_time(proc, 0);
    struct query_coreload r;
    for (int i = 0; p && i < p->count(); i++)
        if (p->fill(i, &r)) *busy += r.busy_ns;
    scheduler_preempt_enable();
}

KTEST("query", "coreload: the cores' busy time sums to QUERY_CPULOAD's proc_ns") {
    if (!fs_exists(SPIN_PATH)) KTEST_SKIP("no " SPIN_PATH " on this boot");
    KTEST_ASSERT(query_find(QUERY_CORELOAD) != 0);
    uint64_t p0, b0, p1, b1;
    snapshot(&p0, &b0);

    int pid[NSPIN];
    for (int k = 0; k < NSPIN; k++) {
        pid[k] = scheduler_spawn(SPIN_PATH, "600");
        KTEST_ASSERT(pid[k] > 0);
    }
    uint64_t t0 = coarse_ticks();
    while (coarse_ticks() - t0 < 50) { }
    snapshot(&p1, &b1);

    for (int k = 0; k < NSPIN; k++) {
        int code;
        scheduler_kill(pid[k], 0);
        t0 = coarse_ticks();
        while (scheduler_poll(pid[k], &code) == SCHED_POLL_RUNNING && coarse_ticks() - t0 < 200) { }
    }

    uint64_t dp = p1 - p0, db = b1 - b0;
    klog_printf("coreload_test: proc %llu ms, cores' busy %llu ms\n",
                (unsigned long long)(dp / 1000000), (unsigned long long)(db / 1000000));
    // The spinners ran: there is something to be short of.
    KTEST_ASSERT(dp > 50ull * 1000 * 1000);
    // Equal up to what a slice billed between the two reads could add --
    // none, with the lock held; the margin is for a clock read apart.
    uint64_t diff = dp > db ? dp - db : db - dp;
    KTEST_ASSERT(diff <= dp / 50 + 2ull * 1000 * 1000);
}
