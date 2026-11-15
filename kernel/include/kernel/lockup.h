#ifndef KERNEL_LOCKUP_H
#define KERNEL_LOCKUP_H

#include <stdint.h>

// THE LOCKUP DETECTOR -- Linux's softlockup, hardlockup and hung_task, for
// one CPU. Each says WHICH code stopped the machine, where the hardware
// watchdog (kernel/watchdog.h) can only restart it.
//
//   SOFT: the timer keeps arriving, but every tick for LOCKUP_SOFT_SECS
//     finds the kernel on the CPU with the preemption guard raised, no
//     switch made, and a process ready to run -- a loop with preemption
//     off. Seen from the tick, with the stuck code's own frame.
//   HARD: the timer has stopped arriving for LOCKUP_HARD_SECS -- a loop
//     with interrupts off. Seen from an NMI that the CPU's cycle counter
//     raises (architectural perfmon v2+, as Linux's perf NMI watchdog).
//     No counter (TCG, a CPU without one): not detected, and the hardware
//     watchdog is what restarts such a machine.
//   HUNG TASK: a process parked inside the kernel on a lock or a disk for
//     hung_task= seconds (default LOCKUP_HUNG_SECS) with no wake. Reported
//     once per park, with its stack, and NOT a panic: a long legitimate
//     hold (a whole-disk check under fs_exclusive_begin()) looks the same.
//
// `lockup=panic` (the default) logs the report and panics, so the panic
// store keeps it and the machine restarts; `lockup=warn` logs and carries
// on; `lockup=off` arms neither CPU check (the hung-task one is
// `hung_task=off|<seconds>`).

#define LOCKUP_SOFT_SECS 20
#define LOCKUP_HARD_SECS 10
#define LOCKUP_HUNG_SECS 120

// From clockevent_tick(), with the interrupted frame.
void lockup_tick(uint64_t *regs);
// From the idle halt: an idle CPU is not a locked one.
void lockup_idle(void);
// From isr_dispatch() for vector 2, BEFORE the kernel debugger sees it:
// 1 when the NMI was the cycle counter's (handled), 0 otherwise.
int  lockup_nmi(uint64_t *regs);
// panic_finish(): no NMI may land in the countdown, which runs with
// interrupts off on purpose.
void lockup_panic_stop(void);

// The soft-lockup rule as a pure function, for the KTEST: given one tick's
// facts and the stretch so far (ns, 0 = none), the new stretch.
uint64_t lockup_soft_step(uint64_t stretch_ns, uint64_t since_last_ns, int guarded,
                          int idle, int switched, int others_ready);

#endif
