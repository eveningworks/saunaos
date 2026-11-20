// `make DEBUG_PREEMPT=1`: a per-core read made where the reader could be
// preempted and moved to another core is logged, once per call site --
// Linux's CONFIG_DEBUG_PREEMPT and its "using smp_processor_id() in
// preemptible code". this_cpu() and spin_cpu_index() call it (percpu.h).
//
// Both of 2026-11-20's migration bugs were this shape: a "which core am
// I" read, then a preemption that moved the thread, then a use of the
// stale answer (the two-step current_index read, shoot()'s `me`). Each
// took hours to find from its symptom; this names the line instead.
//
// **A REPORT IS A CANDIDATE, NOT A BUG**: a reader that uses the answer
// only as a hint, or re-reads it with interrupts off, is fine -- it takes
// this_cpu_raw()/spin_cpu_index_raw() and says why.
#include "percpu.h"
#include "spinlock.h"
#include "smp.h"
#include "scheduler.h"
#include "irqflags.h"
#include "klog.h"
#include "kfmt.h"
#include "ksyms.h"
#include "reloc.h"

int debug_preempt_unsafe(int irqs_on, int guard_depth, int slot, int scheduling) {
    return irqs_on && guard_depth == 0 && slot >= 0 && scheduling;
}

#ifdef SAUNAOS_DEBUG_PREEMPT

// Each site logs once; beyond the table they are counted only, and the
// table filling says so (a cap that logs when hit).
#define DEBUG_PREEMPT_SITES 64
static uintptr_t g_sites[DEBUG_PREEMPT_SITES];
static int g_nsites, g_full_said;
static unsigned g_reports;

unsigned debug_preempt_reports(void) { return __atomic_load_n(&g_reports, __ATOMIC_RELAXED); }

// With interrupts off: the caller's.
static void note(uintptr_t site, int slot) {
    __atomic_fetch_add(&g_reports, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < g_nsites; i++)
        if (g_sites[i] == site) return;
    if (g_nsites == DEBUG_PREEMPT_SITES) {
        if (!g_full_said++)
            klog_printf(KLOG_WARN "preempt: more than %d call sites -- the rest are counted only\n",
                        DEBUG_PREEMPT_SITES);
        return;
    }
    g_sites[g_nsites++] = site;
    uint32_t off = 0;
    const char *sym = ksyms_lookup(site, &off);
    klog_printf(KLOG_WARN "preempt: a per-core read at %s+0x%x (link-time 0x%lx) on core %u, "
                "slot %d, with interrupts on and the guard down -- it can move cores under it\n",
                sym ? sym : "?", off, (unsigned long)(site - kernel_reloc_delta()),
                spin_cpu_index_raw(), slot);
}

void debug_preempt_check(uintptr_t site) {
    if (!g_percpu_ready) return;
    uint64_t fl;
    __asm__ volatile ("pushfq; popq %0" : "=r"(fl));
    if (!(fl & 0x200)) return;   // interrupts off: the common case, and cheap
    uint64_t f = irq_save();
    int slot = this_cpu_read(sched_cur);
    if (debug_preempt_unsafe(1, scheduler_preempt_depth(), slot, smp_scheduling()))
        note(site, slot);
    irq_restore(f);
}

#else
unsigned debug_preempt_reports(void) { return 0; }
#endif
