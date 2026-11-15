// stats16.c -- `w16 --stats FILE`: where a program's time goes, rewritten every few seconds and at
// its end. The program's own instructions against the time inside each module's calls (USER, GDI,
// KERNEL, ...) and DOS/DPMI's interrupts, MIPS over the wall clock and over the program's own
// time, and what the JIT did (jit16.h). What the 2026-11-15 timing build measured, kept as a switch
// so the build that is published is the build that is measured.
//
// THE TIME IS EXCLUSIVE: a stack of who is running -- the program, a module's call, an interrupt
// -- and a window procedure the program gave USER, called back from inside DispatchMessage, is
// the program's again (w16_stats_enter(W16_STATS_OWN)). THE CLOCK IS THE TSC, calibrated against
// CLOCK_MONOTONIC: a call into Windows is a few hundred nanoseconds, and a clock_gettime() around
// each would be most of what it measured.
#include "w16_internal.h"                 // the w16_stats_* declarations
#include "jit16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STATS_EVERY_S 5
#define DEPTH 64                          // nested calls and callbacks; deeper ones are not split

int g_w16_stats_on;
static FILE *g_f;
static const char *g_path;
static uint64_t g_t0, g_ref_tsc, g_mark;
static double g_ref_s;
// A row's name is COPIED: a module's name lives in w->thunks, which grows by realloc as modules load.
// One row per module the program calls, grown as they appear.
struct row { char name[16]; uint64_t tsc, calls; };
static struct row *g_row;
static int g_nrow, g_caprow, g_cur, g_stack[DEPTH], g_depth;

static uint64_t tsc(void) { return __builtin_ia32_rdtsc(); }
static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int w16_stats_open(const char *path) {
    g_f = fopen(path, "w");
    if (!g_f) return -1;
    g_path = path;
    g_ref_s = mono();
    g_t0 = g_ref_tsc = g_mark = tsc();
    g_caprow = 16;
    if (!(g_row = calloc((size_t)g_caprow, sizeof *g_row))) { fclose(g_f); g_f = 0; return -1; }
    snprintf(g_row[0].name, sizeof g_row[0].name, "%s", W16_STATS_OWN);
    snprintf(g_row[1].name, sizeof g_row[1].name, "%s", W16_STATS_INT);
    g_nrow = 2;
    g_cur = 0;
    g_w16_stats_on = 1;
    return 0;
}

static int row_of(const char *name) {
    for (int i = 0; i < g_nrow; i++)
        if (!strncmp(g_row[i].name, name, sizeof g_row[i].name - 1)) return i;
    if (g_nrow == g_caprow) {
        struct row *n = realloc(g_row, sizeof *g_row * (size_t)g_caprow * 2);
        if (!n) return 0;                        // no memory: charged to the program's own row
        g_row = n;
        g_caprow *= 2;
    }
    memset(&g_row[g_nrow], 0, sizeof g_row[g_nrow]);
    snprintf(g_row[g_nrow].name, sizeof g_row[g_nrow].name, "%s", name);
    return g_nrow++;
}

void w16_stats_enter(const char *name) {
    if (!g_f) return;
    uint64_t now = tsc();
    g_row[g_cur].tsc += now - g_mark;
    g_mark = now;
    if (g_depth < DEPTH) g_stack[g_depth] = g_cur;
    g_depth++;
    g_cur = row_of(name);
    g_row[g_cur].calls++;
}

void w16_stats_leave(void) {
    if (!g_f || !g_depth) return;
    uint64_t now = tsc();
    g_row[g_cur].tsc += now - g_mark;
    g_mark = now;
    g_depth--;
    if (g_depth < DEPTH) g_cur = g_stack[g_depth];
}

static void write_all(struct cpu16 *c, const char *when) {
    uint64_t now = tsc();
    g_row[g_cur].tsc += now - g_mark;            // what is running now, up to now
    g_mark = now;
    double s = mono() - g_ref_s;
    if (s < 0.5) return;
    double hz = (double)(now - g_ref_tsc) / s;   // the TSC's rate over the whole run
    double wall = (double)(now - g_t0) / hz, own = (double)g_row[0].tsc / hz;
    if (!freopen(g_path, "w", g_f)) { g_f = 0; return; }
    fprintf(g_f, "w16 stats (%s): %.1f s, %llu instructions\n", when, wall, (unsigned long long)c->steps);
    fprintf(g_f, "MIPS %.1f over the wall clock, %.1f over the program's own time\n",
            (double)c->steps / wall / 1e6, own > 0 ? (double)c->steps / own / 1e6 : 0.0);
    for (int i = 0; i < g_nrow; i++) {
        double t = (double)g_row[i].tsc / hz;
        fprintf(g_f, "%-10s %8.2f s  %5.1f%%", g_row[i].name, t, 100 * t / wall);
        if (i) fprintf(g_f, "  %llu %s", (unsigned long long)g_row[i].calls, i == 1 ? "interrupts" : "calls");
        fprintf(g_f, "\n");
    }
    const struct jit16_stats *j = jit16_stats(c);
    if (!j) {
        fprintf(g_f, "jit: off\n");
    } else {
        fprintf(g_f, "jit: %llu blocks of %llu instructions, %llu KiB of code; %.1f%% of instructions run "
                     "translated\n", (unsigned long long)j->blocks, (unsigned long long)j->insns,
                (unsigned long long)(j->code_bytes >> 10),
                c->steps ? 100.0 * (double)j->run_insns / (double)c->steps : 0.0);
        uint64_t calls = 0;
        for (int k = 0; k < 512; k++) calls += j->callout[k];
        fprintf(g_f, "jit: %llu block runs, %llu ended, %llu left early, %llu refused, "
                     "%llu invalidated, %llu flushes, %llu segment changes\n",
                (unsigned long long)j->runs, (unsigned long long)j->exit_end, (unsigned long long)j->exit_slow,
                (unsigned long long)j->refused, (unsigned long long)j->invalidated, (unsigned long long)j->flushes,
                (unsigned long long)j->guard_miss);
        fprintf(g_f, "jit: %llu instructions handed to the interpreter from inside a block (%.1f%%), "
                     "%llu of them leaving it:\n", (unsigned long long)calls,
                c->steps ? 100.0 * (double)calls / (double)c->steps : 0.0, (unsigned long long)j->callout_exit);
        int done[12], nd = 0;
        while (nd < 12) {
            int best = -1;
            uint64_t bv = 0;
            for (int k = 0; k < 512; k++) {
                int dup = 0;
                for (int m = 0; m < nd; m++) dup |= done[m] == k;
                if (!dup && j->callout[k] > bv) { bv = j->callout[k]; best = k; }
            }
            if (best < 0) break;
            done[nd++] = best;
            fprintf(g_f, "  %s%02x: %llu\n", best < 256 ? "" : "0f ", best & 0xFF, (unsigned long long)bv);
        }
        // Where runs fell out of translated code for want of an instruction -- how often, most
        // first -- and how many blocks end there: the translator's next instructions.
        fprintf(g_f, "jit: runs that ended before an instruction it lacks (blocks ending there):\n");
        int shown[24], n = 0;
        while (n < 24) {
            int best = -1;
            uint64_t bv = 0;
            for (int k = 0; k < 512; k++) {
                int dup = 0;
                for (int m = 0; m < n; m++) dup |= shown[m] == k;
                if (!dup && j->run_stop[k] > bv) { bv = j->run_stop[k]; best = k; }
            }
            if (best < 0) break;
            shown[n++] = best;
            fprintf(g_f, "  %s%02x: %llu (%u)\n", best < 256 ? "" : "0f ", best & 0xFF, (unsigned long long)bv,
                    best < 256 ? j->stop_op[best] : j->stop_0f[best - 256]);
        }
    }
    fflush(g_f);
}

// From the program's tick (every CPU16_TICK_STEPS instructions): one clock read each.
void w16_stats_tick(struct cpu16 *c) {
    static double next;
    if (!g_f) return;
    double now = mono();
    if (now < next) return;
    next = now + STATS_EVERY_S;
    write_all(c, "running");
}

void w16_stats_close(struct cpu16 *c) {
    if (!g_f) return;
    write_all(c, "ended");
    fclose(g_f);
    g_f = 0;
    g_w16_stats_on = 0;
}
