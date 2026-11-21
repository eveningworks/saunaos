// Lock-order checking (lockdep.h, docs/lockdep-design.md). Compiled into
// every kernel; everything but the stubs the KTESTs need is behind
// `option lockdep` (SAUNAOS_LOCKDEP).
//
// THE GRAPH IS FIXED-SIZE ON PURPOSE: it is written from inside
// spin_lock() -- under the heap's own lock, in interrupts -- where
// nothing may allocate. Linux's lockdep has static tables for the same
// reason (MAX_LOCKDEP_KEYS, MAX_LOCKDEP_ENTRIES). A full table LOGS once
// and the check carries on without what did not fit.
//
// **ONE GRAPH LOCK, A BARE TEST-AND-SET WITH INTERRUPTS OFF**, never a
// spinlock of its own (that would check itself), taken only on a slow
// path: a class first seen, an edge first seen, a report. Its holder
// waits on nothing and writes no log. The fast path -- every edge from
// the locks held already known -- reads the bitmaps unlocked: a bit is
// only ever set (lockdep_forget() aside, for a KTEST's classes).
#include "lockdep.h"

#ifdef SAUNAOS_LOCKDEP
#include "spinlock.h"
#include "kmutex.h"
#include "lockstat.h"
#include "percpu.h"
#include "gdt.h"          // gdt_boot_cpu(): before GS.base is set
#include "irqflags.h"
#include "barrier.h"      // cpu_relax()
#include "smp.h"          // smp_poll_ipis(): a spinner answers a call
#include "bkl.h"          // bkl_held_here(): who may write the log
#include "klog.h"
#include "kfmt.h"
#include "ksyms.h"
#include "reloc.h"        // kernel_reloc_delta(): a static lock's link address
#include "idt.h"          // idt_log_stack_scan()
#include "string.h"
#include "query.h"
#include "query_abi.h"
#include "initcall.h"
#include "sched_internal.h"   // procs[]: a context's held kmutexes

#define LD_MAX 256              // classes, subclass nodes included; id 0 is "none"
#define LD_W (LD_MAX / 64)
#define LD_MAX_EDGES 2048       // edges remembered with their sites
#define LD_MAX_REPORTS 64
#define LD_UNNAMED_SITES 32
#define LD_NAME 24

#define U_IRQ 1   // taken in a hardware interrupt (Linux's IN-HARDIRQ)
#define U_ON  2   // taken with interrupts on, outside one (HARDIRQ-ON)

struct ld_class {
    const char *name;        // given, a counted lock's, or 0 for a static's own
    char auto_name[LD_NAME]; // a static lock's: "@" + the function first taking it
    uintptr_t key;           // a static lock's address; 0 for a named class
    uint8_t is_kmutex, sub, usage, pad;
    uint16_t reported;       // 1 << kind, for the kinds reported once per class
    uint16_t base;           // the subclass-0 class (itself for one)
    uint16_t subs[LOCKDEP_SUBCLASSES];
    uintptr_t first_site, irq_site, on_site;
};

struct ld_edge { uint16_t from, to; uintptr_t held_site, take_site; };

static struct ld_class g_cls[LD_MAX];
static int g_ncls;
static uint64_t g_adj[LD_MAX][LD_W];      // a dependency: `from` held while `to` was taken
static uint64_t g_seen[LD_MAX][LD_W];     // ...or one reported as closing a cycle
static uint64_t g_reported[LD_MAX][LD_W]; // a pair reported already, any kind
static struct ld_edge g_edges[LD_MAX_EDGES];
static int g_nedges, g_edges_total;
static struct lockdep_report g_rep[LD_MAX_REPORTS];
static volatile int g_nrep;
static unsigned g_rep_lost, g_unmatched, g_max_spin, g_max_task;
static uintptr_t g_unnamed[LD_UNNAMED_SITES];
static volatile int g_pending;   // a report not yet written to the log
static int g_full_logged;

extern char __kimage_start[], _kernel_end[];

// Interrupts off for a hook, and back -- without a popf when they were
// off already, which is every irqsave take: a popf costs more than the
// rest of a hook's fast path.
static inline uint64_t ld_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0" : "=r"(f) :: "memory");
    if (f & (1u << 9)) __asm__ volatile ("cli" ::: "memory");
    return f;
}
static inline void ld_restore(uint64_t f) {
    if (f & (1u << 9)) __asm__ volatile ("sti" ::: "memory");
}

static volatile int g_glock;

static uint64_t glock(void) {
    uint64_t f = ld_save();
    while (__atomic_exchange_n(&g_glock, 1, __ATOMIC_ACQUIRE)) {
        smp_poll_ipis();
        cpu_relax();
    }
    return f;
}

static void gunlock(uint64_t f) {
    __atomic_store_n(&g_glock, 0, __ATOMIC_RELEASE);
    ld_restore(f);
}

// Interrupts off: the pointer is this core's until they come back on.
static struct cpu *here(void) { return g_percpu_ready ? this_cpu_raw() : gdt_boot_cpu(); }

// THE RUNNING CONTEXT'S, WITH INTERRUPTS ON OR OFF: its slot is one %gs
// load (sched_internal.h's rule), and a slot -1 context -- the boot
// core's kernel context, an AP's idle loop -- never changes cores.
static struct lockdep_task *cur_task(void) {
    int s = SCHED_HERE_GET(sched_cur);
    if (s >= 0 && procs && s < g_max_procs) return &procs[s].ld;
    return &here()->ld.kernel_task;
}

static int bit(uint64_t (*m)[LD_W], int a, int b) {
    return (int)((__atomic_load_n(&m[a][b >> 6], __ATOMIC_RELAXED) >> (b & 63)) & 1);
}
static void setbit(uint64_t (*m)[LD_W], int a, int b) {
    __atomic_or_fetch(&m[a][b >> 6], 1ull << (b & 63), __ATOMIC_RELAXED);
}
static void clearbit(uint64_t (*m)[LD_W], int a, int b) {
    __atomic_and_fetch(&m[a][b >> 6], ~(1ull << (b & 63)), __ATOMIC_RELAXED);
}

static int in_image(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= (uintptr_t)__kimage_start && a < (uintptr_t)_kernel_end;
}

// ---- classes ----------------------------------------------------------

// Under the graph lock.
static int class_new(const char *name, uintptr_t key, int kmutex, uintptr_t site) {
    if (g_ncls + 1 >= LD_MAX) {
        if (!g_full_logged) { g_full_logged = 1; g_pending = 1; }
        return 0;
    }
    int id = g_ncls + 1;
    struct ld_class *k = &g_cls[id];
    k->name = name;
    k->key = key;
    k->is_kmutex = (uint8_t)kmutex;
    k->base = (uint16_t)id;
    k->first_site = site;
    if (!name) {
        uint32_t off;
        const char *fn = ksyms_lookup(site, &off);
        k_snprintf(k->auto_name, sizeof k->auto_name, "@%s", fn ? fn : "?");
    }
    __atomic_store_n(&g_ncls, id, __ATOMIC_RELEASE);
    return id;
}

static int class_find(const char *name, uintptr_t key, int kmutex) {
    for (int i = 1; i <= g_ncls; i++) {
        const struct ld_class *k = &g_cls[i];
        if (k->sub || k->is_kmutex != kmutex) continue;
        if (key ? k->key == key : (!k->key && k->name && (k->name == name || k_strcmp(k->name, name) == 0)))
            return i;
    }
    return 0;
}

static int class_get(const char *name, uintptr_t key, int kmutex, uintptr_t site) {
    uint64_t f = glock();
    int id = class_find(name, key, kmutex);
    if (!id) id = class_new(name, key, kmutex, site);
    gunlock(f);
    return id;
}

// A subclass is its own graph node: an edge between two of them is an
// order, where two of one node held together is a recursion.
static int node_of(int base, int sub) {
    if (sub <= 0 || !base) return base;
    if (sub >= LOCKDEP_SUBCLASSES) sub = LOCKDEP_SUBCLASSES - 1;
    int id = __atomic_load_n(&g_cls[base].subs[sub], __ATOMIC_ACQUIRE);
    if (id) return id;
    uint64_t f = glock();
    id = g_cls[base].subs[sub];
    if (!id) {
        id = class_new(g_cls[base].name, 0, g_cls[base].is_kmutex, g_cls[base].first_site);
        if (id) {
            g_cls[id].sub = (uint8_t)sub;
            g_cls[id].base = (uint16_t)base;
            k_memcpy(g_cls[id].auto_name, g_cls[base].auto_name, sizeof g_cls[id].auto_name);
            __atomic_store_n(&g_cls[base].subs[sub], (uint16_t)id, __ATOMIC_RELEASE);
        }
    }
    gunlock(f);
    return id ? id : base;
}

const char *lockdep_class_name(int cls, char *buf, int cap) {
    if (cls <= 0 || cls > g_ncls) { k_strlcpy(buf, "?", (size_t)cap); return buf; }
    const struct ld_class *k = &g_cls[cls];
    const char *n = k->name ? k->name : k->auto_name;
    if (k->sub) k_snprintf(buf, (size_t)cap, "%s/%u", n, (unsigned)k->sub);
    else k_strlcpy(buf, n, (size_t)cap);
    return buf;
}

// An unnamed lock outside the image: reported once per site, never checked.
static void note_unnamed(uintptr_t site);

static int spin_class(struct spinlock *l, uintptr_t site) {
    uint32_t c = l->ld_class;
    if (c & LOCKDEP_KMUTEX_INNER) return 0;   // a kmutex's own wait_lock
    if (c == LOCKDEP_UNNAMED_CLASS) return 0;
    if (c) return (int)c;
    int id;
    if (in_image(l)) {
        id = class_get(l->stat ? l->stat->name : 0, (uintptr_t)l, 0, site);
    } else {
        note_unnamed(site);
        id = LOCKDEP_UNNAMED_CLASS;
    }
    if (id) l->ld_class = (uint32_t)id;   // two first takers agree: one class per key
    return id == (int)LOCKDEP_UNNAMED_CLASS ? 0 : id;
}

static int kmutex_class(struct kmutex *m, uintptr_t site) {
    uint32_t c = m->wait_lock.ld_class & ~LOCKDEP_KMUTEX_INNER;
    if (c == LOCKDEP_UNNAMED_CLASS) return 0;
    if (c) return (int)c;
    int id;
    if (in_image(m)) {
        id = class_get(0, (uintptr_t)m, 1, site);
    } else {
        note_unnamed(site);
        id = LOCKDEP_UNNAMED_CLASS;
    }
    if (id) m->wait_lock.ld_class = LOCKDEP_KMUTEX_INNER | (uint32_t)id;
    return id == (int)LOCKDEP_UNNAMED_CLASS ? 0 : id;
}

void lockdep_name_spin(struct spinlock *l, const char *name) {
    uint32_t c = l->ld_class;
    if (c && c < LD_MAX && g_cls[c].name == name) return;
    int id = class_get(name, 0, 0, (uintptr_t)__builtin_return_address(0));
    if (id) l->ld_class = (uint32_t)id;
}

void lockdep_name_kmutex(struct kmutex *m, const char *name) {
    uint32_t c = m->wait_lock.ld_class & ~LOCKDEP_KMUTEX_INNER;
    if (c && c < LD_MAX && g_cls[c].name == name) return;   // the common case: mount_lock()'s
    int id = class_get(name, 0, 1, (uintptr_t)__builtin_return_address(0));
    m->wait_lock.ld_class = LOCKDEP_KMUTEX_INNER | (uint32_t)id;
}

void lockdep_kmutex_prepare(struct kmutex *m) {
    if (__atomic_load_n(&m->wait_lock.ld_class, __ATOMIC_RELAXED)) return;
    uint32_t zero = 0;
    __atomic_compare_exchange_n(&m->wait_lock.ld_class, &zero, LOCKDEP_KMUTEX_INNER, 0,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

// ---- reports ----------------------------------------------------------

// Under the graph lock. A pair is reported once whatever the kind; a
// class-only kind once per class.
// `path` is `nodes` graph nodes, an inversion's old path: `edges` long,
// cut to LOCKDEP_PATH_MAX nodes.
static void report_add(int kind, int a, int b, uintptr_t s0, uintptr_t s1,
                       uintptr_t s2, uintptr_t s3, const uint16_t *path, int edges) {
    if (g_nrep >= LD_MAX_REPORTS) { g_rep_lost++; return; }
    struct lockdep_report *r = &g_rep[g_nrep];
    k_memset(r, 0, sizeof *r);
    r->kind = (uint16_t)kind;
    r->a = (uint16_t)a;
    r->b = (uint16_t)b;
    r->site[0] = s0; r->site[1] = s1; r->site[2] = s2; r->site[3] = s3;
    r->path_len = (uint16_t)edges;
    for (int i = 0; path && i <= edges && i < LOCKDEP_PATH_MAX; i++) r->path[i] = path[i];
    __atomic_store_n(&g_nrep, g_nrep + 1, __ATOMIC_RELEASE);
    g_pending = 1;
}

static void pair_report(int kind, int a, int b, uintptr_t s0, uintptr_t s1) {
    if (bit(g_reported, a, b)) return;
    uint64_t f = glock();
    if (!bit(g_reported, a, b)) {
        setbit(g_reported, a, b);
        report_add(kind, a, b, s0, s1, 0, 0, 0, 0);
    }
    gunlock(f);
}

static void class_report(int kind, int a, uintptr_t s0, uintptr_t s1) {
    int base = g_cls[a].base;
    if (g_cls[base].reported & (1u << kind)) return;
    uint64_t f = glock();
    if (!(g_cls[base].reported & (1u << kind))) {
        g_cls[base].reported |= (uint16_t)(1u << kind);
        report_add(kind, a, 0, s0, s1, 0, 0, 0, 0);
    }
    gunlock(f);
}

static void note_unnamed(uintptr_t site) {
    uint64_t f = glock();
    for (int i = 0; i < LD_UNNAMED_SITES; i++) {
        if (g_unnamed[i] == site) break;
        if (!g_unnamed[i]) {
            g_unnamed[i] = site;
            report_add(LOCKDEP_UNNAMED, 0, 0, 0, site, 0, 0, 0, 0);
            break;
        }
    }
    gunlock(f);
}

static const struct ld_edge *edge_find(int from, int to) {
    for (int i = 0; i < g_nedges; i++)
        if (g_edges[i].from == from && g_edges[i].to == to) return &g_edges[i];
    return 0;
}

static const char *site_str(uintptr_t site, char *buf, size_t cap) {
    if (!site) { k_strlcpy(buf, "?", cap); return buf; }
    uint32_t off;
    const char *fn = ksyms_lookup(site, &off);
    if (fn) k_snprintf(buf, cap, "%s+0x%x", fn, off);
    else k_snprintf(buf, cap, "%p", (void *)site);
    return buf;
}

static void print_report(const struct lockdep_report *r) {
    char a[40], b[40], s0[64], s1[64], s2[64], s3[64];
    lockdep_class_name(r->a, a, sizeof a);
    lockdep_class_name(r->b, b, sizeof b);
    site_str(r->site[0], s0, sizeof s0);
    site_str(r->site[1], s1, sizeof s1);
    switch (r->kind) {
    case LOCKDEP_INVERSION:
        klog_printf(KLOG_ERR "lockdep: lock order inversion: \"%s\" taken at %s, holding \"%s\" "
                    "(taken at %s)\n", b, s1, a, s0);
        for (int i = 0; i < r->path_len && i + 1 < LOCKDEP_PATH_MAX; i++) {
            int u = r->path[i], v = r->path[i + 1];
            if (!u || !v) break;
            const struct ld_edge *e = edge_find(u, v);
            char x[40], y[40];
            klog_printf(KLOG_ERR "lockdep:   before: \"%s\" taken at %s, holding \"%s\" (taken at %s)\n",
                        lockdep_class_name(v, y, sizeof y), site_str(e ? e->take_site : 0, s3, sizeof s3),
                        lockdep_class_name(u, x, sizeof x), site_str(e ? e->held_site : 0, s2, sizeof s2));
        }
        break;
    case LOCKDEP_RECURSION:
        klog_printf(KLOG_ERR "lockdep: two \"%s\" locks held: the second taken at %s, the first at %s "
                    "-- a fixed order needs kmutex_lock_nested()\n", a, s1, s0);
        break;
    case LOCKDEP_IRQ_INCONSISTENT:
        klog_printf(KLOG_ERR "lockdep: \"%s\" is taken in an interrupt (at %s) and with interrupts on "
                    "(at %s) -- the interrupt can spin on its own core's holder\n", a, s0, s1);
        break;
    case LOCKDEP_IRQ_UNSAFE_ORDER:
        klog_printf(KLOG_ERR "lockdep: irq-safe \"%s\" (held from %s) is held while taking irq-unsafe "
                    "\"%s\" at %s\n", a, s0, b, s1);
        break;
    case LOCKDEP_SLEEP_IN_SPIN:
        klog_printf(KLOG_ERR "lockdep: kmutex \"%s\" taken at %s holding spinlock \"%s\" (taken at %s) "
                    "-- a sleep under a spinlock\n", b, s1, a, s0);
        break;
    case LOCKDEP_KMUTEX_IN_IRQ:
        klog_printf(KLOG_ERR "lockdep: kmutex \"%s\" taken in a hardware interrupt at %s\n", a, s1);
        break;
    case LOCKDEP_BKL_IN_SPIN:
        klog_printf(KLOG_ERR "lockdep: the kernel lock taken at %s holding spinlock \"%s\" (taken at %s)\n",
                    s1, a, s0);
        break;
    case LOCKDEP_HELD_TO_USER:
        klog_printf(KLOG_ERR "lockdep: back to ring 3 holding kmutex \"%s\" (taken at %s)\n", a, s1);
        break;
    case LOCKDEP_UNNAMED:
        klog_printf(KLOG_ERR "lockdep: an unnamed lock outside the kernel image taken at %s is not "
                    "checked -- name it where it is set up (kmutex_name(), spin_lock_name())\n", s1);
        break;
    default:
        break;
    }
}

// THE LOG IS WRITTEN BY THE KERNEL LOCK'S HOLDER ONLY: a report made
// without it (an AP's call function) waits for the next lockdep event
// on a core that has it, or for the query.
static void flush(int with_stack) {
    if (!g_pending || !bkl_held_here()) return;
    g_pending = 0;
    if (g_full_logged == 1) {
        g_full_logged = 2;
        klog_printf(KLOG_ERR "lockdep: the class table is full (%d) -- later classes are not checked\n",
                    LD_MAX - 1);
    }
    int n = __atomic_load_n(&g_nrep, __ATOMIC_ACQUIRE), printed = 0;
    for (int i = 0; i < n; i++) {
        if (g_rep[i].printed) continue;
        g_rep[i].printed = 1;
        print_report(&g_rep[i]);
        printed++;
    }
    if (printed && with_stack) {
        uint64_t rsp;
        __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
        idt_log_stack_scan(rsp, 10);
    }
}

// ---- the graph ----------------------------------------------------------

static uint16_t g_parent[LD_MAX], g_queue[LD_MAX];

// Under the graph lock: does `src` reach `dst`? Fills `path` with the
// nodes src .. dst (at most `max`) and returns how many edges it has.
static int path_find(int src, int dst, uint16_t *path, int max) {
    uint64_t vis[LD_W] = {0};
    int head = 0, tail = 0;
    g_queue[tail++] = (uint16_t)src;
    vis[src >> 6] |= 1ull << (src & 63);
    while (head < tail) {
        int u = g_queue[head++];
        if (u == dst) break;
        for (int w = 0; w < LD_W; w++) {
            uint64_t bits = g_adj[u][w] & ~vis[w];
            while (bits) {
                int v = w * 64 + __builtin_ctzll(bits);
                bits &= bits - 1;
                vis[v >> 6] |= 1ull << (v & 63);
                g_parent[v] = (uint16_t)u;
                g_queue[tail++] = (uint16_t)v;
            }
        }
    }
    if (!((vis[dst >> 6] >> (dst & 63)) & 1)) return 0;
    static uint16_t rev[LD_MAX];   // under the graph lock, like the queue
    int n = 0;
    for (int v = dst; ; v = g_parent[v]) {
        rev[n++] = (uint16_t)v;
        if (v == src || n == LD_MAX) break;
    }
    for (int i = 0; i < n && i < max; i++) path[i] = rev[n - 1 - i];
    return n - 1;
}

static int usage_of(int node) { return g_cls[g_cls[node].base].usage; }

// `from` held while `to` is taken, never seen before.
static void edge_add(int from, int to, uintptr_t held_site, uintptr_t take_site) {
    uint64_t f = glock();
    if (bit(g_seen, from, to)) { gunlock(f); return; }
    uint16_t path[LOCKDEP_PATH_MAX];
    int len = path_find(to, from, path, LOCKDEP_PATH_MAX);
    setbit(g_seen, from, to);
    if (len) {
        // Not added: the graph stays acyclic, so every later cycle is new.
        const struct ld_edge *e = edge_find(path[0], path[1]);
        if (!bit(g_reported, from, to)) {
            setbit(g_reported, from, to);
            report_add(LOCKDEP_INVERSION, from, to, held_site, take_site,
                       e ? e->held_site : 0, e ? e->take_site : 0, path, len);
        }
    } else {
        setbit(g_adj, from, to);
        g_edges_total++;
        if (g_nedges < LD_MAX_EDGES)
            g_edges[g_nedges++] = (struct ld_edge){ (uint16_t)from, (uint16_t)to, held_site, take_site };
        if ((usage_of(from) & U_IRQ) && (usage_of(to) & U_ON) && !bit(g_reported, from, to)) {
            setbit(g_reported, from, to);
            report_add(LOCKDEP_IRQ_UNSAFE_ORDER, from, to, held_site, take_site, 0, 0, 0, 0);
        }
    }
    gunlock(f);
}

static void check_order(const struct lockdep_held *h, int n, int node, const void *lock,
                        uintptr_t site) {
    for (int i = 0; i < n; i++) {
        if (!h[i].count) continue;
        if (h[i].cls == node) {
            if (h[i].lock != lock) pair_report(LOCKDEP_RECURSION, node, node, h[i].site, site);
            continue;
        }
        if (!bit(g_seen, h[i].cls, node)) edge_add(h[i].cls, node, h[i].site, site);
    }
}

// A spinlock class's first interrupt use and first use with interrupts
// on; both is a report, and so is an edge from irq-safe to irq-unsafe.
static void mark_usage(int cls, int in_irq, int irqs_on, uintptr_t site) {
    int want = in_irq ? U_IRQ : irqs_on ? U_ON : 0;
    struct ld_class *k = &g_cls[g_cls[cls].base];
    if (!want || (k->usage & want)) return;
    uint64_t f = glock();
    if (!(k->usage & want)) {
        k->usage |= (uint8_t)want;
        if (want == U_IRQ) k->irq_site = site; else k->on_site = site;
        int base = g_cls[cls].base;
        if (k->usage == (U_IRQ | U_ON) && !(k->reported & (1u << LOCKDEP_IRQ_INCONSISTENT))) {
            k->reported |= (uint16_t)(1u << LOCKDEP_IRQ_INCONSISTENT);
            report_add(LOCKDEP_IRQ_INCONSISTENT, base, 0, k->irq_site, k->on_site, 0, 0, 0, 0);
        }
        for (int o = 1; o <= g_ncls; o++) {
            int from = want == U_IRQ ? cls : o, to = want == U_IRQ ? o : cls;
            int other = want == U_IRQ ? U_ON : U_IRQ;
            if (o == cls || !bit(g_adj, from, to) || !(usage_of(o) & other)) continue;
            if (bit(g_reported, from, to)) continue;
            setbit(g_reported, from, to);
            const struct ld_edge *e = edge_find(from, to);
            report_add(LOCKDEP_IRQ_UNSAFE_ORDER, from, to, e ? e->held_site : 0,
                       e ? e->take_site : 0, 0, 0, 0, 0);
        }
    }
    gunlock(f);
}

static void push(struct lockdep_held *h, int *n, int cap, int node, const void *lock,
                 const void *nest, uintptr_t site, unsigned *max) {
    if (*n >= cap) { g_unmatched++; return; }   // too deep: its release will not match either
    h[*n] = (struct lockdep_held){ .cls = (uint16_t)node, .count = 1, .lock = lock,
                                   .nest = nest, .site = site };
    (*n)++;
    if ((unsigned)*n > *max) *max = (unsigned)*n;
}

static void pop(struct lockdep_held *h, int *n, const void *lock, int base) {
    int j = -1;
    for (int i = *n - 1; i >= 0; i--) if (h[i].lock == lock) { j = i; break; }
    // One of several under a nest lock: they share an entry.
    if (j < 0)
        for (int i = *n - 1; i >= 0; i--)
            if (h[i].nest && h[i].cls == base && h[i].count > 1) { j = i; break; }
    if (j < 0) { g_unmatched++; return; }
    if (--h[j].count > 0) {
        if (h[j].lock == lock) h[j].lock = (const void *)1;   // the entry now stands for the rest
        return;
    }
    for (int i = j; i + 1 < *n; i++) h[i] = h[i + 1];
    (*n)--;
}

// ---- the hooks --------------------------------------------------------

void lockdep_spin_acquired(struct spinlock *l, uintptr_t site, int trylock) {
    uint64_t f = ld_save();
    struct cpu *c = here();
    if (c->ld.busy) { ld_restore(f); return; }
    c->ld.busy = 1;
    int cls = spin_class(l, site);
    if (cls) {
        c->ld.checked++;
        mark_usage(cls, cur_task()->hardirq > 0, (f >> 9) & 1, site);
        if (!trylock) check_order(c->ld.h, c->ld.n, cls, l, site);
        push(c->ld.h, &c->ld.n, LOCKDEP_CPU_HELD, cls, l, 0, site, &g_max_spin);
    }
    flush(1);
    c->ld.busy = 0;
    ld_restore(f);
}

void lockdep_spin_release(struct spinlock *l) {
    uint32_t cls = l->ld_class;
    if (!cls || (cls & LOCKDEP_KMUTEX_INNER) || cls == LOCKDEP_UNNAMED_CLASS) return;
    uint64_t f = ld_save();
    struct cpu *c = here();
    if (!c->ld.busy) pop(c->ld.h, &c->ld.n, l, (int)cls);
    ld_restore(f);
}

// Called with interrupts off (bkl.c), and a core holding a spinlock
// cannot move: the count is this core's whichever it reads.
void lockdep_bkl_acquire(uintptr_t site) {
    struct cpu *c = here();
    if (!c->ld.n || c->ld.busy) return;
    uint64_t f = ld_save();
    c->ld.busy = 1;
    const struct lockdep_held *h = &c->ld.h[c->ld.n - 1];
    class_report(LOCKDEP_BKL_IN_SPIN, h->cls, h->site, site);
    c->ld.busy = 0;   // no flush: the kernel lock is what this is taking
    ld_restore(f);
}

void lockdep_kmutex_acquired(struct kmutex *m, int sub, const struct kmutex *nest,
                             uintptr_t site, int trylock) {
    uint64_t f = ld_save();
    struct cpu *c = here();
    if (c->ld.busy) { ld_restore(f); return; }
    c->ld.busy = 1;
    int base = kmutex_class(m, site);
    struct lockdep_task *t = cur_task();
    if (base && t->hardirq > 0) {
        if (!trylock) class_report(LOCKDEP_KMUTEX_IN_IRQ, base, 0, site);
    } else if (base) {
        c->ld.checked++;
        if (!trylock)
            for (int i = 0; i < c->ld.n; i++)
                pair_report(LOCKDEP_SLEEP_IN_SPIN, c->ld.h[i].cls, base, c->ld.h[i].site, site);
        int node = node_of(base, sub);
        int merged = 0;
        for (int i = 0; nest && i < t->n; i++)
            if (t->h[i].nest == nest && t->h[i].cls == base) { t->h[i].count++; merged = 1; break; }
        if (!merged) {
            if (!trylock) check_order(t->h, t->n, node, m, site);
            push(t->h, &t->n, LOCKDEP_TASK_HELD, node, m, nest, site, &g_max_task);
        }
    }
    flush(1);
    c->ld.busy = 0;
    ld_restore(f);
}

void lockdep_kmutex_release(struct kmutex *m) {
    uint32_t cls = m->wait_lock.ld_class & ~LOCKDEP_KMUTEX_INNER;
    if (!cls || cls == LOCKDEP_UNNAMED_CLASS) return;
    uint64_t f = ld_save();
    struct cpu *c = here();
    struct lockdep_task *t = cur_task();
    if (!c->ld.busy && !t->hardirq) pop(t->h, &t->n, m, (int)cls);
    ld_restore(f);
}

// Only ever called with interrupts off (isr_dispatch()), on the running
// context's own field.
int lockdep_hardirq_enter(void) {
    struct lockdep_task *t = cur_task();
    int was = t->hardirq;
    t->hardirq = was + 1;
    return was;
}

// The context re-read: a switch inside the body resumed this one.
void lockdep_hardirq_exit(int was) { cur_task()->hardirq = was; }

int lockdep_hardirq_depth(void) { return cur_task()->hardirq; }

// A context with a scheduler slot only: the legacy loader runs ring 3
// from inside a kernel call on slot -1 (process_run_ring3()), whose
// caller's holds -- an fs exclusion -- are that call's, not the program's.
// Nothing held is one load and a branch: this runs on every return.
void lockdep_user_return(void) {
    struct lockdep_task *t = cur_task();
    if (!t->n || SCHED_HERE_GET(sched_cur) < 0) return;
    uint64_t f = ld_save();
    struct cpu *c = here();
    if (!c->ld.busy) {
        c->ld.busy = 1;
        for (int i = 0; i < t->n; i++) class_report(LOCKDEP_HELD_TO_USER, t->h[i].cls, 0, t->h[i].site);
        flush(0);
        c->ld.busy = 0;
    }
    ld_restore(f);
}

void lockdep_task_reset(struct lockdep_task *t) { k_memset(t, 0, sizeof *t); }

// ---- readers ------------------------------------------------------------

void lockdep_get_stats(struct lockdep_stats *out) {
    k_memset(out, 0, sizeof *out);
    out->classes = (uint32_t)g_ncls;
    out->classes_max = LD_MAX - 1;
    out->edges = (uint32_t)g_edges_total;
    out->edges_max = LD_MAX_EDGES;
    out->edges_unsited = (uint32_t)(g_edges_total - g_nedges);
    out->reports = (uint32_t)g_nrep;
    out->reports_max = LD_MAX_REPORTS;
    out->reports_lost = g_rep_lost;
    out->max_spin_held = g_max_spin;
    out->max_task_held = g_max_task;
    out->unmatched = g_unmatched;
    for (int i = 0; i < smp_cpu_count(); i++) {
        const struct cpu *c = smp_cpu_at(i);
        if (c) out->checked += c->ld.checked;
    }
    if (!smp_cpu_count()) out->checked = gdt_boot_cpu()->ld.checked;
}

int lockdep_report_count(void) { return __atomic_load_n(&g_nrep, __ATOMIC_ACQUIRE); }

int lockdep_report_get(int i, struct lockdep_report *out) {
    if (i < 0 || i >= lockdep_report_count()) return 0;
    *out = g_rep[i];
    return 1;
}

static int named(int id, const char *name) {
    const struct ld_class *k = &g_cls[g_cls[id].base];
    return k->name && k_strcmp(k->name, name) == 0;
}

void lockdep_forget(const char *name) {
    uint64_t f = glock();
    for (int id = 1; id <= g_ncls; id++) {
        if (!named(id, name)) continue;
        for (int o = 1; o <= g_ncls; o++) {
            clearbit(g_adj, id, o); clearbit(g_adj, o, id);
            clearbit(g_seen, id, o); clearbit(g_seen, o, id);
            clearbit(g_reported, id, o); clearbit(g_reported, o, id);
        }
        g_cls[id].usage = 0;
        g_cls[id].reported = 0;
    }
    int w = 0;
    for (int i = 0; i < g_nedges; i++) {
        if (named(g_edges[i].from, name) || named(g_edges[i].to, name)) { g_edges_total--; continue; }
        g_edges[w++] = g_edges[i];
    }
    g_nedges = w;
    w = 0;
    for (int i = 0; i < g_nrep; i++) {
        if ((g_rep[i].a && named(g_rep[i].a, name)) || (g_rep[i].b && named(g_rep[i].b, name))) continue;
        g_rep[w++] = g_rep[i];
    }
    g_nrep = w;
    gunlock(f);
}

// QUERY_LOCKDEP: the summary, then each report.
static int ld_query_count(void) { return 1 + lockdep_report_count(); }

static int ld_query_fill(int index, void *out) {
    struct query_lockdep *q = out;
    k_memset(q, 0, sizeof *q);
    uint64_t f = ld_save();
    struct cpu *c = here();
    if (!c->ld.busy) { c->ld.busy = 1; flush(0); c->ld.busy = 0; }   // the log first
    ld_restore(f);
    if (index == 0) {
        struct lockdep_stats s;
        lockdep_get_stats(&s);
        q->kind = QUERY_LOCKDEP_STATS;
        q->classes = s.classes; q->classes_max = s.classes_max;
        q->edges = s.edges; q->edges_max = s.edges_max;
        q->reports = s.reports; q->reports_lost = s.reports_lost;
        q->max_spin_held = s.max_spin_held; q->max_task_held = s.max_task_held;
        q->unmatched = s.unmatched;
        q->checked = s.checked;
        return 1;
    }
    struct lockdep_report r;
    if (!lockdep_report_get(index - 1, &r)) return 0;
    q->kind = r.kind;
    q->path_len = r.path_len;
    if (r.a) lockdep_class_name(r.a, q->a, sizeof q->a);
    if (r.b) lockdep_class_name(r.b, q->b, sizeof q->b);
    for (int i = 0; i < 4; i++)
        if (r.site[i]) site_str(r.site[i], q->site[i], sizeof q->site[i]);
    return 1;
}

static const struct query_provider lockdep_provider = {
    .cls = QUERY_LOCKDEP,
    .name = "lockdep",
    .record_size = sizeof(struct query_lockdep),
    .flags = QUERY_F_LIST,
    .count = ld_query_count,
    .fill = ld_query_fill,
};

static void lockdep_query_init(void) { query_register(&lockdep_provider); }
INITCALL(lockdep_query_init, INIT_QUERY);

#endif
