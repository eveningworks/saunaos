// The sleeping lock. See kmutex.h for what it is and what it replaced.
#include "kmutex.h"
#include "scheduler.h"
#include "barrier.h"   // cpu_relax()
#include "kfmt.h"      // klog_printf
#include "klog.h"
#include "spinlock.h"
#include "ktrace.h"    // a sleep on a held lock, and the hand to a waiter

// depth/owner/handed change under the lock's own spinlock with this
// core's interrupts off: the spinlock against another core, the irqsave
// against an interrupt on this one -- Linux's mutex and its wait_lock.
// Held for a few stores only, never across the park below.

// **THE SPIN MUST NOT DISABLE PREEMPTION, and that is the opposite of
// what a spinlock usually does.** The holder of this lock is allowed to
// SLEEP -- that is the feature -- so a spinner that stopped the tick
// would stop the only thing that can ever schedule the holder again,
// and neither would move for the rest of the boot. So the spin is a
// plain `pause`: the tick rotates to the holder, it finishes, it
// unlocks, and this loop sees it.
//
// WHICH LEAVES ONE CASE THIS CANNOT FIX: a spinner that cannot be
// rotated away either -- the preemption guard raised, or interrupts off
// with no slot. Behind a holder asleep in a disk wait (ata.c) it spins
// forever. note_atomic_take() reports such a caller on entry; the fix is
// always for that path to stop being atomic, never for this loop to
// drop a guard it does not own. It also spins HOLDING THE KERNEL LOCK
// (bkl.h): on one core nothing changes, but from SMP stage 5 a holder on
// another core needs that lock to finish.
#define SPIN_COMPLAINT 2000000u

static void spin_note(const struct kmutex *m, unsigned spins) {
    static int once;
    if (once || spins < SPIN_COMPLAINT || scheduler_preempt_depth() == 0) return;
    once = 1;
    klog_printf(KLOG_ERR "kmutex: spinning for pid %d with preemption off -- "
                "if the holder is asleep, neither of us can move\n", m->owner);
}

// **TAKEN FROM A CONTEXT THAT CAN NEITHER SLEEP NOR BE ROTATED AWAY**
// -- the preemption guard raised, or interrupts off with no scheduler
// slot. Such a caller only works while the lock is free; if the holder
// is asleep in a disk wait, it spins forever. So it is reported on
// ENTRY, contended or not, which is Linux's might_sleep(): the race
// that turns it into a hang is rare, and the call site is not.
static unsigned g_atomic_takes;
static uintptr_t g_atomic_sites[8];

static int atomic_context(void) {
    if (scheduler_preempt_depth() > 0) return 1;
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0" : "=r"(f));
    return !(f & (1u << 9)) && scheduler_current_pid() == 0;
}

static void note_atomic_take(const struct kmutex *m, uintptr_t site) {
    g_atomic_takes++;
    for (unsigned i = 0; i < sizeof g_atomic_sites / sizeof g_atomic_sites[0]; i++) {
        if (g_atomic_sites[i] == site) return;
        if (!g_atomic_sites[i]) {
            g_atomic_sites[i] = site;
            klog_printf(KLOG_ERR "kmutex: %p taken from atomic context (preempt %d, pid %d) "
                        "at %p -- it would spin forever behind a sleeping holder\n",
                        (const void *)m, scheduler_preempt_depth(),
                        scheduler_current_pid(), (void *)site);
            return;
        }
    }
}

unsigned kmutex_atomic_takes(void) { return g_atomic_takes; }

// Lockdep (lockdep.h) sees a FIRST take and the LAST release; a
// recursive one is the same hold, as it is to the lock. The take is
// checked BEFORE the wait, Linux's lock_acquire(): an inversion that
// deadlocks is reported rather than hung on.
#ifdef SAUNAOS_LOCKDEP
#define LD_PREPARE(m) lockdep_kmutex_prepare(m)
#define LD_TAKEN(m, sub, nest, site, try) lockdep_kmutex_acquired((m), (sub), (nest), (site), (try))
#define LD_RELEASE(m) lockdep_kmutex_release(m)
#else
#define LD_PREPARE(m) ((void)0)
#define LD_TAKEN(m, sub, nest, site, try) ((void)(sub), (void)(nest))
#define LD_RELEASE(m) ((void)0)
#endif

// INLINED INTO EACH ENTRY, so without lockdep kmutex_lock() is the code
// it was -- and __builtin_return_address(0) here is the entry's caller.
// Ours already is stable to read: only this context moves it to or from itself.
static inline __attribute__((always_inline))
void lock_common(struct kmutex *m, int sub, const struct kmutex *nest) {
    if (!(m->owned && m->owner == scheduler_current_pid() && m->depth > 0)) {
        if (atomic_context()) note_atomic_take(m, (uintptr_t)__builtin_return_address(0));
        LD_PREPARE(m);
        LD_TAKEN(m, sub, nest, (uintptr_t)__builtin_return_address(0), 0);
    }
    unsigned spins = 0;
    for (;;) {
        int me = scheduler_current_pid();
        uint64_t f = spin_lock_irqsave(&m->wait_lock);
        if (m->depth == 0) {
            m->depth = 1;
            m->owner = me;
            m->owned = 1;
            spin_unlock_irqrestore(&m->wait_lock, f);
            return;
        }
        // ALREADY OURS -- nest, or claim what an unlock handed us while
        // we slept. Compared through `owned` rather than against a
        // sentinel pid, because the kernel context is pid 0 and is a
        // real owner.
        if (m->owned && m->owner == me) {
            if (m->handed) m->handed = 0;   // depth is already 1
            else m->depth++;
            spin_unlock_irqrestore(&m->wait_lock, f);
            return;
        }
        spin_unlock_irqrestore(&m->wait_lock, f);

        // ARM BEFORE RE-TESTING. A kmutex_unlock() landing between the
        // test above and the park below would otherwise be lost, and
        // there is no ring-3 retry loop under this to paper over it --
        // the same rule scheduler_block_kernel() states.
        scheduler_wait_arm(m);
        if (m->depth == 0 || (m->owned && m->owner == me)) {
            scheduler_wait_disarm();
            continue;
        }
        KTRACE(KTRACE_G_MUTEX, KTRACE_EV_MUTEX_SLEEP, m, m->owner, 0, 0);
        if (scheduler_block_kernel(m, SCHED_WAIT_LOCK)) { spins = 0; continue; }
        scheduler_wait_disarm();

        spin_note(m, ++spins);
        cpu_relax();
    }
}

void kmutex_lock(struct kmutex *m) { lock_common(m, 0, 0); }

#ifdef SAUNAOS_LOCKDEP
void kmutex_lock_nested(struct kmutex *m, int subclass) { lock_common(m, subclass, 0); }
void kmutex_lock_nest_lock(struct kmutex *m, const struct kmutex *outer) { lock_common(m, 0, outer); }
#endif

static int trylock_common(struct kmutex *m, int subclass, uintptr_t site) {
    int me = scheduler_current_pid();
    LD_PREPARE(m);
    uint64_t f = spin_lock_irqsave(&m->wait_lock);
    int got = 0;
    if (m->depth == 0) {
        m->depth = 1; m->owner = me; m->owned = 1; got = 2;
    } else if (m->owned && m->owner == me && !m->handed) {
        m->depth++; got = 1;
    }
    spin_unlock_irqrestore(&m->wait_lock, f);
    if (got == 2) LD_TAKEN(m, subclass, 0, site, 1);
    (void)subclass; (void)site;
    return got != 0;
}

int kmutex_trylock(struct kmutex *m) {
    return trylock_common(m, 0, (uintptr_t)__builtin_return_address(0));
}

#ifdef SAUNAOS_LOCKDEP
int kmutex_trylock_nested(struct kmutex *m, int subclass) {
    return trylock_common(m, subclass, (uintptr_t)__builtin_return_address(0));
}
#endif

void kmutex_unlock(struct kmutex *m) {
    LD_PREPARE(m);
    uint64_t f = spin_lock_irqsave(&m->wait_lock);
    if (m->depth > 1) { m->depth--; spin_unlock_irqrestore(&m->wait_lock, f); return; }  // still ours
    LD_RELEASE(m);   // the last release, handed on or dropped: ours no longer

    // **HANDED STRAIGHT TO A SLEEPING WAITER, NOT DROPPED.** Dropped,
    // the releaser -- still running, and usually about to make its next
    // file call -- can take it back before the woken waiter is ever
    // scheduled, and a waiter can lose that race every time: starvation
    // with nothing blocked for good. Linux's mutex handoff (4.10). The
    // waiter owns it from here and claims it when it resumes.
    int next = scheduler_wake_one(m);
    if (next) {
        m->owner = next;
        m->handed = 1;           // depth stays 1, owned stays 1
        spin_unlock_irqrestore(&m->wait_lock, f);
        KTRACE(KTRACE_G_MUTEX, KTRACE_EV_MUTEX_WAKE, m, next, 0, 0);
        return;
    }
    m->depth = 0;
    m->owner = 0;
    m->owned = 0;
    spin_unlock_irqrestore(&m->wait_lock, f);
    // Nobody parked -- but somebody may have ARMED and be about to. The
    // arm/armed_woken pair makes that park decline and re-test.
    scheduler_wake(m, 0);
}

int kmutex_held(const struct kmutex *m)  { return m && m->depth > 0; }
int kmutex_owner(const struct kmutex *m) { return (m && m->owned) ? m->owner : 0; }
