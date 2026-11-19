// The big kernel lock (bkl.h). The lock is a ticket spinlock; what this
// file adds is the per-core depth and the checks on it.
#include "bkl.h"
#include "spinlock.h"
#include "percpu.h"
#include "irqflags.h"
#include "klog.h"
#include "kfmt.h"

#define FRAME_CS 18   // isr.asm's frame, as sched_internal.h's TF_CS

static struct spinlock g_bkl;
static int g_ready;
static unsigned g_faults;

unsigned bkl_faults(void) { return __atomic_load_n(&g_faults, __ATOMIC_RELAXED); }

// The first few, on the BSP only: an AP may not log before stage 5.
static void fault(const char *what, int depth, uintptr_t site) {
    unsigned n = __atomic_fetch_add(&g_faults, 1, __ATOMIC_RELAXED);
    if (n >= 8 || this_cpu()->index != 0) return;
    klog_printf(KLOG_ERR "bkl: %s (depth %d) at %p\n", what, depth, (void *)site);
}

static int from_ring3(const uint64_t *regs) { return regs && (regs[FRAME_CS] & 3); }

void bkl_boot(void) {
    spin_percpu_ready();
    spin_lock_raw(&g_bkl, (uintptr_t)__builtin_return_address(0));
    this_cpu()->bkl_depth = 1;
    g_ready = 1;
}

void bkl_enter(const uint64_t *regs) {
    if (!g_ready) return;
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    uintptr_t site = (uintptr_t)__builtin_return_address(0);
    if (from_ring3(regs) && c->bkl_depth != 0) {
        // Something went back to ring 3 holding it. This entry is depth
        // 1; the lock is already ours, so it is kept, not retaken.
        fault("entry from ring 3 already holding it", c->bkl_depth, site);
        if (spin_held_here(&g_bkl)) {
            c->bkl_depth = 1;
            irq_restore(f);
            return;
        }
        c->bkl_depth = 0;
    }
    if (c->bkl_depth++ == 0) spin_lock_raw(&g_bkl, site);
    irq_restore(f);
}

void bkl_exit(const uint64_t *regs) {
    if (!g_ready) return;
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    if (c->bkl_depth <= 0) {
        fault("exit without holding it", c->bkl_depth, (uintptr_t)__builtin_return_address(0));
        c->bkl_depth = 0;
    } else if (--c->bkl_depth == 0) {
        spin_unlock_raw(&g_bkl);
    } else if (from_ring3(regs)) {
        fault("return to ring 3 still holding it", c->bkl_depth,
              (uintptr_t)__builtin_return_address(0));
        c->bkl_depth = 0;
        spin_unlock_raw(&g_bkl);
    }
    irq_restore(f);
}

int bkl_depth(void) { return g_ready ? this_cpu()->bkl_depth : 0; }

// Interrupts are off: switch_to() runs under the interrupt gate.
void bkl_load_depth(int depth) {
    if (!g_ready) return;
    uintptr_t site = (uintptr_t)__builtin_return_address(0);
    if (!spin_held_here(&g_bkl)) {
        fault("switch without holding it", depth, site);
        spin_lock_raw(&g_bkl, site);
    }
    if (depth < 1) {
        // Every context resumes inside the kernel: in switch_to()'s
        // caller, or in isr_start_frame, which starts at 1.
        fault("switch into a context at depth < 1", depth, site);
        depth = 1;
    }
    this_cpu()->bkl_depth = depth;
}

int bkl_drop_all(void) {
    if (!g_ready) return 0;
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    int d = c->bkl_depth;
    if (d > 0) {
        c->bkl_depth = 0;
        spin_unlock_raw(&g_bkl);
    }
    irq_restore(f);
    return d;
}

void bkl_retake(int depth) {
    if (!g_ready || depth <= 0) return;
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    if (c->bkl_depth != 0) {
        // An interrupt in between came back to us still holding it.
        fault("retake while already holding it", c->bkl_depth,
              (uintptr_t)__builtin_return_address(0));
    } else {
        spin_lock_raw(&g_bkl, (uintptr_t)__builtin_return_address(0));
    }
    c->bkl_depth = depth;
    irq_restore(f);
}

void bkl_start_tail(const uint64_t *tf) {
    if (from_ring3(tf)) bkl_exit(tf);
}

int bkl_held_here(void) { return g_ready && spin_held_here(&g_bkl); }
int bkl_held(void) { return spin_held(&g_bkl); }
