// `cpus` -- each core's state, for a machine where one has stopped
// answering: what runs there, whether it holds the kernel lock, how long
// since its last tick and where that tick landed, and its cross-core
// calls and shootdowns (QUERY_CPUSTATE). The debug console's own `cpus`
// prints the same record from the kernel, for when nothing in ring 3
// can start.
//
// A snapshot: another core keeps running while it is read, so a busy
// one's fields may be a moment apart -- Linux's /proc is no different.
#include <stdio.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "base/uargs.h"

static int g_verbose;

static const char *state_name(uint64_t st) {
    static const char *const names[] = {
        "absent", "failed", "wedged", "stopped", "starting", "alive", "parked", "running",
    };
    return st < sizeof names / sizeof names[0] ? names[st] : "?";
}

static void print_tick(const struct query_cpustate *q) {
    if (q->tick_age_ns == ~0ull) { printf("never"); return; }
    printf("%llu ms ago in ring %llu", (unsigned long long)(q->tick_age_ns / 1000000),
           (unsigned long long)(q->tick_cs & 3));
    if (g_verbose) printf(" at %#llx", (unsigned long long)q->tick_rip);
}


static const struct uargs_opt OPTS[] = {
    { "verbose", 'v', 0, "also where each core last ticked and took the kernel lock", &g_verbose, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "cpus",
    .summary = "Show each core's state: what runs there, the kernel lock, its last tick, its calls.",
    .opts = OPTS,
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc) return uargs_error(&PROG, "takes no arguments");

    struct query_cpustate q;
    int n = 0;
    QUERY_FOREACH(QUERY_CPUSTATE, q, i) {
        n++;
        printf("CPU %llu (APIC id %llu): %s", (unsigned long long)q.index,
               (unsigned long long)q.apic_id, state_name(q.state));
        if (q.state == QUERY_CPUSTATE_RUNNING || q.state == QUERY_CPUSTATE_PARKED) {
            if (q.pid > 0) printf(", running pid %lld", (long long)q.pid);
            else printf(", idle");
        }
        // Depth without the lock is a core queued for it (bkl_enter()
        // counts the entry before the spin).
        printf("\n  kernel lock: depth %llu%s\n", (unsigned long long)q.bkl_depth,
               q.bkl_holder ? ", held by this core" : q.bkl_depth ? ", waiting for it" : "");
        if (g_verbose && q.bkl_rip)
            printf("    last taken by vector %llu at %#llx for pid %lld\n",
                   (unsigned long long)q.bkl_vec, (unsigned long long)q.bkl_rip,
                   (long long)q.bkl_pid);
        printf("  last tick:   ");
        print_tick(&q);
        printf("\n  calls:       %llu run%s\n", (unsigned long long)q.calls_taken,
               q.call_pending ? ", one sent and not finished" : "");
        printf("  shootdowns:  %llu answered%s\n", (unsigned long long)q.flushes_taken,
               q.flush_pending ? ", one sent and not answered" : "");
        if (q.wedge_vec)
            printf("  wedged by:   vector %llu\n", (unsigned long long)(q.wedge_vec - 1));
        if (q.nmi_stray)
            printf("  stray NMIs:  %llu\n", (unsigned long long)q.nmi_stray);
    }
    if (!n) {
        printf("cpus: this kernel has no QUERY_CPUSTATE\n");
        return 1;
    }
    return 0;
}
