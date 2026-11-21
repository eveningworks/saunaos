// The big kernel lock (bkl.h). The lock is a ticket spinlock; what this
// file adds is the per-core depth and the checks on it.
#include "bkl.h"
#include "spinlock.h"
#include "percpu.h"
#include "irqflags.h"
#include "klog.h"
#include "kfmt.h"
#include "lockstat.h"
#include "smp.h"             // bkl_stat_zero_holds(): every core
#include "syscall_stall.h" // SYSCALL_STALL_MAX

#define FRAME_CS 18   // isr.asm's frame, as sched_internal.h's TF_CS
#define FRAME_RAX 14
#define FRAME_VECTOR 15
#define FRAME_RIP 17

SPINLOCK_COUNTED(g_bkl, "kernel");
static void fault(const char *what, int depth, uintptr_t site);

_Static_assert(BKL_SYSCALLS == SYSCALL_STALL_MAX,
               "bkl.h's BKL_SYSCALLS is the per-syscall table bound");

// Who takes it and who waits (bkl.h's keys); written under the lock.
static struct bkl_entry_stat g_entries[BKL_KEYS];

static uint32_t key_of(const uint64_t *regs) {
    if (!regs) return BKL_KEY_KERNEL;
    if (regs[FRAME_VECTOR] == 128 && regs[FRAME_RAX] < BKL_SYSCALLS)
        return BKL_KEY_SYSCALL(regs[FRAME_RAX]);
    return BKL_KEY_VECTOR(regs[FRAME_VECTOR]);
}

// Right after the take, still under the lock: the wait take() measured.
static void account_take(struct cpu *c, uint32_t key) {
    uint64_t w = g_bkl_stat.last_wait_tsc;
    struct bkl_entry_stat *e = &g_entries[key];
    c->bkl_key = key;
    c->bkl_takes++;
    e->takes++;
    if (w) {
        c->bkl_contended++;
        c->bkl_wait_tsc += w;
        e->contended++;
        e->wait_tsc += w;
        if (w > e->wait_max_tsc) e->wait_max_tsc = w;
    }
}

// The unlock, its hold (when hold times are armed) billed first to the
// entry that took it and to this core -- under the lock, like the rest.
static void release(struct cpu *c) {
    uint64_t h = lockstat_hold_end(&g_bkl_stat);
    if (h) {
        struct bkl_entry_stat *e = &g_entries[c->bkl_key < BKL_KEYS ? c->bkl_key : BKL_KEY_KERNEL];
        c->bkl_hold_tsc += h;
        e->hold_tsc += h;
        if (h > e->hold_max_tsc) e->hold_max_tsc = h;
    }
    spin_unlock_raw(&g_bkl);
}

void bkl_stat_zero_holds(void) {
    if (!bkl_held_here()) fault("hold totals zeroed without holding it", bkl_depth(), (uintptr_t)__builtin_return_address(0));
    for (uint32_t k = 0; k < BKL_KEYS; k++) {
        g_entries[k].hold_tsc = 0;
        g_entries[k].hold_max_tsc = 0;
    }
    for (int i = 0; i < smp_cpu_count(); i++) {
        struct cpu *c = (struct cpu *)smp_cpu_at(i);
        if (c) c->bkl_hold_tsc = 0;
    }
}

_Static_assert(CPU_LOCKLESS_SYSCALLS == BKL_SYSCALLS,
               "struct cpu's lockless_calls[] must cover every BKL syscall key");

void bkl_note_lockless(uint64_t nr) {
    struct cpu *c = this_cpu_raw();   // interrupts are off: this core's
    c->bkl_lockless++;
    if (nr < BKL_SYSCALLS) c->lockless_calls[nr]++;
}

// A snapshot: each core's count may move under the sum.
static uint64_t lockless_of(uint32_t key) {
    if (key < BKL_KEY_SYSCALL(0) || key >= BKL_KEY_KERNEL) return 0;
    uint64_t n = 0;
    for (int i = 0; i < smp_cpu_count(); i++) {
        const struct cpu *c = smp_cpu_at(i);
        if (c) n += c->lockless_calls[key - BKL_KEY_SYSCALL(0)];
    }
    return n;
}

int bkl_entry_stat(uint32_t key, struct bkl_entry_stat *out) {
    if (key >= BKL_KEYS) return 0;
    uint64_t lockless = lockless_of(key);
    if (!g_entries[key].takes && !lockless) return 0;
    *out = g_entries[key];
    out->lockless = lockless;
    return 1;
}
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
    account_take(this_cpu(), BKL_KEY_KERNEL);
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
    // DEPTH WITHOUT THE LOCK: this core is still waiting for it, and the
    // spin answered a cross-core call (smp_poll_ipis()) that took it --
    // which would run unlocked. A call function never takes the kernel
    // lock, Linux's rule for an IPI handler.
    if (c->bkl_depth != 0 && !spin_held_here(&g_bkl))
        fault("taken inside its own wait -- a cross-core call must not take it",
              c->bkl_depth, site);
    if (c->bkl_depth++ == 0) {
        spin_lock_raw(&g_bkl, site);
        account_take(c, key_of(regs));
        c->bkl_vec = regs ? regs[FRAME_VECTOR] : ~0ull;
        c->bkl_rax = regs ? regs[FRAME_RAX] : 0;
        c->bkl_rip = regs ? regs[FRAME_RIP] : site;
        // The SLOT, a plain load from this core's struct (interrupts are
        // off): its pid is looked up only by a lockup report or `cpus`,
        // and this runs on every first entry to the kernel.
        c->bkl_slot = c->sched_cur;
    }
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
        release(c);
    } else if (from_ring3(regs)) {
        fault("return to ring 3 still holding it", c->bkl_depth,
              (uintptr_t)__builtin_return_address(0));
        c->bkl_depth = 0;
        release(c);
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
        account_take(this_cpu(), BKL_KEY_KERNEL);
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
        release(c);
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
        account_take(c, BKL_KEY_KERNEL);
    }
    c->bkl_depth = depth;
    irq_restore(f);
}

void bkl_start_tail(const uint64_t *tf) {
    if (from_ring3(tf)) bkl_exit(tf);
}

int bkl_held_here(void) { return g_ready && spin_held_here(&g_bkl); }

int bkl_contended(void) {
    uint32_t next = __atomic_load_n(&g_bkl.next, __ATOMIC_ACQUIRE);
    uint32_t serving = __atomic_load_n(&g_bkl.serving, __ATOMIC_ACQUIRE);
    return next - serving > 1;   // a ticket queued behind the holder's
}
int bkl_held(void) { return spin_held(&g_bkl); }

int bkl_holder_core(void) {
    uint32_t h = __atomic_load_n(&g_bkl.holder, __ATOMIC_RELAXED);
    return h ? (int)h - 1 : -1;
}
