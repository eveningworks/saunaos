// How the machine's time was spent, as a queryable FACT -- the whole
// machine (QUERY_CPULOAD) and each core (QUERY_CORELOAD).
//
// Beside the scheduler because the scheduler is what knows: the split
// comes straight out of bill_current(), so this cannot drift from what
// `ps` and Task Manager report per process.
//
// WHY A FACT AND NOT A PERCENTAGE. The counters are cumulative since
// boot, and a caller divides two samples' deltas. A kernel that reported
// a percentage would have to choose the window, and every caller wants a
// different one -- Task Manager refreshes twice a second, a status line
// might want ten seconds. Linux's /proc/stat makes the same call, and
// its per-`cpuN` lines are QUERY_CORELOAD's shape.
#include "query.h"
#include "scheduler.h"
#include "clocksource.h"
#include "smp.h"
#include "percpu.h"   // struct cpu's busy_ns
#include "string.h"
#include <stddef.h>
#include "initcall.h"

static int cpuload_count(void) { return 1; }

static int cpuload_fill(int index, void *out) {
    if (index != 0) return 0; // scalar
    struct query_cpuload *c = out;
    k_memset(c, 0, sizeof *c);
    scheduler_cpu_time(&c->proc_ns, &c->kernel_ns);
    c->now_ns = clocksource_now_ns();
    return 1;
}

static const struct query_field cpuload_fields[] = {
    QUERY_FIELD(struct query_cpuload, proc_ns,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cpuload, kernel_ns, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cpuload, now_ns,    QUERY_TYPE_U64),
};

static const struct query_provider cpuload_provider = {
    .cls = QUERY_CPULOAD,
    .name = "cpu",
    .record_size = sizeof(struct query_cpuload),
    .flags = 0, // scalar
    .count = cpuload_count,
    .fill = cpuload_fill,
    .fields = cpuload_fields,
    .field_count = sizeof cpuload_fields / sizeof cpuload_fields[0],
};

// --- per core -----------------------------------------------------------

// The index'th ONLINE core, or NULL. A core that never started has no
// busy time to report, and listing it would make "how many cores share
// the load" a question every reader has to answer again.
static const struct cpu *online_at(int index) {
    for (int i = 0, seen = 0; i < smp_cpu_count(); i++) {
        const struct cpu *c = smp_cpu_at(i);
        if (!c || !smp_apic_online(c->apic_id)) continue;
        if (seen++ == index) return c;
    }
    return 0;
}

static int coreload_count(void) { return smp_online_count(); }

static int coreload_fill(int index, void *out) {
    const struct cpu *c = online_at(index);
    if (!c) return 0;
    struct query_coreload *q = out;
    k_memset(q, 0, sizeof *q);
    // The caller's own slice, billed up to now -- cpuload_fill()'s reason.
    scheduler_cpu_time(0, 0);
    q->index   = c->index;
    q->apic_id = c->apic_id;
    q->flags   = QUERY_CPU_ONLINE |
                 (smp_apic_scheduling(c->apic_id) ? QUERY_CPU_SCHEDULING : 0);
    // ONE 64-BIT LOAD, never torn: the owning core is the only writer
    // (bill_current() stores it whole), and an aligned 8-byte access is
    // atomic on x86-64. The atomic builtin is what stops the compiler
    // splitting or repeating it.
    q->busy_ns = __atomic_load_n(&c->busy_ns, __ATOMIC_RELAXED);
    q->now_ns  = clocksource_now_ns();
    return 1;
}

static const struct query_provider coreload_provider = {
    .cls = QUERY_CORELOAD,
    .name = "coreload",
    .record_size = sizeof(struct query_coreload),
    .flags = QUERY_F_LIST,
    .count = coreload_count,
    .fill = coreload_fill,
    .fields = 0,
    .field_count = 0,
};

void cpuload_query_init(void) {
    query_register(&cpuload_provider);
    query_register(&coreload_provider);
}
INITCALL(cpuload_query_init, INIT_QUERY);
