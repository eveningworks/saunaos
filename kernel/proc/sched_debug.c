// The scheduler for the kernel debugger -- see sched_debug.h. Read-only
// and lock-free on purpose: it runs with the machine stopped at an
// arbitrary instruction, possibly inside the scheduler itself.
#include "sched_debug.h"
#include "sched_internal.h"
#include "string.h"
#include "vmm.h"   // vmm_kernel_pml4_phys()
#include "smp.h"   // every core's kernel context and what it runs
#include "percpu.h"
#include "kfmt.h"

// An online core's struct, or NULL: an AP never started has a zeroed
// one, whose sched_cur would name slot 0. The boot core always counts --
// a `kdebug=...,wait` stop comes before smp_init() marks it running.
static const struct cpu *core_at(int c) {
    const struct cpu *p = smp_cpu_at(c);
    return p && (c == 0 || p->state >= CPU_PARKED) ? p : 0;
}

static int core_running(int slot) {
    for (int c = 0; c < smp_cpu_count(); c++) {
        const struct cpu *p = core_at(c);
        if (p && p->sched_cur == slot) return c;
    }
    return -1;
}

static void from_slot(int i, struct sched_debug_thread *out) {
    const struct sched_process *p = &procs[i];
    out->tid = p->pid;
    out->core = core_running(i);
    out->running = out->core >= 0;
    out->state = (int)p->state;
    out->stopped = p->stopped;
    k_strlcpy(out->name, p->name[0] ? p->name : "?", sizeof out->name);
    out->kctx = out->running ? 0 : &p->kctx;
    out->pml4 = p->pml4_phys;
}

// Core c's kernel context: the boot core's is scheduler.c's, an AP's is
// its idle loop's (percpu.h's idle_kctx).
static void kernel_thread(int c, const struct cpu *cpu, struct sched_debug_thread *out) {
    const struct kernel_context *k = c == 0 ? scheduler_kernel_kctx() : &cpu->idle_kctx;
    out->tid = SCHED_DEBUG_KERNEL_TID + c;
    out->running = cpu->sched_cur < 0;
    out->core = out->running ? c : -1;
    out->state = -1;
    out->stopped = 0;
    if (c == 0) k_strlcpy(out->name, "kernel", sizeof out->name);
    else k_snprintf(out->name, sizeof out->name, "kernel/%d", c);
    out->kctx = out->running || !k->rip ? 0 : k;
    out->pml4 = vmm_kernel_pml4_phys();
}

static int live(int i) {
    return procs[i].state != SCHED_UNUSED && procs[i].state != SCHED_ZOMBIE;
}

int sched_debug_thread(int n, struct sched_debug_thread *out) {
    for (int c = 0; c < smp_cpu_count(); c++) {
        const struct cpu *cpu = core_at(c);
        if (!cpu) continue;
        if (n-- == 0) { kernel_thread(c, cpu, out); return 1; }
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!live(i)) continue;
        if (n-- == 0) { from_slot(i, out); return 1; }
    }
    return 0;
}

int sched_debug_find(int tid, struct sched_debug_thread *out) {
    if (SCHED_DEBUG_IS_KERNEL(tid)) {
        int c = tid - SCHED_DEBUG_KERNEL_TID;
        const struct cpu *cpu = c < smp_cpu_count() ? core_at(c) : 0;
        if (!cpu) return 0;
        kernel_thread(c, cpu, out);
        return 1;
    }
    int s = pid_slot(tid);
    if (s < 0 || !live(s)) return 0;
    from_slot(s, out);
    return 1;
}
