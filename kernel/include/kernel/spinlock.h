#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

// A ticket spinlock: excludes other CORES, where irq_save() and the
// preemption guard exclude only this one (docs/smp-design.md, stage 4).
// FIFO by construction -- a test-and-set lock lets one waiter lose
// every race, which under a hypervisor is a stall of milliseconds.
//
// **NEVER HELD ACROSS A SLEEP, AND NEVER TAKEN WITH IT ALREADY HELD ON
// THIS CORE** (a held lock raises the preemption guard, so a switch
// cannot put a second taker on this core). The first spins forever behind a holder that is not
// running; the second is a certain deadlock and panics. A lock an
// interrupt handler also takes is taken with spin_lock_irqsave(), or
// the handler spins on its own core's holder.
//
// All-zero is unlocked, so a static or a `= {0}` struct needs no init.
#include <stdint.h>

struct spinlock {
    volatile uint32_t next;      // the next ticket handed out
    volatile uint32_t serving;   // the ticket that holds it
    volatile uint32_t holder;    // holding core's index + 1; 0 = free
    uint32_t pad;
    uintptr_t site;              // the holder's call site, for the lockup report
};

// A holder is not preempted: these raise the preemption guard (on the
// BSP until stage 5), as Linux's spin_lock() does.
void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

// Without the guard: the kernel lock's alone, which is held whenever
// the kernel runs and must not stop preemption. `site` names the caller.
void spin_lock_raw(struct spinlock *l, uintptr_t site);
void spin_unlock_raw(struct spinlock *l);
int  spin_trylock(struct spinlock *l);

// Interrupts off on this core, then the lock; the flags go back to
// spin_unlock_irqrestore().
uint64_t spin_lock_irqsave(struct spinlock *l);
void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags);

int spin_held(const struct spinlock *l);
int spin_held_here(const struct spinlock *l);   // by THIS core

// "Lockup suspected": a spin past SPIN_LOCKUP_MS logs once (on the BSP;
// an AP may not log before stage 5, so it only counts) and keeps
// spinning. Linux's CONFIG_DEBUG_SPINLOCK report.
#define SPIN_LOCKUP_MS 1000

// The last wait past SPIN_LOCKUP_MS: which lock, where it was waited for,
// where its holder took it, and on which cores.
struct spin_lockup_info {
    uintptr_t lock, waiter_site, holder_site;
    uint32_t waiter, holder;
    // The holder's last kernel-lock entry (percpu.h's bkl_*), when it is the kernel lock.
    uint64_t vec, rax, rip;
    int pid;
    int now_pid;   // what the holder core was running when the wait was noticed
    uint64_t tick_age_ms, tick_rip, tick_cs;   // the holder's last tick (an AP): is it ticking?
    int guard;     // the preemption guard's depth, which is the kernel-lock holder's
};
void spin_lockup_last(struct spin_lockup_info *out);
unsigned spin_lockups(void);

// The current core's index (0 = the BSP), safe before GS.base is set.
uint32_t spin_cpu_index(void);
void spin_percpu_ready(void);   // GS.base is set on the BSP (gdt_init())
extern int g_percpu_ready;      // ...and from then on a %gs load reads this core

#endif
