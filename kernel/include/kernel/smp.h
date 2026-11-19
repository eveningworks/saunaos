#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

// The other cores (docs/smp-design.md). Stage 3: each AP the MADT lists
// as enabled is started, set up like the BSP and PARKED -- halted with
// interrupts off, running nothing. The scheduler still runs on the BSP
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

#endif
