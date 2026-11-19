#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

// The other cores (docs/smp-design.md). Stage 3: each AP the MADT lists
// as enabled is started, set up like the BSP and PARKED -- halted,
// running nothing but what smp_call() hands it. The scheduler still runs on the BSP
// alone; `nosmp` (or `maxcpus=1`) starts none, `maxcpus=N` at most N
// cores counting the BSP, as Linux's words do.

#include <stdint.h>

struct cpu;

// After lapic_init(), cpu_info_init() and the clocksource, before the
// scheduler. Never fails the boot: a core that does not call in is
// logged and left in INIT.
void smp_init(void);

// Cores known to this kernel: the BSP, then every AP it tried to start.
// The struct is read-only to anyone but smp.c and the core itself.
int smp_cpu_count(void);
const struct cpu *smp_cpu_at(int index);

// How many are online (the BSP counts), and whether the core with this
// APIC id is one of them -- QUERY_CPUS's QUERY_CPU_ONLINE.
int smp_online_count(void);
int smp_apic_online(uint32_t apic_id);

// Where the AP trampoline was (0 if none) -- for smp_test.c, which
// checks the page went back to writable and NX.
uint64_t smp_trampoline_phys(void);

// Run fn(arg) on core `index` (0 runs it here, interrupts off). Only the
// BSP calls until stage 5. `wait` waits up to a second for it to finish;
// 0 for a core that is not parked, a mailbox still holding the last call
// (a wait = 0 call is followed by smp_call_wait()), or a call that did
// not finish -- which leaves the core CPU_WEDGED, given no more calls.
// fn runs with interrupts off on a core that may touch nothing shared
// but what fn's caller arranged: no log, no heap, no kernel lock.
#define SMP_CALL_VECTOR 0xFB   // Linux's CALL_FUNCTION_VECTOR
int smp_call(int index, void (*fn)(void *), void *arg, int wait);
int smp_call_wait(int index, uint32_t ms);    // a call made with wait = 0
int smp_call_others(void (*fn)(void *), void *arg);   // every AP, waiting; how many ran it

// Every AP takes the BSP's PAT and MTRRs again (after a WC change).
void smp_memory_types_changed(void);

// isr_dispatch() on an AP: before anything shared.
void smp_ap_interrupt(uint64_t vector);

#endif
