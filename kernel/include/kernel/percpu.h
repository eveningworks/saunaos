#ifndef KERNEL_PERCPU_H
#define KERNEL_PERCPU_H

// One `struct cpu` per core, found through GS.base -- Linux's per-CPU
// area, NT's KPRCB. Each core's GDT and TSS live inside it, so a core
// loading its own is one pointer (gdt_load_cpu()).
//
// **THE INVARIANT EVERY ENTRY PATH LEANS ON: RING 3's GS.base IS ALWAYS 0.**
// Nothing lets a program set it (no CR4.FSGSBASE, no ARCH_SET_GS, and a
// selector load from ring 3 takes base 0 from the descriptor), so a
// non-zero GS.base means the kernel's -- which is how isr.asm's paranoid
// entry tells the two apart when the saved CS cannot (an NMI inside
// syscall_entry's first instructions). Giving programs a GS.base breaks
// that test; it would need Linux's "compare against this CPU's value".
//
// The asm offsets below are checked against the struct in smp.c.

#include <stdint.h>

#define CPU_OFF_SELF      0
#define CPU_OFF_USER_RSP  8
#define CPU_OFF_TSS_RSP0  (32 + 4)   // offsetof(struct cpu, tss) + the TSS's own 4

#ifndef __ASSEMBLER__

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

// Ordered: a core is ONLINE from CPU_PARKED up (smp.c's online()).
enum cpu_state {
    CPU_ABSENT = 0,   // never started (nosmp, maxcpus=, or not enabled by firmware)
    CPU_FAILED,       // did not call in before the deadline; sent back to INIT
    CPU_STARTING,     // SIPI sent, the trampoline has not called in
    CPU_ALIVE,        // in C on its own stack, replaying the BSP's setup
    CPU_PARKED,       // set up and halted -- an AP's whole life until stage 5
    CPU_RUNNING,      // the BSP
};

struct cpu {
    struct cpu *self;          // CPU_OFF_SELF: gs:0, how this_cpu() finds the rest
    uint64_t user_rsp;         // CPU_OFF_USER_RSP: syscall_entry's scratch word
    uint32_t index;            // 0 is the BSP
    uint32_t apic_id;
    volatile uint32_t state;   // enum cpu_state; the AP writes it, the BSP polls
    uint32_t pad0;
    struct tss tss;            // offset 32 -- CPU_OFF_TSS_RSP0 depends on it
    uint64_t gdt[7];           // gdt.h's layout; 5-6 are this core's TSS
    // What the AP read back after replaying the BSP's setup: smp_test.c
    // compares them, so a register the replay forgets shows as a red test
    // rather than as a fault the first time stage 5 runs code there.
    uint64_t seen_cr0, seen_cr4, seen_efer, seen_pat, seen_mtrr_def;
};

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(c));
    return c;
}

#endif
#endif
