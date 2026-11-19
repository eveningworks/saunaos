#ifndef KERNEL_TLB_H
#define KERNEL_TLB_H

// TLB shootdown (docs/smp-design.md, stage 6): a page-table change other
// cores may have cached is flushed on THEM before it counts -- before a
// frame it unmapped is freed, before a downgrade to read-only is relied
// on. Linux's flush_tlb_mm_range(), NT's KeFlushEntireTb() in its first,
// whole-space shape: a target reloads CR3, no per-page list.
//
// A target is every other online core whose struct cpu->cr3 names the
// space -- Linux's mm_cpumask, kept per core instead of per space. A
// core idling in the kernel keeps the CR3 it last loaded (lazy TLB), so
// it stays a target until it loads another. Parked cores hold the
// kernel's, so a user shootdown sends nothing until stage 5 runs
// processes on them; a kernel-map change goes to every online core.
//
// **A SENDER HOLDS THE KERNEL LOCK** (bkl.h), which is what makes one
// mailbox per core enough and keeps a CR3 load on another core from
// slipping in between choosing the targets and sending; it waits for
// every acknowledgement with interrupts off. **SO A CORE WAITING WITH
// INTERRUPTS OFF MUST ANSWER ONE ITSELF**: spinlock.c's spin does
// (tlb_service()), as Linux 2.0's lock_kernel() loop checked
// smp_invalidate_needed -- a core queued for the kernel lock is exactly
// the one that cannot take the IPI.
//
// No global pages are mapped (no PTE sets G), so a CR3 reload is a whole
// flush, kernel map included.
#include <stdint.h>

#define TLB_FLUSH_VECTOR 0xFD   // Linux's old INVALIDATE_TLB_VECTOR

// Load CR3 and record it as this core's (every CR3 load after boot goes
// through here: vmm_switch_address_space()).
void tlb_load_cr3(uint64_t pml4_phys);

// One user page of `pml4_phys` changed: invlpg here if it is live, and
// a shootdown to every other core with it loaded.
void tlb_flush_user_page(uint64_t pml4_phys, uint64_t vaddr);

// The same for a whole space at once (fork's COW downgrade): a reload
// here if live, one shootdown for the others.
void tlb_flush_user_space(uint64_t pml4_phys);

// A kernel-map change (paging.c): a reload here and on every online core.
void tlb_flush_kernel(void);

// A space about to be freed: every other core that still has it loaded
// (idling on it) loads the kernel's instead. The caller has already
// moved this core off it.
void tlb_leave_space(uint64_t pml4_phys);

// A pending request on this core, answered: from the IPI, and from any
// spin that waits with interrupts off. Cheap when there is none.
void tlb_service(void);

// For tlb_test.c and lscpu-style reporting.
struct tlb_stats {
    uint64_t shootdowns;   // requests that had at least one target
    uint64_t ipis;         // targets sent to
    uint64_t timeouts;     // a target that did not answer in time (then CPU_WEDGED)
};
void tlb_stats_get(struct tlb_stats *out);

#endif
