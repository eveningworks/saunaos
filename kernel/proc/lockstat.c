// The counted spinlocks' registry and its two queries (lockstat.h):
// QUERY_LOCKSTAT, a row per lock, and QUERY_BKLSTAT, who takes the
// kernel lock -- each core, then each entry that has.
#include "lockstat.h"
#include "bkl.h"
#include "query.h"
#include "initcall.h"
#include "clocksource.h"
#include "cpuinfo.h"         // cpu_tsc_ns()
#include "smp.h"
#include "percpu.h"
#include "syscall_table.h"   // a syscall row's name
#include "string.h"
#include "kfmt.h"
#include "lapic.h"           // an MSI vector's handler
#include "ksyms.h"           // ...named
#include "tlb.h"             // TLB_FLUSH_VECTOR
#include "random_hw.h"       // arch_rdtsc(): the arming epoch

extern struct lockstat *const __lockstats_start[];
extern struct lockstat *const __lockstats_end[];

volatile int g_lockstat_timing;
volatile uint64_t g_lockstat_armed_tsc;
static uint64_t g_timing_since_ns;

int lockstat_timing_set(int on) {
    if (on && !cpu_tsc_hz()) return 0;
    if (!on) {
        g_lockstat_timing = 0;
        g_timing_since_ns = 0;
        return 1;
    }
    // The epoch first: a hold that began before it is never billed, so
    // a holder adding to a total while it is zeroed adds only to the old
    // stretch's, which nobody reads again. Then zero, then the flag.
    g_lockstat_armed_tsc = arch_rdtsc();
    for (int i = 0; i < lockstat_count(); i++) {
        __lockstats_start[i]->hold_tsc = 0;
        __lockstats_start[i]->hold_max_tsc = 0;
    }
    bkl_stat_zero_holds();
    g_timing_since_ns = clocksource_now_ns();
    g_lockstat_timing = 1;
    return 1;
}

uint64_t lockstat_timing_since_ns(void) { return g_lockstat_timing ? g_timing_since_ns : 0; }

int lockstat_count(void) { return (int)(__lockstats_end - __lockstats_start); }

const struct lockstat *lockstat_at(int i) {
    return i >= 0 && i < lockstat_count() ? __lockstats_start[i] : 0;
}

static int lock_fill(int index, void *out) {
    const struct lockstat *s = lockstat_at(index);
    if (!s) return 0;
    struct query_lockstat *q = out;
    k_memset(q, 0, sizeof *q);
    k_strlcpy(q->name, s->name, sizeof q->name);
    q->acquired = s->acquired;
    q->contended = s->contended;
    q->wait_ns = cpu_tsc_ns(s->wait_tsc);
    q->wait_max_ns = cpu_tsc_ns(s->wait_max_tsc);
    q->hold_ns = cpu_tsc_ns(s->hold_tsc);
    q->hold_max_ns = cpu_tsc_ns(s->hold_max_tsc);
    q->now_ns = clocksource_now_ns();
    q->timed_since_ns = lockstat_timing_since_ns();
    return 1;
}

static const struct query_provider lockstat_provider = {
    .cls = QUERY_LOCKSTAT,
    .name = "lockstat",
    .record_size = sizeof(struct query_lockstat),
    .flags = QUERY_F_LIST,
    .count = lockstat_count,
    .fill = lock_fill,
};

// The cores first, then every key something has taken it with. A key
// first used between count() and fill() shifts the rows after it, which
// a snapshot accepts.
static int entries_used(void) {
    struct bkl_entry_stat e;
    int n = 0;
    for (uint32_t k = 0; k < BKL_KEYS; k++) n += bkl_entry_stat(k, &e);
    return n;
}

static int bkl_count(void) { return smp_cpu_count() + entries_used(); }

// What a reader calls the vector: a fixed one by its role, a legacy
// line by its number, an allocated one by its handler's symbol.
static void vector_name(uint32_t v, char *out, size_t cap) {
    void (*h)(uint64_t *) = 0;
    if (v == 14) k_strlcpy(out, "page fault", cap);
    else if (v == 128) k_strlcpy(out, "syscall (other)", cap);
    else if (v < 32) k_snprintf(out, cap, "exception %u", v);
    else if (v < LAPIC_VECTOR_BASE) k_snprintf(out, cap, "irq %u", v - 32);
    else if (v == SMP_CALL_VECTOR) k_strlcpy(out, "call IPI", cap);
    else if (v == SMP_SCHED_VECTOR) k_strlcpy(out, "core tick", cap);
    else if (v == TLB_FLUSH_VECTOR) k_strlcpy(out, "shootdown IPI", cap);
    else if ((h = lapic_vector_handler((uint8_t)v)) != 0) {
        uint32_t off;
        const char *sym = ksyms_lookup((uint64_t)(uintptr_t)h, &off);
        if (sym) k_strlcpy(out, sym, cap);
        else k_snprintf(out, cap, "vector %u", v);
    } else k_snprintf(out, cap, "vector %u", v);
}

static void fill_entry(struct query_bklstat *q, uint32_t key, const struct bkl_entry_stat *e) {
    if (key == BKL_KEY_KERNEL) {
        q->kind = QUERY_BKLSTAT_KERNEL;
    } else if (key >= BKL_KEY_SYSCALL(0)) {
        q->kind = QUERY_BKLSTAT_SYSCALL;
        q->id = key - BKL_KEY_SYSCALL(0);
        const struct syscall_desc *d = syscall_desc_at(q->id);
        if (d && d->name) k_strlcpy(q->name, d->name, sizeof q->name);
    } else {
        q->kind = QUERY_BKLSTAT_VECTOR;
        q->id = key;
        vector_name(key, q->name, sizeof q->name);
    }
    q->takes = e->takes;
    q->contended = e->contended;
    q->wait_ns = cpu_tsc_ns(e->wait_tsc);
    q->wait_max_ns = cpu_tsc_ns(e->wait_max_tsc);
    q->hold_ns = cpu_tsc_ns(e->hold_tsc);
    q->hold_max_ns = cpu_tsc_ns(e->hold_max_tsc);
}

static int bkl_fill(int index, void *out) {
    struct query_bklstat *q = out;
    k_memset(q, 0, sizeof *q);
    int cores = smp_cpu_count();
    if (index < cores) {
        const struct cpu *c = smp_cpu_at(index);
        if (!c) return 0;
        q->kind = QUERY_BKLSTAT_CORE;
        q->id = c->index;
        q->takes = c->bkl_takes;
        q->contended = c->bkl_contended;
        q->wait_ns = cpu_tsc_ns(c->bkl_wait_tsc);
        q->hold_ns = cpu_tsc_ns(c->bkl_hold_tsc);
        return 1;
    }
    int want = index - cores;
    struct bkl_entry_stat e;
    for (uint32_t k = 0; k < BKL_KEYS; k++) {
        if (!bkl_entry_stat(k, &e)) continue;
        if (want-- == 0) {
            fill_entry(q, k, &e);
            return 1;
        }
    }
    return 0;
}

static const struct query_provider bklstat_provider = {
    .cls = QUERY_BKLSTAT,
    .name = "bklstat",
    .record_size = sizeof(struct query_bklstat),
    .flags = QUERY_F_LIST,
    .count = bkl_count,
    .fill = bkl_fill,
};

static void lockstat_query_init(void) {
    query_register(&lockstat_provider);
    query_register(&bklstat_provider);
}
INITCALL(lockstat_query_init, INIT_QUERY);
