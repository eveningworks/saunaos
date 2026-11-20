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
uint32_t spin_cpu_index_raw(void) {
    if (!g_percpu_ready) return 0;
    return this_cpu_read(index);
}

uint32_t spin_cpu_index(void) {
#ifdef SAUNAOS_DEBUG_PREEMPT
    debug_preempt_check((uintptr_t)__builtin_return_address(0));
#endif
    return spin_cpu_index_raw();
}

static unsigned g_lockups;
unsigned spin_lockups(void) { return __atomic_load_n(&g_lockups, __ATOMIC_RELAXED); }

// Read the TSC only every 64K spins: the check must not slow the spin
// it is timing, and rdtsc is this core's own (a clocksource read may
// take the BSP's port I/O).
#define CHECK_EVERY 0xFFFFu

// The last suspected lockup, for a reader on the boot core: an AP does
// not log from inside a spin (the log has a lock of its own).
static struct spin_lockup_info g_last;

void spin_lockup_last(struct spin_lockup_info *out) { *out = g_last; }

// What lockup_report() and spin_wait_overdue() share: count it, and keep
// what the boot core needs to say who the waiter was waiting on.
static void note_overdue(uintptr_t what, uintptr_t site, uintptr_t holder_site,
                         uint32_t me, uint32_t h) {
    __atomic_fetch_add(&g_lockups, 1, __ATOMIC_RELAXED);
    g_last = (struct spin_lockup_info){ .lock = what, .waiter_site = site,
                                        .holder_site = holder_site, .waiter = me, .holder = h };
    const struct cpu *hc = h != 0xFFFFFFFFu ? smp_cpu_at((int)h) : 0;
    if (hc) {
        g_last.vec = hc->bkl_vec; g_last.rax = hc->bkl_rax;
        // Resolved now: the slot may have been reused since, which a
        // diagnostic accepts.
        g_last.rip = hc->bkl_rip;
        g_last.pid = hc->bkl_slot >= 0 ? scheduler_slot_pid(hc->bkl_slot) : 0;
        g_last.now_pid = hc->sched_cur >= 0 ? scheduler_slot_pid(hc->sched_cur) : 0;
        uint64_t hz = cpu_tsc_hz() ? cpu_tsc_hz() : 3000000000ull;
        g_last.tick_age_ms = hc->tick_tsc ? (arch_rdtsc() - hc->tick_tsc) / (hz / 1000) : ~0ull;
        g_last.tick_rip = hc->tick_rip; g_last.tick_cs = hc->tick_cs;
        g_last.guard = scheduler_preempt_depth();
    }
}

static void lockup_report(struct spinlock *l, uintptr_t site, uint32_t me) {
    uint32_t h = l->holder ? l->holder - 1 : 0xFFFFFFFFu;
    note_overdue((uintptr_t)l, site, l->site, me, h);
    if (me != 0) return;
    static int once;
    if (once) return;
    once = 1;
    klog_printf(KLOG_ERR "spinlock: lockup suspected on %p -- core %u has waited %d ms; "
                "holder core %d took it at %p, waiter at %p\n",
                (void *)l, me, SPIN_LOCKUP_MS, h == 0xFFFFFFFFu ? -1 : (int)h,
                (void *)l->site, (void *)site);
}

void spin_wait_overdue(uintptr_t what, uintptr_t site, uint32_t me, uint32_t core) {
    note_overdue(what, site, 0, me, core);
}

static void __attribute__((noinline)) take(struct spinlock *l, uintptr_t site) {
    uint32_t me = spin_cpu_index();
    if (l->holder == me + 1) {
        // Our own ticket would wait for our own unlock. Any core: the
        // panic stops the others before this logs.
        panic_begin();
        klog_printf(KLOG_ERR "spinlock: core %u took %p again at %p; it took it at %p\n",
                    me, (void *)l, (void *)site, (void *)l->site);
        panic_finish();
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
