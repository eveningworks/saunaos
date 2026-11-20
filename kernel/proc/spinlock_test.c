// KTESTs for the ticket spinlock, the call IPI and the kernel lock
// (docs/smp-design.md, stage 4). In the "smp" suite, which ktest_run.py
// boots on four vCPUs; under multi-threaded TCG the cores really do run
// at once, so a lock that does not exclude loses increments here.
#include "spinlock.h"
#include "bkl.h"
#include "smp.h"
#include "percpu.h"
#include "paging.h"
#include "barrier.h"
#include "clocksource.h"
#include "scheduler.h"
#include "ktest.h"

KTEST("smp", "a ticket lock is free at zero, and lock, unlock and trylock move it") {
    struct spinlock l = {0};
    KTEST_ASSERT(!spin_held(&l));
    spin_lock(&l);
    KTEST_ASSERT(spin_held(&l));
    KTEST_ASSERT(spin_held_here(&l));
    KTEST_ASSERT(!spin_trylock(&l));
    spin_unlock(&l);
    KTEST_ASSERT(!spin_held(&l));
    KTEST_ASSERT(spin_trylock(&l));
    KTEST_ASSERT(spin_held_here(&l));
    spin_unlock(&l);
    KTEST_ASSERT(!spin_held(&l));
    KTEST_ASSERT_EQ(l.next, l.serving);
}

KTEST("smp", "spin_lock_irqsave turns interrupts off, and the restore puts them back") {
    struct spinlock l = {0};
    uint64_t before;
    __asm__ volatile ("pushfq; popq %0" : "=r"(before));
    uint64_t f = spin_lock_irqsave(&l);
    uint64_t during;
    __asm__ volatile ("pushfq; popq %0" : "=r"(during));
    KTEST_ASSERT(!(during & (1u << 9)));
    spin_unlock_irqrestore(&l, f);
    uint64_t after;
    __asm__ volatile ("pushfq; popq %0" : "=r"(after));
    KTEST_ASSERT_EQ(after & (1u << 9), before & (1u << 9));
}

static volatile uint32_t g_ran_on[64];

static void note_core(void *arg) {
    (void)arg;
    uint32_t i = this_cpu()->index;
    if (i < 64) g_ran_on[i]++;
}

KTEST("smp", "every parked core runs a call and reports it done") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    for (int i = 0; i < 64; i++) g_ran_on[i] = 0;
    for (int round = 0; round < 3; round++)
        for (int i = 1; i < smp_cpu_count(); i++)
            KTEST_ASSERT(smp_call(i, note_core, 0, 1));
    for (int i = 1; i < smp_cpu_count() && i < 64; i++) KTEST_ASSERT_EQ(g_ran_on[i], 3u);
    KTEST_ASSERT_EQ(g_ran_on[0], 0u);
}

// All the cores at once, each adding to one counter in a read, wait,
// write that loses increments unless the lock excludes.
#define COUNT_EACH 2000
static struct spinlock g_count_lock;
static volatile uint64_t g_count;

static void count_under_lock(void *arg) {
    (void)arg;
    for (int n = 0; n < COUNT_EACH; n++) {
        spin_lock(&g_count_lock);
        uint64_t v = g_count;
        for (int k = 0; k < 20; k++) cpu_relax();
        g_count = v + 1;
        spin_unlock(&g_count_lock);
    }
}

KTEST("smp", "every core counting under one ticket lock loses no increment") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    g_count = 0;
    int cores = 1;
    for (int i = 1; i < smp_cpu_count(); i++) cores += smp_call(i, count_under_lock, 0, 0);
    KTEST_ASSERT(cores > 1);
    count_under_lock(0);
    for (int i = 1; i < smp_cpu_count(); i++) KTEST_ASSERT(smp_call_wait(i, 30000));
    KTEST_ASSERT_EQ(g_count, (uint64_t)cores * COUNT_EACH);
    KTEST_ASSERT(!spin_held(&g_count_lock));
}

// The AP's side of the kernel-lock test: take it as an entry would. A
// PARKED AP answers the call holding nothing and takes it here; a
// SCHEDULING one (smp=run) reached the call through its interrupt
// entry, which is queued for the lock already, and answers from inside
// that wait -- where taking it again would run unlocked (bkl.c). There
// the entry is the taker held out, and the call only says so.
static volatile uint32_t g_bkl_runs, g_bkl_queued;

static void take_kernel_lock(void *arg) {
    (void)arg;
    if (bkl_depth() != 0 && !bkl_held_here()) {
        g_bkl_queued = 1;
        return;
    }
    bkl_enter(0);
    g_bkl_runs++;
    bkl_exit(0);
}

KTEST("smp", "the kernel lock keeps another core out until this one drops it") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    KTEST_ASSERT(bkl_held_here());
    KTEST_ASSERT(bkl_depth() >= 1);
    g_bkl_runs = 0;
    g_bkl_queued = 0;
    // Unpreemptible, or a tick hands this core to a process whose return
    // to ring 3 releases the lock -- which is the lock working (bkl.h).
    // No assertion inside: one that returns leaves the guard raised.
    scheduler_preempt_disable();
    int called = smp_call(1, take_kernel_lock, 0, 0);
    if (called) clocksource_delay_ms(50);
    uint32_t runs_held = g_bkl_runs;
    int queued_held = g_bkl_queued && bkl_contended();   // its entry waits behind us
    scheduler_preempt_enable();
    KTEST_ASSERT(called);
    KTEST_ASSERT_EQ(runs_held, 0u);         // it was spinning behind us
    int d = bkl_drop_all();
    int ran = smp_call_wait(1, 5000);
    bkl_retake(d);
    KTEST_ASSERT(ran);
    if (g_bkl_queued) KTEST_ASSERT(queued_held);
    else KTEST_ASSERT_EQ(g_bkl_runs, 1u);
    KTEST_ASSERT(bkl_held_here());
    KTEST_ASSERT_EQ(bkl_depth(), d);
}

KTEST("smp", "no kernel-lock accounting has broken since boot") {
    // Every entry, exit, switch and fresh context so far: a leak shows
    // as a count here and a `bkl:` line in the log.
    KTEST_ASSERT_EQ(bkl_faults(), 0u);
    KTEST_ASSERT_EQ(spin_lockups(), 0u);
    KTEST_ASSERT(bkl_held_here());
}

static void read_types(void *arg) {
    uint64_t *out = arg;
    paging_memory_types_now(&out[0], &out[1]);
}

KTEST("smp", "every core runs with the BSP's memory types, read on the core itself") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    smp_memory_types_changed();
    uint64_t pat, def;
    paging_memory_types_now(&pat, &def);
    for (int i = 1; i < smp_cpu_count(); i++) {
        uint64_t seen[2] = {~0ull, ~0ull};
        KTEST_ASSERT(smp_call(i, read_types, seen, 1));
        KTEST_ASSERT_EQ(seen[0], pat);
        KTEST_ASSERT_EQ(seen[1], def);
    }
}
