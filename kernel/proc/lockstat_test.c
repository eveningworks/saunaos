// KTESTs for SMP stage 7's first pieces (docs/smp-design.md): the lock
// counters, the per-core preemption guard, and the heap and frame
// allocator excluding other cores on their own locks. In the "smp" suite,
// which ktest_run.py boots on four vCPUs.
//
// The hammers run on the other cores through smp_call(), WITHOUT the
// kernel lock -- the boot core holds it throughout -- so they are the
// first kernel code here that allocates on several cores at once. A
// hammer run from inside its core's own wait for a counted lock would
// queue behind its own ticket; nothing an AP runs takes the heap or the
// frame lock outside the kernel lock, which this core holds.
#include "lockstat.h"
#include "bkl.h"
#include "smp.h"
#include "percpu.h"
#include "barrier.h"
#include "scheduler.h"
#include "heap.h"
#include "pmm.h"
#include "ktest.h"
#include "string.h"

static int find_lock(const char *name) {
    for (int i = 0; i < lockstat_count(); i++)
        if (k_strcmp(lockstat_at(i)->name, name) == 0) return i;
    return -1;
}

KTEST("smp", "the kernel lock, the heap and the frame allocator are counted locks") {
    KTEST_ASSERT(find_lock("kernel") >= 0);
    KTEST_ASSERT(find_lock("heap") >= 0);
    KTEST_ASSERT(find_lock("frames") >= 0);
    // Every one of them has been taken by now.
    for (int i = 0; i < lockstat_count(); i++) {
        const struct lockstat *s = lockstat_at(i);
        KTEST_ASSERT(s->acquired > 0);
        KTEST_ASSERT(s->contended <= s->acquired);
    }
}

// Counted but NOT registered: a KTEST's lock would be a row in every
// kernel's `lockstat` (SPINLOCK_COUNTED registers what it declares).
static struct lockstat g_test_lock_stat = { .name = "test" };
static struct spinlock g_test_lock = { .stat = &g_test_lock_stat };

#define TAKES 200

KTEST("smp", "with hold timing off, a take is counted and its hold is not") {
    int was = lockstat_timing();
    KTEST_ASSERT(lockstat_timing_set(0));
    uint64_t acq = g_test_lock_stat.acquired, hold = g_test_lock_stat.hold_tsc;
    spin_lock(&g_test_lock);
    uint64_t since = g_test_lock_stat.held_since;   // no clock read for it
    spin_unlock(&g_test_lock);
    lockstat_timing_set(was);
    KTEST_ASSERT_EQ(since, 0ull);
    KTEST_ASSERT_EQ(g_test_lock_stat.acquired, acq + 1);
    KTEST_ASSERT_EQ(g_test_lock_stat.hold_tsc, hold);
}

KTEST("smp", "a counted lock counts each take, and a hold as long as it was held") {
    int was = lockstat_timing();
    if (!lockstat_timing_set(1)) KTEST_SKIP("no calibrated TSC rate");
    uint64_t acq = g_test_lock_stat.acquired, cont = g_test_lock_stat.contended;
    uint64_t hold = g_test_lock_stat.hold_tsc;
    for (int i = 0; i < TAKES; i++) {
        spin_lock(&g_test_lock);
        for (int k = 0; k < 50; k++) cpu_relax();
        spin_unlock(&g_test_lock);
    }
    KTEST_ASSERT(spin_trylock(&g_test_lock));
    spin_unlock(&g_test_lock);
    lockstat_timing_set(was);
    KTEST_ASSERT_EQ(g_test_lock_stat.acquired - acq, (uint64_t)TAKES + 1);
    KTEST_ASSERT_EQ(g_test_lock_stat.contended, cont);   // nobody else wanted it
    KTEST_ASSERT(g_test_lock_stat.hold_tsc > hold);
    KTEST_ASSERT(g_test_lock_stat.hold_max_tsc > 0);
}

static void take_test_lock(void *arg) {
    (void)arg;
    for (int i = 0; i < TAKES; i++) {
        spin_lock(&g_test_lock);
        for (int k = 0; k < 50; k++) cpu_relax();
        spin_unlock(&g_test_lock);
    }
}

KTEST("smp", "a counted lock several cores want counts the waits") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    uint64_t cont = g_test_lock_stat.contended, wait = g_test_lock_stat.wait_tsc;
    int cores = 1;
    for (int i = 1; i < smp_cpu_count(); i++) cores += smp_call(i, take_test_lock, 0, 0);
    KTEST_ASSERT(cores > 1);
    take_test_lock(0);
    for (int i = 1; i < smp_cpu_count(); i++) KTEST_ASSERT(smp_call_wait(i, 30000));
    KTEST_ASSERT(g_test_lock_stat.contended > cont);
    KTEST_ASSERT(g_test_lock_stat.wait_tsc > wait);
    KTEST_ASSERT(g_test_lock_stat.wait_max_tsc > 0);
}

KTEST("smp", "every take of the kernel lock is billed to one core and one entry") {
    // Exact, not approximate: the totals move only under the kernel lock,
    // which this test holds.
    KTEST_ASSERT(bkl_held_here());
    uint64_t by_core = 0, by_entry = 0, wait_core = 0, wait_entry = 0;
    for (int i = 0; i < smp_cpu_count(); i++) {
        const struct cpu *c = smp_cpu_at(i);
        by_core += c->bkl_takes;
        wait_core += c->bkl_wait_tsc;
    }
    struct bkl_entry_stat e;
    int kernel_seen = 0;
    for (uint32_t k = 0; k < BKL_KEYS; k++) {
        if (!bkl_entry_stat(k, &e)) continue;
        by_entry += e.takes;
        wait_entry += e.wait_tsc;
        if (k == BKL_KEY_KERNEL) kernel_seen = 1;
    }
    KTEST_ASSERT(by_core > 0);
    KTEST_ASSERT_EQ(by_core, by_entry);
    KTEST_ASSERT_EQ(wait_core, wait_entry);
    KTEST_ASSERT(kernel_seen);   // the boot took it
    const struct lockstat *k = lockstat_at(find_lock("kernel"));
    KTEST_ASSERT_EQ(k->acquired, by_core);
}

// The guard as the other core saw it: its own, and the boot core's.
static volatile int g_ap_own, g_ap_saw_bsp;

static void raise_guard_there(void *arg) {
    (void)arg;
    int before = scheduler_preempt_depth();
    scheduler_preempt_disable();
    g_ap_own = scheduler_preempt_depth() - before;
    g_ap_saw_bsp = smp_cpu_at(0)->preempt_depth;
    scheduler_preempt_enable();
}

KTEST("smp", "the preemption guard is each core's own") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    int mine = scheduler_preempt_depth();
    g_ap_own = -1;
    g_ap_saw_bsp = -1;
    KTEST_ASSERT(smp_call(1, raise_guard_there, 0, 1));
    KTEST_ASSERT_EQ(g_ap_own, 1);
    KTEST_ASSERT_EQ(g_ap_saw_bsp, mine);   // one global would read mine + 1
    KTEST_ASSERT_EQ(scheduler_preempt_depth(), mine);
}

// Each core keeps a few allocations live, stamped with who it is, and
// checks the stamp before it frees one: a block or frame handed to two
// cores at once is overwritten by the other.
#define HAMMER_ROUNDS 3000
#define HAMMER_LIVE 16
static volatile uint32_t g_heap_bad, g_frame_bad, g_hammer_oom;

static uint64_t next_rand(uint64_t *s) {
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    return *s;
}

static void heap_hammer(void *arg) {
    uint64_t me = (uint64_t)(uintptr_t)arg, seed = 0x9E3779B97F4A7C15ull * (me + 1);
    uint64_t *live[HAMMER_LIVE] = {0};
    uint32_t len[HAMMER_LIVE] = {0};
    for (int r = 0; r < HAMMER_ROUNDS; r++) {
        int slot = (int)(next_rand(&seed) % HAMMER_LIVE);
        if (live[slot]) {
            for (uint32_t w = 0; w < len[slot]; w++)
                if (live[slot][w] != (me << 32 | w)) { __atomic_fetch_add(&g_heap_bad, 1, __ATOMIC_RELAXED); break; }
            kfree(live[slot]);
            live[slot] = 0;
        }
        uint32_t words = 2 + (uint32_t)(next_rand(&seed) % 120);
        uint64_t *p = kmalloc(words * 8);
        if (!p) { __atomic_fetch_add(&g_hammer_oom, 1, __ATOMIC_RELAXED); continue; }
        for (uint32_t w = 0; w < words; w++) p[w] = me << 32 | w;
        live[slot] = p;
        len[slot] = words;
    }
    for (int s = 0; s < HAMMER_LIVE; s++) kfree(live[s]);
}

static void frame_hammer(void *arg) {
    uint64_t me = (uint64_t)(uintptr_t)arg, seed = 0xD1B54A32D192ED03ull * (me + 1);
    uint64_t live[HAMMER_LIVE] = {0};
    for (int r = 0; r < HAMMER_ROUNDS; r++) {
        int slot = (int)(next_rand(&seed) % HAMMER_LIVE);
        if (live[slot]) {
            volatile uint64_t *f = (volatile uint64_t *)(uintptr_t)live[slot];
            if (f[0] != (me << 32 | (uint64_t)slot) || f[511] != ~(me << 32 | (uint64_t)slot))
                __atomic_fetch_add(&g_frame_bad, 1, __ATOMIC_RELAXED);
            pmm_free_frame(live[slot]);
            live[slot] = 0;
        }
        uint64_t phys = pmm_alloc_frame(PMM_ZONE_ANY);   // identity-mapped (heap_os.c)
        if (!phys) { __atomic_fetch_add(&g_hammer_oom, 1, __ATOMIC_RELAXED); continue; }
        volatile uint64_t *f = (volatile uint64_t *)(uintptr_t)phys;
        f[0] = me << 32 | (uint64_t)slot;
        f[511] = ~(me << 32 | (uint64_t)slot);
        live[slot] = phys;
    }
    for (int s = 0; s < HAMMER_LIVE; s++) if (live[s]) pmm_free_frame(live[s]);
}

static int hammer_everywhere(void (*fn)(void *)) {
    int cores = 1;
    for (int i = 1; i < smp_cpu_count(); i++)
        cores += smp_call(i, fn, (void *)(uintptr_t)i, 0);
    fn((void *)0);
    for (int i = 1; i < smp_cpu_count(); i++) smp_call_wait(i, 60000);
    return cores;
}

KTEST("smp", "every core allocating from the heap at once hands no block to two") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    g_heap_bad = g_hammer_oom = 0;
    uint64_t cont = lockstat_at(find_lock("heap"))->contended;
    KTEST_ASSERT(hammer_everywhere(heap_hammer) > 1);
    KTEST_ASSERT_EQ(g_heap_bad, 0u);
    KTEST_ASSERT_EQ(g_hammer_oom, 0u);
    KTEST_ASSERT_EQ(heap_check(), 0ull);
    // It was really contended: otherwise the test proved nothing.
    KTEST_ASSERT(lockstat_at(find_lock("heap"))->contended > cont);
}

KTEST("smp", "every core allocating frames at once hands no frame to two") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    g_frame_bad = g_hammer_oom = 0;
    uint64_t cont = lockstat_at(find_lock("frames"))->contended;
    KTEST_ASSERT(hammer_everywhere(frame_hammer) > 1);
    KTEST_ASSERT_EQ(g_frame_bad, 0u);
    KTEST_ASSERT_EQ(g_hammer_oom, 0u);
    KTEST_ASSERT(lockstat_at(find_lock("frames"))->contended > cont);
}
