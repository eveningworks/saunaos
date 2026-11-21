// `lockstat` -- how contended the kernel's counted spinlocks are, Linux's
// /proc/lock_stat: takes, how many waited, the wait and hold times
// (QUERY_LOCKSTAT). `-e` adds who uses the kernel lock -- each core, and
// the syscalls and vectors that held it longest (QUERY_BKLSTAT). What
// SMP stage 7 splits next is read off these (docs/smp-design.md).
//
// The counters are cumulative since boot; `-i N` samples twice, N
// seconds apart, and prints the difference -- vmstat's shape, so nothing
// in the kernel is ever reset under another reader. A maximum cannot be
// differenced, so it stays the boot's.
//
// HOLD TIMES ARE ARMED (`lockstat track on`, the kernel.lockstat
// tunable): they cost two clock reads per take, so the kernel counts them
// only while asked. Takes and waits are always counted.
//
// `lockstat order` is the other half of Linux's family (/proc/lockdep_stats
// and lockdep's reports): what lock-order checking found, on a kernel
// built with `option lockdep` (QUERY_LOCKDEP, docs/lockdep-design.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "base/uargs.h"
#include "base/tunable.h"

#define TUNABLE "kernel.lockstat"

static int g_entries, g_all;
static const char *g_interval;

static const struct uargs_opt OPTS[] = {
    { "interval", 'i', "SECONDS", "count over the next SECONDS instead of since boot", 0, &g_interval },
    { "entries", 'e', 0, "also who uses the kernel lock: each core, and the entries holding it longest", &g_entries, 0 },
    { "all", 'a', 0, "with -e, every entry rather than the top 15", &g_all, 0 },
    { 0 },
};

static const struct uargs_cmd CMDS[] = {
    { "track", "on|off", "start or stop timing holds (starting zeroes them)" },
    { "order", 0, "what lock-order checking found (a kernel built with option lockdep)" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "lockstat",
    .usage = "[-i SECONDS] [-e [-a]]\ntrack on|off\norder",
    .summary = "Show how contended the kernel's spinlocks are: takes, waits and hold times.",
    .opts = OPTS,
    .cmds = CMDS,
};

struct locks { struct query_lockstat *v; int n; };
struct bkls  { struct query_bklstat *v; int n; };

static struct locks take_locks(void) {
    struct locks l = {0};
    int cap = 0;
    struct query_lockstat r;
    QUERY_FOREACH(QUERY_LOCKSTAT, r, i) {
        if (l.n == cap) {
            cap = cap ? cap * 2 : 8;
            struct query_lockstat *nv = realloc(l.v, (size_t)cap * sizeof *nv);
            if (!nv) break;
            l.v = nv;
        }
        l.v[l.n++] = r;
    }
    return l;
}

static struct bkls take_bkl(void) {
    struct bkls b = {0};
    int cap = 0;
    struct query_bklstat r;
    QUERY_FOREACH(QUERY_BKLSTAT, r, i) {
        if (b.n == cap) {
            cap = cap ? cap * 2 : 64;
            struct query_bklstat *nv = realloc(b.v, (size_t)cap * sizeof *nv);
            if (!nv) break;
            b.v = nv;
        }
        b.v[b.n++] = r;
    }
    return b;
}

static uint64_t minus(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }

// Subtract the earlier sample's row with the same name or key from each
// of `now`'s. A row missing earlier was new, and stays whole.
static void diff_locks(struct locks now, struct locks then) {
    for (int i = 0; i < now.n; i++)
        for (int j = 0; j < then.n; j++) {
            if (strcmp(now.v[i].name, then.v[j].name) != 0) continue;
            now.v[i].acquired = minus(now.v[i].acquired, then.v[j].acquired);
            now.v[i].contended = minus(now.v[i].contended, then.v[j].contended);
            now.v[i].wait_ns = minus(now.v[i].wait_ns, then.v[j].wait_ns);
            now.v[i].hold_ns = minus(now.v[i].hold_ns, then.v[j].hold_ns);
            break;
        }
}

static void diff_bkl(struct bkls now, struct bkls then) {
    for (int i = 0; i < now.n; i++)
        for (int j = 0; j < then.n; j++) {
            if (now.v[i].kind != then.v[j].kind || now.v[i].id != then.v[j].id) continue;
            now.v[i].takes = minus(now.v[i].takes, then.v[j].takes);
            now.v[i].contended = minus(now.v[i].contended, then.v[j].contended);
            now.v[i].wait_ns = minus(now.v[i].wait_ns, then.v[j].wait_ns);
            now.v[i].hold_ns = minus(now.v[i].hold_ns, then.v[j].hold_ns);
            break;
        }
}

// A duration in the unit that keeps it short: "840 ns", "12.3 us", "4.1 s".
static const char *dur(uint64_t ns, char *buf, size_t cap) {
    if (ns < 1000) snprintf(buf, cap, "%llu ns", (unsigned long long)ns);
    else if (ns < 1000000) snprintf(buf, cap, "%llu.%llu us", (unsigned long long)(ns / 1000), (unsigned long long)(ns / 100 % 10));
    else if (ns < 1000000000) snprintf(buf, cap, "%llu.%llu ms", (unsigned long long)(ns / 1000000), (unsigned long long)(ns / 100000 % 10));
    else snprintf(buf, cap, "%llu.%llu s", (unsigned long long)(ns / 1000000000), (unsigned long long)(ns / 100000000 % 10));
    return buf;
}

// "3.7%" of `whole`, to a tenth.
static const char *pct(uint64_t part, uint64_t whole, char *buf, size_t cap) {
    if (!whole) { snprintf(buf, cap, "-"); return buf; }
    uint64_t t = part * 1000 / whole;
    snprintf(buf, cap, "%llu.%llu%%", (unsigned long long)(t / 10), (unsigned long long)(t % 10));
    return buf;
}

static int g_timed;   // hold times are being counted: a "-" otherwise

static const char *hold(uint64_t ns, char *buf, size_t cap) {
    if (!g_timed) { snprintf(buf, cap, "-"); return buf; }
    return dur(ns, buf, cap);
}

static void print_locks(struct locks l, uint64_t span_ns) {
    printf("%-8s %10s %9s %6s %10s %10s %10s %10s %6s\n", "LOCK", "TAKES", "WAITED", "", "WAIT", "WAIT MAX",
           "HOLD", "HOLD MAX", "HELD");
    for (int i = 0; i < l.n; i++) {
        const struct query_lockstat *q = &l.v[i];
        char a[16], b[16], c[16], d[16], e[16], f[16];
        printf("%-8s %10llu %9llu %6s %10s %10s %10s %10s %6s\n", q->name,
               (unsigned long long)q->acquired, (unsigned long long)q->contended,
               pct(q->contended, q->acquired, a, sizeof a), dur(q->wait_ns, b, sizeof b),
               dur(q->wait_max_ns, c, sizeof c), hold(q->hold_ns, d, sizeof d),
               hold(q->hold_max_ns, e, sizeof e),
               g_timed ? pct(q->hold_ns, span_ns, f, sizeof f) : hold(0, f, sizeof f));
    }
}

static const char *entry_name(const struct query_bklstat *q, char *buf, size_t cap) {
    switch (q->kind) {
    case QUERY_BKLSTAT_SYSCALL:
        if (q->name[0]) snprintf(buf, cap, "syscall %s", q->name);
        else snprintf(buf, cap, "syscall %llu", (unsigned long long)q->id);
        break;
    case QUERY_BKLSTAT_VECTOR:   // the kernel names it: "page fault", "irq 14", a handler
        if (q->name[0]) snprintf(buf, cap, "%s", q->name);
        else snprintf(buf, cap, "vector %llu", (unsigned long long)q->id);
        break;
    case QUERY_BKLSTAT_KERNEL: snprintf(buf, cap, "kernel"); break;
    default: snprintf(buf, cap, "?"); break;
    }
    return buf;
}

static int by_hold(const void *a, const void *b) {
    const struct query_bklstat *x = a, *y = b;
    return x->hold_ns < y->hold_ns ? 1 : x->hold_ns > y->hold_ns ? -1 : 0;
}

static int by_wait(const void *a, const void *b) {
    const struct query_bklstat *x = a, *y = b;
    return x->wait_ns < y->wait_ns ? 1 : x->wait_ns > y->wait_ns ? -1 : 0;
}

static void print_bkl(struct bkls b, uint64_t span_ns) {
    printf("\nKernel lock by core:\n");
    printf("%-8s %10s %9s %6s %10s %10s %6s\n", "CPU", "TAKES", "WAITED", "", "WAIT", "HOLD", "HELD");
    int first = b.n;
    for (int i = 0; i < b.n; i++) {
        const struct query_bklstat *q = &b.v[i];
        if (q->kind != QUERY_BKLSTAT_CORE) { first = i; break; }
        char x[16], y[16], z[16], w[16];
        printf("%-8llu %10llu %9llu %6s %10s %10s %6s\n", (unsigned long long)q->id,
               (unsigned long long)q->takes, (unsigned long long)q->contended,
               pct(q->contended, q->takes, x, sizeof x), dur(q->wait_ns, y, sizeof y),
               hold(q->hold_ns, z, sizeof z),
               g_timed ? pct(q->hold_ns, span_ns, w, sizeof w) : hold(0, w, sizeof w));
    }
    int n = b.n - first, active = 0;
    for (int i = first; i < b.n; i++) active += b.v[i].takes || b.v[i].hold_ns;
    if (g_timed) qsort(b.v + first, (size_t)n, sizeof *b.v, by_hold);
    else qsort(b.v + first, (size_t)n, sizeof *b.v, by_wait);
    int shown = 0;
    printf(g_timed ? "\nKernel lock by entry, longest held first:\n"
                   : "\nKernel lock by entry, longest waited for first:\n");
    printf("%-22s %10s %9s %10s %10s %10s %10s\n", "ENTRY", "TAKES", "WAITED", "WAIT", "WAIT MAX", "HOLD",
           "HOLD MAX");
    for (int i = first; i < b.n; i++) {
        const struct query_bklstat *q = &b.v[i];
        if (!q->takes && !q->hold_ns) continue;   // nothing in this interval
        if (!g_all && shown == 15) {
            printf("(%d more; -a lists every one)\n", active - shown);
            break;
        }
        char nm[40], c[16], d[16], e[16], f[16];
        printf("%-22s %10llu %9llu %10s %10s %10s %10s\n", entry_name(q, nm, sizeof nm),
               (unsigned long long)q->takes, (unsigned long long)q->contended,
               dur(q->wait_ns, c, sizeof c), dur(q->wait_max_ns, d, sizeof d),
               hold(q->hold_ns, e, sizeof e), hold(q->hold_max_ns, f, sizeof f));
        shown++;
    }
}

static const char *kind_name(uint32_t k) {
    switch (k) {
    case QUERY_LOCKDEP_INVERSION: return "lock order inversion";
    case QUERY_LOCKDEP_RECURSION: return "two of one class held";
    case QUERY_LOCKDEP_IRQ_INCONSISTENT: return "taken in an interrupt and with interrupts on";
    case QUERY_LOCKDEP_IRQ_UNSAFE_ORDER: return "irq-safe lock held taking an irq-unsafe one";
    case QUERY_LOCKDEP_SLEEP_IN_SPIN: return "kmutex taken holding a spinlock";
    case QUERY_LOCKDEP_KMUTEX_IN_IRQ: return "kmutex taken in an interrupt";
    case QUERY_LOCKDEP_BKL_IN_SPIN: return "kernel lock taken holding a spinlock";
    case QUERY_LOCKDEP_HELD_TO_USER: return "back to ring 3 holding a kmutex";
    case QUERY_LOCKDEP_UNNAMED: return "an unnamed lock, not checked";
    default: return "?";
    }
}

static void print_finding(int n, const struct query_lockdep *q) {
    printf("%d. %s\n", n, kind_name(q->kind));
    switch (q->kind) {
    case QUERY_LOCKDEP_INVERSION:
        printf("   now:    holding \"%s\" (taken at %s), took \"%s\" at %s\n", q->a, q->site[0], q->b, q->site[1]);
        printf("   before: holding \"%s\" (taken at %s), took the next at %s", q->b, q->site[2], q->site[3]);
        if (q->path_len > 1) printf(" -- a cycle of %u; dmesg has every edge", q->path_len + 1);
        printf("\n");
        break;
    case QUERY_LOCKDEP_RECURSION:
        printf("   \"%s\" taken at %s while one was held from %s\n", q->a, q->site[1], q->site[0]);
        break;
    case QUERY_LOCKDEP_IRQ_INCONSISTENT:
        printf("   \"%s\" in an interrupt at %s, with interrupts on at %s\n", q->a, q->site[0], q->site[1]);
        break;
    case QUERY_LOCKDEP_UNNAMED:
        printf("   first taken at %s\n", q->site[1]);
        break;
    case QUERY_LOCKDEP_KMUTEX_IN_IRQ:
    case QUERY_LOCKDEP_HELD_TO_USER:
        printf("   \"%s\" taken at %s\n", q->a, q->site[1]);
        break;
    default:   // a pair: a held from site[0], b (or the kernel lock) taken at site[1]
        printf("   holding \"%s\" (taken at %s), took %s%s%s at %s\n", q->a, q->site[0],
               q->b[0] ? "\"" : "", q->b[0] ? q->b : "it", q->b[0] ? "\"" : "", q->site[1]);
        break;
    }
}

static int order(void) {
    struct query_lockdep q;
    int n = 0;
    QUERY_FOREACH(QUERY_LOCKDEP, q, i) {
        if (q.kind == QUERY_LOCKDEP_STATS) {
            printf("Lock-order checking: %u classes [max %u], %u dependencies [max %u], "
                   "%llu takes checked\n", q.classes, q.classes_max, q.edges, q.edges_max,
                   (unsigned long long)q.checked);
            printf("Most held at once: %u spinlocks on a core, %u kmutexes in a context",
                   q.max_spin_held, q.max_task_held);
            if (q.unmatched) printf("; %u releases matched no take", q.unmatched);
            printf("\n");
            if (!q.reports) printf("Nothing found.\n");
            else printf("\n%u finding%s%s:\n", q.reports, q.reports == 1 ? "" : "s",
                        q.reports_lost ? " (and more the table had no room for)" : "");
            n++;
            continue;
        }
        print_finding(n++, &q);
    }
    if (!n) {
        printf("lockstat: this kernel was built without lock-order checking "
               "(option lockdep = yes in build.conf, or make iso PROFILE=debug)\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc && strcmp(a.argv[0], "order") == 0) {
        if (a.argc != 1) return uargs_error(&PROG, "order takes no arguments");
        return order();
    }
    if (a.argc) {   // `track`
        if (a.argc != 2 || (strcmp(a.argv[1], "on") && strcmp(a.argv[1], "off")))
            return uargs_error(&PROG, "track takes on or off");
        // `on` writes off first: a setting written the value it holds
        // does nothing, so a repeat `on` would keep the old totals
        // (`stalls reset`'s reason).
        int on = strcmp(a.argv[1], "on") == 0;
        if ((on && !tunable_set(TUNABLE, "off")) || !tunable_set(TUNABLE, a.argv[1])) {
            printf("lockstat: the kernel refused that (no calibrated TSC rate?)\n");
            return 1;
        }
        printf(on ? "lockstat: timing holds (totals zeroed)\n"
                                            : "lockstat: hold timing off\n");
        return 0;
    }
    int secs = 0;
    if (g_interval) {
        char *end;
        long v = strtol(g_interval, &end, 10);
        if (*end || v < 1 || v > 86400) return uargs_error(&PROG, "an interval of %s is not 1..86400", g_interval);
        secs = (int)v;
    }

    struct locks l0 = take_locks();
    if (!l0.n) {
        printf("lockstat: this kernel has no QUERY_LOCKSTAT\n");
        return 1;
    }
    struct bkls b0 = g_entries ? take_bkl() : (struct bkls){0};
    if (secs) sleep((unsigned)secs);
    struct locks l = secs ? take_locks() : l0;
    struct bkls b = secs && g_entries ? take_bkl() : b0;
    // HELD is the share of the span a lock was held: the interval, or
    // the time since hold timing was armed.
    // Re-armed between the samples means zeroed between them: no holds.
    g_timed = l.v[0].timed_since_ns != 0 && l.v[0].timed_since_ns == l0.v[0].timed_since_ns;
    uint64_t span = secs ? minus(l.v[0].now_ns, l0.v[0].now_ns)
                         : minus(l.v[0].now_ns, l.v[0].timed_since_ns);
    if (secs) {
        diff_locks(l, l0);
        diff_bkl(b, b0);
        printf("Over %d s (the maxima are since boot):\n", secs);
    } else {
        printf("Since boot%s:\n", g_timed ? "; holds since `lockstat track on`" : "");
    }
    if (!g_timed) printf("(hold times are off -- `lockstat track on` counts them)\n");
    print_locks(l, span);
    if (g_entries) print_bkl(b, span);
    return 0;
}
