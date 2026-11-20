// Each core's state, as a fact (QUERY_CPUSTATE) and as the debug
// console's `cpus` -- what had to be read through QEMU's gdbstub when a
// core stopped answering. One fill serves both, so the two cannot
// disagree. It READS another core's struct cpu without stopping it: a
// busy core's fields may be a moment apart, which a diagnostic accepts
// (Linux's /proc reads are no more atomic).
#include "smp.h"
#include "percpu.h"
#include "bkl.h"
#include "gdt.h"         // gdt_boot_cpu()
#include "query.h"
#include "initcall.h"
#include "scheduler.h"   // scheduler_slot_pid()
#include "cpuinfo.h"     // cpu_tsc_hz()
#include "random_hw.h"   // arch_rdtsc()
#include "kfmt.h"
#include "string.h"

static uint64_t abi_state(uint32_t st) {
    switch (st) {
    case CPU_FAILED:   return QUERY_CPUSTATE_FAILED;
    case CPU_WEDGED:   return QUERY_CPUSTATE_WEDGED;
    case CPU_STOPPED:  return QUERY_CPUSTATE_STOPPED;
    case CPU_STARTING: return QUERY_CPUSTATE_STARTING;
    case CPU_ALIVE:    return QUERY_CPUSTATE_ALIVE;
    case CPU_PARKED:   return QUERY_CPUSTATE_PARKED;
    case CPU_RUNNING:  return QUERY_CPUSTATE_RUNNING;
    default:           return QUERY_CPUSTATE_ABSENT;
    }
}

static int cpustate_count(void) { return smp_cpu_count(); }

static int cpustate_fill(int index, void *out) {
    const struct cpu *c = smp_cpu_at(index);
    if (!c) return 0;
    struct query_cpustate *q = out;
    k_memset(q, 0, sizeof *q);
    uint32_t st = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
    q->index = c->index;
    q->apic_id = c->apic_id;
    q->state = abi_state(st);
    int slot = __atomic_load_n(&c->sched_cur, __ATOMIC_RELAXED);
    q->pid = slot >= 0 ? scheduler_slot_pid(slot) : 0;
    q->bkl_depth = (uint64_t)(c->bkl_depth > 0 ? c->bkl_depth : 0);
    q->bkl_holder = bkl_holder_core() == (int)c->index;
    uint64_t t = __atomic_load_n(&c->tick_tsc, __ATOMIC_RELAXED), hz = cpu_tsc_hz(), now = arch_rdtsc();
    uint64_t per_us = hz >= 1000000 ? hz / 1000000 : 1;
    q->tick_age_ns = !t ? ~0ull : now > t ? (now - t) / per_us * 1000 : 0;
    q->tick_rip = c->tick_rip;
    q->tick_cs = c->tick_cs;
    q->wedge_vec = c->wedge_vec;
    q->nmi_stray = c->nmi_stray;
    q->calls_taken = c->calls_taken;
    q->call_pending = __atomic_load_n(&c->call_seq, __ATOMIC_ACQUIRE) !=
                      __atomic_load_n(&c->call_done, __ATOMIC_ACQUIRE);
    q->flushes_taken = c->flushes_taken;
    q->flush_pending = __atomic_load_n(&c->tlb_seq, __ATOMIC_ACQUIRE) !=
                       __atomic_load_n(&c->tlb_done, __ATOMIC_ACQUIRE);
    q->bkl_vec = c->bkl_vec;
    q->bkl_rip = c->bkl_rip;
    q->bkl_pid = c->bkl_slot >= 0 ? scheduler_slot_pid(c->bkl_slot) : 0;
    return 1;
}

static const struct query_provider cpustate_provider = {
    .cls = QUERY_CPUSTATE,
    .name = "cpustate",
    .record_size = sizeof(struct query_cpustate),
    .flags = QUERY_F_LIST,
    .count = cpustate_count,
    .fill = cpustate_fill,
    .fields = 0,
    .field_count = 0,
};

static void smp_query_init(void) { query_register(&cpustate_provider); }
INITCALL(smp_query_init, INIT_QUERY);

void smp_note_boot_tick(const uint64_t *regs) {
    struct cpu *c = gdt_boot_cpu();
    if (!c) return;
    c->tick_tsc = arch_rdtsc();
    c->tick_rip = regs ? regs[17] : 0;
    c->tick_cs = regs ? regs[18] : 0;
}

static const char *const STATE_NAMES[] = {
    "absent", "failed", "wedged", "stopped", "starting", "alive", "parked", "running",
};

// One line per core, the shape `/bin/cpus` prints (userland/utils/cpus.c).
void smp_report(void (*out)(const char *)) {
    char line[200];
    for (int i = 0; i < smp_cpu_count(); i++) {
        struct query_cpustate q;
        if (!cpustate_fill(i, &q)) continue;
        char tick[24];
        if (q.tick_age_ns == ~0ull) k_strlcpy(tick, "never", sizeof tick);
        else k_snprintf(tick, sizeof tick, "%lu ms r%lu", (unsigned long)(q.tick_age_ns / 1000000),
                        (unsigned long)(q.tick_cs & 3));
        k_snprintf(line, sizeof line,
                   "cpu %lu apic %lu %-8s pid %-5ld bkl %lu%s tick %s calls %lu%s flushes %lu%s",
                   (unsigned long)q.index, (unsigned long)q.apic_id,
                   q.state < 8 ? STATE_NAMES[q.state] : "?", (long)q.pid,
                   (unsigned long)q.bkl_depth, q.bkl_holder ? " (holds it)" : q.bkl_depth ? " (waits)" : "", tick,
                   (unsigned long)q.calls_taken, q.call_pending ? " (one pending)" : "",
                   (unsigned long)q.flushes_taken, q.flush_pending ? " (one pending)" : "");
        out(line);
        if (q.wedge_vec) {
            k_snprintf(line, sizeof line, "\r\n  wedged by vector %lu", (unsigned long)q.wedge_vec - 1);
            out(line);
        }
        if (q.nmi_stray) {
            k_snprintf(line, sizeof line, "\r\n  %lu stray NMI(s)", (unsigned long)q.nmi_stray);
            out(line);
        }
        out("\r\n");
    }
}
