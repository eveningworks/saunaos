// Kernel threads -- see kthread.h.
#include "kthread.h"
#include "sched_internal.h"
#include "vmm.h"     // vmm_kernel_pml4_phys()
#include "gdt.h"     // SEL_KERNEL_CODE/_DATA
#include "fpu.h"
#include "errno.h"
#include "string.h"
#include "irqflags.h"

// Where a kernel thread starts: the trapframe below puts fn in RDI and
// arg in RSI, so this is entered as kthread_entry(fn, arg).
static void kthread_entry(void (*fn)(void *), void *arg) {
    fn(arg);
    scheduler_on_kthread_exit();
}

int kthread_start(const char *name, void (*fn)(void *), void *arg) {
    if (!fn) return -EINVAL;
    int slot = slot_claim();
    if (slot < 0) return -EAGAIN;

    // A spawn's first trapframe, at ring 0: isr_resume_frame's iretq
    // lands in kthread_entry() on this slot's own stack, below the frame.
    // RFLAGS has IF CLEAR (bit 1 is the reserved always-one bit).
    uint64_t *tf = (uint64_t *)(kernel_stack_top(slot) - TRAPFRAME_WORDS * 8);
    for (int i = 0; i < TRAPFRAME_WORDS; i++) tf[i] = 0;
    tf[TF_RDI]    = (uint64_t)(uintptr_t)fn;
    tf[TF_RSI]    = (uint64_t)(uintptr_t)arg;
    tf[TF_RIP]    = (uint64_t)(uintptr_t)kthread_entry;
    tf[TF_CS]     = SEL_KERNEL_CODE;
    tf[TF_RFLAGS] = 0x2;
    tf[TF_RSP]    = ((uint64_t)(uintptr_t)tf & ~15ull) - 8;   // a callee's alignment
    tf[TF_SS]     = SEL_KERNEL_DATA;

    procs[slot].pml4_phys  = vmm_kernel_pml4_phys();
    procs[slot].kernel_rsp = (uint64_t)(uintptr_t)tf;
    proc_start_context(slot, tf);
    kstack_arm_slot(slot);
    fpu_init_state(procs[slot].fpu);
    procs[slot].wait_chan   = 0;
    procs[slot].wait_reason = 0;
    procs[slot].wake_at_ns  = 0;
    procs[slot].tgid     = procs[slot].pid;
    procs[slot].ppid     = 0;
    procs[slot].pgid     = procs[slot].pid;
    procs[slot].sid      = procs[slot].pid;
    procs[slot].fs_base  = 0;
    procs[slot].detached = 1;
    procs[slot].kthread  = 1;
    procs[slot].ustack_lo = procs[slot].ustack_hi = 0;
    sched_class_reset(slot, -1);
    procs[slot].vruntime = g_min_vruntime;
    signal_state_reset(slot);
    procs[slot].blocked = 0;
    procs[slot].ready   = 0;
    procs[slot].cpu_ns  = 0;
    procs[slot].name[0] = '[';
    k_strlcpy(procs[slot].name + 1, name, sizeof procs[slot].name - 2);
    k_strlcpy(procs[slot].name + k_strlen(procs[slot].name), "]",
              sizeof procs[slot].name - k_strlen(procs[slot].name));
    procs[slot].ext->exec_path[0] = '\0';
    k_memset(&procs[slot].mm, 0, sizeof procs[slot].mm);
    procs[slot].ext->cwd.path[0] = '\0';
    procs[slot].ext->sandbox = 0;
    // EVERYTHING BEFORE READY: the caller is preemptible, and a short fn
    // can run to its end and free the slot before the next line here.
    int pid = procs[slot].pid;
    uint64_t f = irq_save();
    alive_count++;
    procs[slot].state = SCHED_READY;
    slot_unclaim(slot);
    irq_restore(f);
    return pid;
}

int kthread_is(int pid) {
    int s = pid_slot(pid);
    return s >= 0 && procs[s].state != SCHED_UNUSED && procs[s].kthread;
}
