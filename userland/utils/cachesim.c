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
static int g_top;

static const struct uargs_opt OPTS[] = {
    { "interval", 'i', "SECONDS", "count only what is read during the next SECONDS", 0, &g_interval },
    { "top", 't', 0, "who reads: the heaviest processes and paths, instead of the sizes", &g_top, 0 },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "cachesim",
    .usage = "[-t] [-i SECONDS]",
    .summary = "How many 4 KiB reads a cache of each size would have served, measured\n"
               "by ghost caches that keep keys and no data. 'file' is a page cache above\n"
               "the filesystem; 'block' is a cache under it, metadata included.",
    .opts = OPTS,
    .notes = "Needs a kernel booted with the word 'cachesim' (bootcfg try 0 +cachesim).\n"
             "Exit status: 0 printed, 1 the kernel is not measuring, 2 a usage error.",
};

// ---- --top --------------------------------------------------------------

#define TOP_PROCS 32
#define TOP_PATHS 128
#define SHOW_PROCS 10
#define SHOW_PATHS 20

static struct query_cachesim_proc g_p0[TOP_PROCS], g_p1[TOP_PROCS];
static struct query_cachesim_path g_f0[TOP_PATHS], g_f1[TOP_PATHS];
static int g_np0, g_np1, g_nf0, g_nf1;

static int take_procs(struct query_cachesim_proc *out) {
    int n = 0;
    struct query_cachesim_proc r;
    QUERY_FOREACH(QUERY_CACHESIM_PROC, r, i) { if (n == TOP_PROCS) break; out[n++] = r; }
    return n;
}

static int take_paths(struct query_cachesim_path *out) {
    int n = 0;
    struct query_cachesim_path r;
    QUERY_FOREACH(QUERY_CACHESIM_PATH, r, i) { if (n == TOP_PATHS) break; out[n++] = r; }
    return n;
}

// The interval's share of one row: the same row in the first sample, or
// none. A row the kernel replaced in between restarted from zero, so a
// count that went DOWN is all new.
static uint64_t since(uint64_t now, uint64_t then) { return now >= then ? now - then : now; }

static void top_procs(void) {
    for (int i = 0; i < g_np1; i++) {
        for (int j = 0; j < g_np0; j++) {
            if (g_p0[j].pid != g_p1[i].pid || strcmp(g_p0[j].name, g_p1[i].name)) continue;
            g_p1[i].pages = since(g_p1[i].pages, g_p0[j].pages);
            g_p1[i].repeats = since(g_p1[i].repeats, g_p0[j].repeats);
            g_p1[i].written = since(g_p1[i].written, g_p0[j].written);
            break;
        }
    }
    printf("Block I/O, by process:\n%7s %-16s %10s %10s %10s\n",
           "pid", "name", "read", "repeats", "written");
    for (int shown = 0; shown < SHOW_PROCS; shown++) {
        int best = -1;
        uint64_t bw = 0;
        for (int i = 0; i < g_np1; i++) {
            uint64_t w = g_p1[i].pages + g_p1[i].written;
            if (w && (best < 0 || w > bw)) { best = i; bw = w; }
        }
        if (best < 0) break;
        struct query_cachesim_proc *e = &g_p1[best];
        printf("%7llu %-16s %10llu %10llu %10llu\n", (unsigned long long)e->pid, e->name,
               (unsigned long long)e->pages, (unsigned long long)e->repeats,
               (unsigned long long)e->written);
        e->pages = e->written = 0;
    }
}

static void top_paths(void) {
    for (int i = 0; i < g_nf1; i++) {
        for (int j = 0; j < g_nf0; j++) {
            if (g_f0[j].pid != g_f1[i].pid || strcmp(g_f0[j].path, g_f1[i].path)) continue;
            g_f1[i].lookups = since(g_f1[i].lookups, g_f0[j].lookups);
            g_f1[i].pages = since(g_f1[i].pages, g_f0[j].pages);
            g_f1[i].repeats = since(g_f1[i].repeats, g_f0[j].repeats);
            break;
        }
    }
    printf("\nFilesystem calls, by process and path:\n%7s %-16s %8s %8s %8s  %s\n",
           "pid", "name", "lookups", "pages", "repeats", "path");
    for (int shown = 0; shown < SHOW_PATHS; shown++) {
        int best = -1;
        uint64_t bw = 0;
        for (int i = 0; i < g_nf1; i++) {
            uint64_t w = g_f1[i].lookups + g_f1[i].pages;
            if (w && (best < 0 || w > bw)) { best = i; bw = w; }
        }
        if (best < 0) break;
        struct query_cachesim_path *e = &g_f1[best];
        printf("%7llu %-16s %8llu %8llu %8llu  %s\n", (unsigned long long)e->pid, e->name,
               (unsigned long long)e->lookups, (unsigned long long)e->pages,
               (unsigned long long)e->repeats, e->path);
        e->lookups = e->pages = 0;
    }
}

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
    if (n && g_top) {
        if (secs) {
            g_np0 = take_procs(g_p0);
            g_nf0 = take_paths(g_f0);
            sleep((unsigned)secs);
        }
        g_np1 = take_procs(g_p1);
        g_nf1 = take_paths(g_f1);
        printf(secs ? "Over %d s:\n" : "Since boot:\n", secs);
        top_procs();
        top_paths();
        printf("(4 KiB pages; a repeat is a page read before. The tables keep the heaviest\n"
               " readers only, so a light one may be missing.)\n");
        return 0;
    }
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
