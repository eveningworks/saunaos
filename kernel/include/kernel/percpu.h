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
#include "context_switch.h"   // struct kernel_context: an AP's idle context

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
    CPU_WEDGED,       // started, then faulted or did not finish a call: no more calls
    CPU_STARTING,     // SIPI sent, the trampoline has not called in
    CPU_ALIVE,        // in C on its own stack, replaying the BSP's setup
    CPU_PARKED,       // set up and halted -- an AP's whole life without smp=run
    CPU_RUNNING,      // the BSP, and an AP that schedules processes (smp=run)
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
    int bkl_depth;             // bkl.h: this core's nesting of the kernel lock
    uint32_t pad1;
    // smp_call()'s mailbox: the BSP writes fn/arg, then bumps call_seq
    // and sends the IPI; the core runs it and stores call_seq to
    // call_done.
    void (*volatile call_fn)(void *);
    void *volatile call_arg;
    volatile uint32_t call_seq, call_done;
    volatile uint32_t call_running;   // inside fn: a spin in it must not run it again
    uint32_t pad3;
    // tlb.h: the address space this core last loaded -- kept while it
    // idles in the kernel (lazy TLB), so a shootdown for that space
    // still reaches it -- and its shootdown mailbox, written by the one
    // sender the kernel lock allows.
    volatile uint64_t cr3;
    volatile uint64_t tlb_leave;   // a space being destroyed: load the kernel's if it is ours
    volatile uint32_t tlb_seq, tlb_done;
    // The scheduler's per-core state (sched_internal.h's current_index,
    // rotation_pos and g_need_resched are these): what runs HERE. The
    // BSP's starts as gdt.c sets it -- -1 and ROT_KERNEL, not 0.
    int sched_cur, sched_rot;
    volatile int sched_resched;
    volatile int sched_go;     // smp=run: leave the park loop and schedule
    uint64_t run_start_ns, slice_start_ns, slice_end_ns;
    // An AP's idle context: what a switch away from its idle loop saves,
    // as scheduler.c's kernel_* statics are the BSP kernel context's.
    struct kernel_context idle_kctx;
    int idle_isr_depth, idle_preempt_depth, idle_bkl_depth;
    uint32_t pad2;
    uint64_t idle_left_tsc, idle_offcpu_tsc;
    uint64_t busy_ns;          // time a process ran here: per-core load
    // How this core last took the kernel lock from an entry (bkl_enter()):
    // read by a waiter that has waited too long (spinlock.c).
    uint64_t bkl_vec, bkl_rax, bkl_rip;
    int bkl_pid;
    uint64_t tick_tsc, tick_rip, tick_cs;   // this AP's last tick: a lockup report's "is it ticking?"
    uint64_t wedge_vec;   // the exception that wedged it, + 1 (0: none)
};

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(c));
    return c;
}

#endif
#endif
