// The kernel event trace's rings, writer, arming, queries and panic
// tail -- see ktrace.h for the contract, abi/ktrace_abi.h for the format.
#include "ktrace.h"
#include "ktrace_fmt.h"
#include "percpu.h"
#include "smp.h"
#include "spinlock.h"      // g_percpu_ready
#include "irqflags.h"
#include "random_hw.h"     // arch_rdtsc()
#include "cpuinfo.h"       // cpu_tsc_hz()
#include "scheduler.h"     // scheduler_current_pid()
#include "syscall_table.h" // a syscall's name, for the panic tail
#include "heap.h"
#include "pmm.h"           // pmm_total_frames(): the size limit
#include "query.h"
#include "initcall.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"

// A core's ring: `head` records written since the trace started, the
// newest at (head - 1) & mask. Written only by its core, read by anyone.
struct ktrace_ring {
    volatile uint64_t head;
    uint64_t mask;      // capacity - 1, a power of two
    struct ktrace_rec rec[];
};

volatile uint32_t g_ktrace_mask;

// Indexed by core; NULL for a core that was not online when they were
// made. Allocated, replaced and freed only under the kernel lock, with
// every core's own pointer moved first (install()).
static struct ktrace_ring **g_rings;
static int g_nrings;
static uint64_t g_ring_cap;                  // the rings' capacity now
static uint64_t g_want = KTRACE_DEFAULT_RECORDS;
static uint64_t g_armed_tsc, g_stopped_tsc;

void ktrace_write(uint32_t event, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t f = irq_save();
    // One %gs load with interrupts off: this core's ring, and it stays
    // this core's until the restore.
    struct ktrace_ring *r = g_percpu_ready ? this_cpu_read(ktrace) : 0;
    if (r) {
        uint64_t h = r->head;
        struct ktrace_rec *e = &r->rec[h & r->mask];
        e->tsc = arch_rdtsc();
        e->event = (uint16_t)event;
        e->cpu = (uint16_t)this_cpu_read(index);
        e->tid = scheduler_current_pid();
        e->a[0] = a0; e->a[1] = a1; e->a[2] = a2; e->a[3] = a3;
        // Published after the record: a reader never sees a head past
        // a record still being filled.
        __atomic_store_n(&r->head, h + 1, __ATOMIC_RELEASE);
    }
    irq_restore(f);
}

uint32_t ktrace_mask(void) { return g_ktrace_mask & KTRACE_G_ALL; }

// ---- arming ----------------------------------------------------------

// Runs ON each core (smp_call): a core writes its own struct cpu. A
// call is taken only where interrupts are on or in a spinner's poll,
// never inside ktrace_write(), so once every core has run it none can
// still be writing through the pointer it replaced -- the sync that
// makes freeing an old ring safe (RCU-sched's shape).
static void install(void *arg) {
    struct ktrace_ring **rings = arg;
    int me = (int)this_cpu_read(index);
    this_cpu_write(ktrace, rings && me < g_nrings ? rings[me] : 0);
}

static int online(const struct cpu *c) {
    return c && (c->state == CPU_PARKED || c->state == CPU_RUNNING);
}

// Every core runs install(rings). 1 when every online core did.
static int install_everywhere(struct ktrace_ring **rings) {
    uint64_t f = irq_save();
    install(rings);
    irq_restore(f);
    int others = 0;
    int me = (int)spin_cpu_index();
    for (int i = 0; i < smp_cpu_count(); i++)
        if (i != me && online(smp_cpu_at(i))) others++;
    int ran = others ? smp_call_others(install, rings) : 0;
    return ran >= others;
}

static void free_rings(void) {
    if (!g_rings) return;
    // NOT freed if a core did not answer: it may still hold a pointer.
    if (!install_everywhere(0)) {
        klog_write(KLOG_ERR "ktrace: a core did not answer -- its old ring is kept, not freed\n");
        g_rings = 0;
        g_nrings = 0;
        g_ring_cap = 0;
        return;
    }
    for (int i = 0; i < g_nrings; i++) kfree(g_rings[i]);
    kfree(g_rings);
    g_rings = 0;
    g_nrings = 0;
    g_ring_cap = 0;
}

static int alloc_rings(uint64_t cap) {
    int n = smp_cpu_count();
    struct ktrace_ring **rings = kzalloc((size_t)n * sizeof *rings);
    if (!rings) return 0;
    for (int i = 0; i < n; i++) {
        if (!online(smp_cpu_at(i))) continue;
        rings[i] = kmalloc(sizeof(struct ktrace_ring) + cap * sizeof(struct ktrace_rec));
        if (!rings[i]) {
            for (int k = 0; k < i; k++) kfree(rings[k]);
            kfree(rings);
            return 0;
        }
        rings[i]->head = 0;
        rings[i]->mask = cap - 1;
    }
    g_rings = rings;
    g_nrings = n;
    g_ring_cap = cap;
    return 1;
}

int ktrace_start(uint32_t mask) {
    if (!mask || !cpu_tsc_hz()) return 0;
    if (g_ktrace_mask) {   // running: change what it records, keep what it has
        __atomic_store_n(&g_ktrace_mask, mask, __ATOMIC_RELEASE);
        return 1;
    }
    if (g_rings && g_ring_cap != g_want) free_rings();
    if (!g_rings) {
        if (!alloc_rings(g_want)) {
            klog_printf(KLOG_ERR "ktrace: no memory for %d rings of %llu records\n",
                        smp_cpu_count(), (unsigned long long)g_want);
            return 0;
        }
    }
    // Every core's pointer (re)installed, which is also the sync: a
    // writer still finishing a record from the last trace is done.
    if (!install_everywhere(g_rings)) {
        klog_write(KLOG_ERR "ktrace: a core did not answer -- it records nothing\n");
    }
    for (int i = 0; i < g_nrings; i++)
        if (g_rings[i]) g_rings[i]->head = 0;
    g_armed_tsc = arch_rdtsc();
    g_stopped_tsc = 0;
    __atomic_store_n(&g_ktrace_mask, mask, __ATOMIC_RELEASE);
    return 1;
}

void ktrace_stop(void) {
    if (!g_ktrace_mask) return;
    __atomic_store_n(&g_ktrace_mask, 0, __ATOMIC_RELEASE);
    g_stopped_tsc = arch_rdtsc();
    // SIZE IT TO THE DATA's rule for a cap that must stay: say when it bit.
    for (int i = 0; i < g_nrings; i++) {
        struct ktrace_ring *r = g_rings[i];
        uint64_t h = r ? __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) : 0;
        if (r && h > g_ring_cap)
            klog_printf(KLOG_WARN "ktrace: core %d overwrote %llu of its %llu records -- "
                        "its ring holds %llu; kernel.trace_size raises it\n", i,
                        (unsigned long long)(h - g_ring_cap), (unsigned long long)h,
                        (unsigned long long)g_ring_cap);
    }
}

int ktrace_set_size(uint64_t records) {
    // BOUNDED BEFORE ANY ARITHMETIC: a huge value wrapped the product and
    // passed, and past 2^63 the doubling below reached 0 and never ended.
    uint64_t per_core = pmm_total_frames() * 4096 / 32 /
                        (sizeof(struct ktrace_rec) * (uint64_t)smp_cpu_count());
    if (records < 64 || records > per_core) return 0;
    uint64_t cap = 64;
    while (cap < records) cap <<= 1;
    uint64_t total = cap * sizeof(struct ktrace_rec) * (uint64_t)smp_cpu_count();
    if (total > pmm_total_frames() * 4096 / 32) return 0;
    g_want = cap;
    return 1;
}

uint64_t ktrace_size(void) { return g_want; }

// ---- reading ---------------------------------------------------------

static struct ktrace_ring *ring_at(int cpu) {
    return cpu >= 0 && cpu < g_nrings ? g_rings[cpu] : 0;
}

int ktrace_cpu_info(int cpu, struct query_ktrace_cpu *out) {
    struct ktrace_ring *r = ring_at(cpu);
    if (!r) return 0;
    k_memset(out, 0, sizeof *out);
    uint64_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    out->cpu = (uint32_t)cpu;
    out->mask = g_ktrace_mask;
    out->capacity = g_ring_cap;
    out->written = h;
    out->lost = h > g_ring_cap ? h - g_ring_cap : 0;
    out->tsc_hz = cpu_tsc_hz();
    out->armed_tsc = g_armed_tsc;
    out->stopped_tsc = g_stopped_tsc;
    return 1;
}

int ktrace_read(int cpu, uint64_t seq, struct ktrace_rec *out, int n, uint64_t *first) {
    struct ktrace_ring *r = ring_at(cpu);
    if (!r || n <= 0) return 0;
    uint64_t cap = g_ring_cap;
    uint64_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint64_t oldest = h > cap ? h - cap : 0;
    if (seq < oldest) seq = oldest;
    if (seq >= h) return 0;
    int got = (int)(h - seq < (uint64_t)n ? h - seq : (uint64_t)n);
    for (int i = 0; i < got; i++) out[i] = r->rec[(seq + i) & r->mask];
    // A live ring may have lapped the copy: the writer at `h2` may be
    // filling slot h2 & mask, which held record h2 - cap -- so only a
    // record newer than that is whole. Stopped, record h2 - cap is too.
    uint64_t h2 = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint64_t ahead = h2 + (g_ktrace_mask ? 1 : 0);   // the record being written, if live
    uint64_t whole_from = ahead > cap ? ahead - cap : 0;
    int skip = 0;
    while (skip < got && seq + (uint64_t)skip < whole_from) skip++;
    if (skip) {
        for (int i = skip; i < got; i++) out[i - skip] = out[i];
        got -= skip;
        seq += (uint64_t)skip;
    }
    if (first) *first = seq;
    return got;
}

// ---- the queries -----------------------------------------------------

static int cpu_count(void) {
    int n = 0;
    for (int i = 0; i < g_nrings; i++) n += g_rings[i] != 0;
    return n;
}

static int cpu_fill(int index, void *out) {
    for (int i = 0; i < g_nrings; i++) {
        if (!g_rings[i]) continue;
        if (index-- == 0) return ktrace_cpu_info(i, out);
    }
    return 0;
}

static uint64_t chunks_of(int cpu) {
    struct ktrace_ring *r = ring_at(cpu);
    if (!r) return 0;
    uint64_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint64_t avail = h < g_ring_cap ? h : g_ring_cap;
    return (avail + QUERY_KTRACE_PER - 1) / QUERY_KTRACE_PER;
}

static int rec_count(void) {
    uint64_t n = 0;
    for (int i = 0; i < g_nrings; i++) n += chunks_of(i);
    return n > 0x7FFFFFFF ? 0x7FFFFFFF : (int)n;
}

// Index `index` is a position at the moment of the read (query_abi.h):
// each core's chunks from its oldest record, the cores in order.
static int rec_fill(int index, void *out) {
    uint64_t want = (uint64_t)index;
    for (int i = 0; i < g_nrings; i++) {
        uint64_t c = chunks_of(i);
        if (want >= c) { want -= c; continue; }
        struct ktrace_ring *r = g_rings[i];
        uint64_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        uint64_t oldest = h > g_ring_cap ? h - g_ring_cap : 0;
        struct query_ktrace *q = out;
        k_memset(q, 0, sizeof *q);
        uint64_t first = 0;
        int n = ktrace_read(i, oldest + want * QUERY_KTRACE_PER, q->rec, QUERY_KTRACE_PER, &first);
        if (n <= 0) return 0;
        q->cpu = (uint32_t)i;
        q->n = (uint32_t)n;
        q->seq = first;
        return 1;
    }
    return 0;
}

static const struct query_provider ktrace_cpu_provider = {
    .cls = QUERY_KTRACE_CPU,
    .name = "ktrace_cpu",
    .record_size = sizeof(struct query_ktrace_cpu),
    .flags = QUERY_F_LIST,
    .count = cpu_count,
    .fill = cpu_fill,
};

static const struct query_provider ktrace_provider = {
    .cls = QUERY_KTRACE,
    .name = "ktrace",
    .record_size = sizeof(struct query_ktrace),
    .flags = QUERY_F_LIST,
    .count = rec_count,
    .fill = rec_fill,
};

static void ktrace_query_init(void) {
    query_register(&ktrace_cpu_provider);
    query_register(&ktrace_provider);
}
INITCALL(ktrace_query_init, INIT_QUERY);

// ---- the panic tail --------------------------------------------------

// What the tail may take of the panic record, which keeps the newest
// klog bytes in the rest: 12 of the store's 32 KiB, ~32 records on each
// of four cores.
#define TAIL_BUDGET 12288u
#define TAIL_AVG    90u    // a typical line, for how many fit
#define TAIL_LINE   120u   // a long one: a syscall's three hex arguments
#define TAIL_MAX    32u    // records per core, when the budget allows

static const char *panic_syscall_name(unsigned nr) {
    const struct syscall_desc *d = syscall_desc_at(nr);
    return d ? d->name : 0;
}

static unsigned tail_per_core(void) {
    unsigned cores = (unsigned)cpu_count();
    if (!cores) return 0;
    unsigned per = (TAIL_BUDGET / TAIL_AVG - 2) / cores - 1;   // a heading line each
    if (per > TAIL_MAX) per = TAIL_MAX;
    return per ? per : 1;
}

uint32_t ktrace_panic_reserve(void) {
    if (!g_ktrace_mask || !g_rings) return 0;
    return TAIL_BUDGET;
}

uint32_t ktrace_panic_tail(char *out, uint32_t cap) {
    uint32_t armed = g_ktrace_mask;
    if (!armed || !g_rings || !out || cap < TAIL_LINE) return 0;
    // Frozen: nothing this core does from here (the report's own spin
    // waits) pushes the cause out. The other cores are stopped.
    __atomic_store_n(&g_ktrace_mask, 0, __ATOMIC_RELEASE);
    struct ktrace_fmt_ctx ctx = { .t0_tsc = g_armed_tsc, .tsc_hz = cpu_tsc_hz(),
                                  .syscall_name = panic_syscall_name };
    unsigned per = tail_per_core();
    char groups[64];
    if (!ktrace_format_groups(armed, groups, sizeof groups)) k_strlcpy(groups, "?", sizeof groups);
    uint32_t len = 0;
    size_t n = k_snprintf(out, cap, "ktrace: the last %u events of each core, oldest first "
                          "(kernel.trace=%s; seconds since it started):\n", per, groups);
    if (n >= cap) return 0;
    len = (uint32_t)n;
    for (int i = 0; i < g_nrings; i++) {
        struct ktrace_ring *r = g_rings[i];
        if (!r) continue;
        uint64_t h = r->head;
        uint64_t from = h > per ? h - per : 0;
        uint64_t oldest = h > g_ring_cap ? h - g_ring_cap : 0;
        if (from < oldest) from = oldest;
        n = k_snprintf(out + len, cap - len, "ktrace: core %d, %llu of %llu records:\n", i,
                       (unsigned long long)(h - from), (unsigned long long)h);
        if (n >= cap - len) break;
        len += (uint32_t)n;
        for (uint64_t s = from; s < h; s++) {
            if (cap - len < TAIL_LINE) return len;
            out[len++] = ' ';
            out[len++] = ' ';
            int w = ktrace_fmt_rec(out + len, cap - len - 1, &r->rec[s & r->mask], &ctx);
            if (w <= 0) { len -= 2; continue; }
            len += (uint32_t)w;
            out[len++] = '\n';
        }
    }
    return len;
}
