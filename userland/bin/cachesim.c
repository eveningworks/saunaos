// cachesim -- what a page cache or a block cache WOULD have hit.
//
// Prints QUERY_CACHESIM: the kernel's ghost caches (kernel/include/
// kernel/cachesim.h), which keep keys only, at two levels and four
// sizes. Since boot, or over an interval -- vmstat's shape, two samples
// and their difference -- so one workload can be measured on its own.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/uargs.h"
#include "query_abi.h"

#define ROWS 16

static const char *g_interval;

static const struct uargs_opt OPTS[] = {
    { "interval", 'i', "SECONDS", "count only what is read during the next SECONDS", 0, &g_interval },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "cachesim",
    .usage = "[-i SECONDS]",
    .summary = "How many 4 KiB reads a cache of each size would have served, measured\n"
               "by ghost caches that keep keys and no data. 'file' is a page cache above\n"
               "the filesystem; 'block' is a cache under it, metadata included.",
    .opts = OPTS,
    .notes = "Needs a kernel booted with the word 'cachesim' (bootcfg try 0 +cachesim).\n"
             "Exit status: 0 printed, 1 the kernel is not measuring, 2 a usage error.",
};

static int sample(struct query_cachesim *rows) {
    int n = 0;
    struct query_cachesim r;
    QUERY_FOREACH(QUERY_CACHESIM, r, i) {
        if (n == ROWS) break;
        rows[n++] = r;
    }
    return n;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc) return uargs_error(&PROG, "unexpected '%s'", a.argv[0]);
    int secs = 0;
    if (g_interval) {
        char *end;
        long v = strtol(g_interval, &end, 10);
        if (*end || v < 1 || v > 86400) return uargs_error(&PROG, "an interval of %s is not 1..86400", g_interval);
        secs = (int)v;
    }

    static struct query_cachesim before[ROWS], after[ROWS];
    int n = sample(before);
    if (n == 0) {
        fprintf(stderr, "cachesim: not measuring -- boot with the word 'cachesim' "
                        "(bootcfg try 0 +cachesim)\n");
        return 1;
    }
    if (secs) {
        sleep((unsigned)secs);
        if (sample(after) != n) { fprintf(stderr, "cachesim: the tiers changed\n"); return 1; }
    } else {
        memcpy(after, before, sizeof before);
        memset(before, 0, sizeof before);
    }

    if (secs) printf("Over %d s:\n", secs);
    else printf("Since boot:\n");
    printf("%-6s %8s %10s %10s %6s %10s\n", "level", "size", "reads", "hits", "hit%", "resident");
    for (int i = 0; i < n; i++) {
        unsigned long long reads = after[i].lookups - before[i].lookups;
        unsigned long long hits = after[i].hits - before[i].hits;
        char size[16], pct[16];
        snprintf(size, sizeof size, "%llu MiB", (unsigned long long)after[i].size_kib / 1024);
        if (reads) snprintf(pct, sizeof pct, "%llu.%llu%%", hits * 100 / reads, hits * 1000 / reads % 10);
        else snprintf(pct, sizeof pct, "-");
        printf("%-6s %8s %10llu %10llu %6s %10llu\n", after[i].level, size, reads, hits, pct,
               (unsigned long long)after[i].resident);
    }
    printf("(4 KiB pages; resident below the size is the whole working set)\n");
    return 0;
}
