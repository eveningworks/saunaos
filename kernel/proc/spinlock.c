// The ticket spinlock (spinlock.h). Linux's arch_spinlock_t before
// qspinlock: one fetch-add takes a ticket, and the holder is whoever's
// ticket is being served.
#include "spinlock.h"
#include "percpu.h"
#include "barrier.h"     // cpu_relax(): KVM's pause-loop exiting
#include "irqflags.h"
#include "cpuinfo.h"     // cpu_tsc_hz()
#include "random_hw.h"   // arch_rdtsc()
#include "klog.h"
#include "kfmt.h"
#include "panic.h"
#include "scheduler.h"   // the preemption guard a holder raises
#include "smp.h"         // smp_poll_ipis(): a spinner answers a shootdown or a call
#include "bkl.h"

int g_percpu_ready;

void spin_percpu_ready(void) { g_percpu_ready = 1; }

// One %gs load, so a migration cannot hand back the core it left
// (sched_internal.h's per-core fields, the same rule).
uint32_t spin_cpu_index(void) {
    if (!g_percpu_ready) return 0;
    uint32_t i;
    __asm__ volatile ("movl %%gs:%c1, %0" : "=r"(i) : "i"(__builtin_offsetof(struct cpu, index))
                      : "memory");
    return i;
}

static unsigned g_lockups;
unsigned spin_lockups(void) { return __atomic_load_n(&g_lockups, __ATOMIC_RELAXED); }

// Read the TSC only every 64K spins: the check must not slow the spin
// it is timing, and rdtsc is this core's own (a clocksource read may
// take the BSP's port I/O).
#define CHECK_EVERY 0xFFFFu

static void lockup_report(struct spinlock *l, uintptr_t site, uint32_t me) {
    __atomic_fetch_add(&g_lockups, 1, __ATOMIC_RELAXED);
    if (me != 0) return;   // an AP may not log until stage 5
    static int once;
    if (once) return;
    once = 1;
    uint32_t h = l->holder;
    klog_printf(KLOG_ERR "spinlock: lockup suspected on %p -- core %u has waited %d ms; "
                "holder core %d took it at %p, waiter at %p\n",
                (void *)l, me, SPIN_LOCKUP_MS, h ? (int)h - 1 : -1,
                (void *)l->site, (void *)site);
}

static void __attribute__((noinline)) take(struct spinlock *l, uintptr_t site) {
    uint32_t me = spin_cpu_index();
    if (l->holder == me + 1) {
        // Our own ticket would wait for our own unlock.
        if (me == 0) {
            klog_printf(KLOG_ERR "spinlock: core 0 took %p again at %p; it took it at %p\n",
                        (void *)l, (void *)site, (void *)l->site);
            panic_finish();
        }
        for (;;) __asm__ volatile ("cli; hlt");
    }
    uint32_t ticket = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) != ticket) {
        uint64_t hz = cpu_tsc_hz();
        uint64_t limit = (hz ? hz : 3000000000ull) / 1000 * SPIN_LOCKUP_MS;
        uint64_t start = arch_rdtsc();
        int reported = 0;
        unsigned n = 0;
        while (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) != ticket) {
            // Interrupts are usually off here, and the holder may be a
            // shootdown's or a call's sender waiting on this core (tlb.h).
            smp_poll_ipis();
            cpu_relax();
            if ((++n & CHECK_EVERY) == 0 && !reported && arch_rdtsc() - start > limit) {
                reported = 1;
                lockup_report(l, site, me);
            }
        }
    }
    l->holder = me + 1;
    l->site = site;
}

// **A HOLDER IS NOT PREEMPTED**, Linux's spin_lock() and its
// preempt_disable(): preempted, it would keep the other cores spinning
// for a whole slice, and the next context on this core to take the lock
// would read as this core taking it twice. The guard's depth is one
// global, the kernel lock holder's to move (sched_internal.h): an AP
// raises it only while it holds that lock, which is every time it runs
// kernel code under smp=run, and never while parked.
static int guard_ours(void) { return spin_cpu_index() == 0 || bkl_held_here(); }
static void guard_raise(void) { if (guard_ours()) scheduler_preempt_disable(); }
static void guard_drop(void)  { if (guard_ours()) scheduler_preempt_enable(); }

void spin_lock(struct spinlock *l) {
    guard_raise();
    take(l, (uintptr_t)__builtin_return_address(0));
}

void spin_lock_raw(struct spinlock *l, uintptr_t site) { take(l, site); }

int spin_trylock(struct spinlock *l) {
    guard_raise();
    uint32_t serving = __atomic_load_n(&l->serving, __ATOMIC_ACQUIRE);
    uint32_t expect = serving;
    // Free means no ticket outstanding past the one being served.
    if (!__atomic_compare_exchange_n(&l->next, &expect, serving + 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        guard_drop();
        return 0;
    }
    l->holder = spin_cpu_index() + 1;
    l->site = (uintptr_t)__builtin_return_address(0);
    return 1;
}

void spin_unlock_raw(struct spinlock *l) {
    l->holder = 0;
    // Only the holder writes `serving`, so a plain increment published
    // with release ordering is the whole unlock.
    __atomic_store_n(&l->serving, l->serving + 1, __ATOMIC_RELEASE);
}

void spin_unlock(struct spinlock *l) {
    spin_unlock_raw(l);
    guard_drop();
}

uint64_t spin_lock_irqsave(struct spinlock *l) {
    uint64_t f = irq_save();
    guard_raise();
    take(l, (uintptr_t)__builtin_return_address(0));
    return f;
}

void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags) {
    spin_unlock(l);
    irq_restore(flags);
}

int spin_held(const struct spinlock *l) {
    return __atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) !=
           __atomic_load_n(&l->next, __ATOMIC_ACQUIRE);
}

int spin_held_here(const struct spinlock *l) {
    return l->holder == spin_cpu_index() + 1;
}
