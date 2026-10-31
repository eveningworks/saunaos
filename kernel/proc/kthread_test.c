// Kernel threads (kthread.h): it runs, at ring 0 on the kernel's page
// table, sleeps and wakes, refuses signals, and its return frees the slot.
// The KTEST body is the kernel context, which the timer preempts, so the
// thread gets the CPU while this loop polls.
#include "ktest.h"
#include "kthread.h"
#include "scheduler.h"
#include "signal.h"
#include "signal_abi.h"
#include "proc_info.h"
#include "vmm.h"
#include "string.h"
#include "timer.h"

#define KT_TIMEOUT_TICKS 3000

struct kt_probe {
    volatile int phase;      // 1 = running and about to sleep, 2 = returning
    volatile int go;
    uint16_t cs;
    uint64_t cr3;
};

static void probe_fn(void *arg) {
    struct kt_probe *p = arg;
    __asm__ volatile ("mov %%cs, %0" : "=r"(p->cs));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(p->cr3));
    p->phase = 1;
    for (;;) {
        scheduler_wait_arm(p);   // armed BEFORE the test, as every waiter does
        if (p->go) { scheduler_wait_disarm(); break; }
        scheduler_block_kernel(p, SCHED_WAIT_EVENT);
    }
    p->phase = 2;
}

static int wait_until(volatile int *v, int want) {
    uint64_t start = coarse_ticks();
    while (*v != want)
        if (coarse_ticks() - start >= KT_TIMEOUT_TICKS) return 0;
    return 1;
}

KTEST("kthread", "a kernel thread runs at ring 0, sleeps, wakes and is freed") {
    static struct kt_probe p;
    k_memset(&p, 0, sizeof p);
    int pid = kthread_start("ktest", probe_fn, &p);
    KTEST_ASSERT(pid > 0);
    KTEST_ASSERT(kthread_is(pid));
    KTEST_ASSERT(wait_until(&p.phase, 1));
    KTEST_ASSERT_EQ(p.cs & 3, 0);
    KTEST_ASSERT_EQ(p.cr3 & ~0xfffull, vmm_kernel_pml4_phys());

    // Parked, not spinning: the state a wake needs to find.
    struct proc_info info;
    uint64_t start = coarse_ticks();
    int blocked = 0;
    while (coarse_ticks() - start < KT_TIMEOUT_TICKS) {
        if (scheduler_proc_info_pid(pid, &info) && info.state == PROC_STATE_BLOCKED) { blocked = 1; break; }
    }
    KTEST_ASSERT(blocked);
    KTEST_ASSERT(k_strcmp(info.name, "[ktest]") == 0);
    KTEST_ASSERT_EQ(info.ppid, 0);

    // No signal reaches it, SIGKILL included.
    KTEST_ASSERT_EQ(signal_send(pid, SIGTERM), 0);
    KTEST_ASSERT_EQ(signal_send(pid, SIGKILL), 0);
    KTEST_ASSERT_EQ(signal_send(pid, SIGSTOP), 0);
    KTEST_ASSERT(kthread_is(pid));

    p.go = 1;
    scheduler_wake(&p, 0);
    KTEST_ASSERT(wait_until(&p.phase, 2));
    start = coarse_ticks();
    while (kthread_is(pid) && coarse_ticks() - start < KT_TIMEOUT_TICKS) {}
    KTEST_ASSERT(!kthread_is(pid));
    KTEST_ASSERT(!scheduler_pid_alive(pid));
}
