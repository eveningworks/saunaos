// QUERY_CPUSTATE and the debug console's `cpus` (smp_query.c): a record
// for every core, the counters it reads moving, and a stray NMI counted
// without costing the core anything (smp_ap_interrupt()).
#include "smp.h"
#include "percpu.h"
#include "query.h"
#include "lapic.h"
#include "spinlock.h"   // spin_cpu_index()
#include "barrier.h"    // cpu_relax()
#include "clocksource.h"
#include "scheduler.h"
#include "ktest.h"

// Through the provider's fill with the guard up, so the core asked about
// is the one this context runs on (query_read() takes a kmutex, which
// the guard forbids).
static int fill_here(struct query_cpustate *q, uint32_t *here) {
    const struct query_provider *p = query_find(QUERY_CPUSTATE);
    if (!p) return 0;
    scheduler_preempt_disable();
    *here = spin_cpu_index();
    int ok = p->fill((int)*here, q);
    scheduler_preempt_enable();
    return ok;
}

KTEST("smp", "cpustate lists every core, and the one asking holds the kernel lock") {
    struct query_cpustate q;
    int n = 0;
    for (int i = 0; query_read(QUERY_CPUSTATE, i, &q, sizeof q) == (int)sizeof q; i++) {
        KTEST_ASSERT_EQ(q.index, (uint64_t)i);
        if (i == 0) KTEST_ASSERT_EQ(q.state, QUERY_CPUSTATE_RUNNING);
        n++;
    }
    KTEST_ASSERT_EQ(n, smp_cpu_count());
    KTEST_ASSERT(query_read(QUERY_CPUSTATE, n, &q, sizeof q) < 0);

    uint32_t here;
    KTEST_ASSERT(fill_here(&q, &here));
    KTEST_ASSERT_EQ(q.bkl_holder, 1);
    KTEST_ASSERT(q.bkl_depth >= 1);
    KTEST_ASSERT_EQ(q.state, QUERY_CPUSTATE_RUNNING);
    // The boot core's clock events are a tick at most apart.
    KTEST_ASSERT(query_read(QUERY_CPUSTATE, 0, &q, sizeof q) == (int)sizeof q);
    KTEST_ASSERT(q.tick_age_ns < 1000000000ull);
}

static volatile int g_hit;
static void hit(void *arg) { (void)arg; g_hit = 1; }

KTEST("smp", "a stray NMI on an AP is counted, and the core keeps taking calls") {
    if (smp_online_count() < 2) KTEST_SKIP("one core online");
    int k = 1;
    struct cpu *c = (struct cpu *)smp_cpu_at(k);
    if (!c || (c->state != CPU_PARKED && c->state != CPU_RUNNING)) KTEST_SKIP("core 1 takes no calls");
    uint32_t state = c->state, nmis = c->nmi_stray, calls = c->calls_taken;
    KTEST_ASSERT(lapic_send_nmi((uint8_t)c->apic_id));
    uint64_t end = clocksource_now_ns() + 1000000000ull;
    while (__atomic_load_n(&c->nmi_stray, __ATOMIC_ACQUIRE) == nmis && clocksource_now_ns() < end)
        cpu_relax();
    KTEST_ASSERT_EQ(c->nmi_stray, nmis + 1);
    KTEST_ASSERT_EQ(c->state, state);
    g_hit = 0;
    KTEST_ASSERT(smp_call(k, hit, 0, 1));
    KTEST_ASSERT_EQ(g_hit, 1);
    KTEST_ASSERT_EQ(c->calls_taken, calls + 1);

    struct query_cpustate q;
    KTEST_ASSERT(query_read(QUERY_CPUSTATE, k, &q, sizeof q) == (int)sizeof q);
    KTEST_ASSERT_EQ(q.nmi_stray, (uint64_t)(nmis + 1));
    KTEST_ASSERT_EQ(q.wedge_vec, 0);
}
