// DEBUG_PREEMPT's rule (preempt_debug.c), and in a build with the flag
// the check itself: a read the rule forbids is counted, one it allows is
// not. The KTEST context is the boot core's kernel context, which never
// moves -- so the forbidden read is made in a spawned process's syscall
// path in the debug build's own soak, and here the rule is driven direct.
#include "percpu.h"
#include "irqflags.h"
#include "scheduler.h"
#include "ktest.h"

KTEST("smp", "DEBUG_PREEMPT flags a read only where a move can happen") {
    KTEST_ASSERT(debug_preempt_unsafe(1, 0, 3, 1));    // a process, preemptible, APs scheduling
    KTEST_ASSERT(!debug_preempt_unsafe(0, 0, 3, 1));   // interrupts off
    KTEST_ASSERT(!debug_preempt_unsafe(1, 1, 3, 1));   // the guard up
    KTEST_ASSERT(!debug_preempt_unsafe(1, 0, -1, 1));  // a context that never moves
    KTEST_ASSERT(!debug_preempt_unsafe(1, 0, 3, 0));   // one core schedules: nowhere to go
}

#ifdef SAUNAOS_DEBUG_PREEMPT
KTEST("smp", "DEBUG_PREEMPT: the kernel context's reads are never reported") {
    unsigned before = debug_preempt_reports();
    uint64_t f = irq_save();
    __asm__ volatile ("sti");
    for (int i = 0; i < 100; i++) (void)this_cpu();
    irq_restore(f);
    KTEST_ASSERT_EQ(debug_preempt_reports(), before);
}
#endif
