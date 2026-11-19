#ifndef KERNEL_BKL_H
#define KERNEL_BKL_H

// The big kernel lock: one core in the kernel at a time, while ring 3
// runs on all of them (docs/smp-design.md, stage 4). Linux 2.0's
// kernel_flag, in its shape: OWNED BY A CORE AND RECURSIVE, so an
// interrupt on the holding core nests instead of deadlocking, with the
// depth in that core's struct cpu.
//
// **THE DEPTH TRAVELS WITH THE CONTEXT, THE LOCK STAYS WITH THE CORE.**
// A switch happens inside the kernel, so the core holds the lock across
// it; switch_to() saves the outgoing context's depth and loads the
// incoming one's, as it does isr_depth and the preemption depth. So a
// sleep needs no drop of its own: while a context sleeps it holds
// nothing, and whoever resumes it is in the kernel already.
//
// **SO THE LOCK PASSES TO ANOTHER CORE ONLY WHERE, ON ONE CORE, ANOTHER
// CONTEXT COULD ALREADY RUN**: a context preempted in the kernel holds
// nothing once the one switched to returns to ring 3. Code that is safe
// against preemption on one core is safe under this lock on many, and
// the preemption guard and irq_save() sections keep meaning what they
// mean now.
//
// Taken by isr_dispatch() for every vector but the paranoid ones (NMI,
// #DF, #MC, #DB, #BP -- an NMI landing inside take() on its own core
// would queue behind its own ticket) and the call-function IPI (its
// sender waits for it while holding this). Released when the depth
// reaches 0, which is exactly on the way back to ring 3.
#include <stdint.h>

void bkl_boot(void);   // after gdt_init(): the BSP holds it, depth 1

void bkl_enter(const uint64_t *regs);
void bkl_exit(const uint64_t *regs);

// For switch_to(): this core's depth, and the incoming context's.
int  bkl_depth(void);
void bkl_load_depth(int depth);

// Around a stretch that holds nothing: the idle halt, and a hand-rolled
// iretq into ring 3 (the legacy loader). Returns the depth to retake.
int  bkl_drop_all(void);
void bkl_retake(int depth);

// A fresh context's first resume (isr_start_frame): it starts at depth
// 1, and a ring-3 frame releases it before the iretq.
void bkl_start_tail(const uint64_t *tf);

int bkl_held_here(void);
int bkl_held(void);
int bkl_contended(void);   // another core is queued for it

// Broken accounting -- entering from ring 3 at a non-zero depth,
// returning to ring 3 still holding it, an exit at depth 0, a switch
// that does not hold it. Each is counted and logged (the first few) and
// repaired; harmless on one core, a deadlock from stage 5.
unsigned bkl_faults(void);

#endif
