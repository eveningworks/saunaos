// A BENCHMARK, not a test: how long ONE filesystem call on a path waits,
// call by call, for a fixed stretch of wall time.
//
//   fslat_bench [--path P] [--secs N] [--gap-ms G] [--out FILE]
//               [--during PROFILE --log FILE]
//
// It exists for tools/tests/sweep/fs_isolation.py, which asks whether I/O on one
// path makes calls on another WAIT -- the claim each stage of
// docs/fslock-design.md makes. A throughput figure cannot answer that:
// a lock wait of one disk operation hides inside an average of
// thousands of microsecond calls. The MAXIMUM and the tail are the
// evidence, so those are what it reports.
//
// The call is stat(): it takes the mount's lock like every fs call and
// does no I/O of its own on a ramfs, so what it measures is the wait.
//
// --gap-ms SLEEPS between calls, and that is the realistic shape: an
// app or the compositor touches the filesystem now and then. With no
// gap the probe is a tight loop that spends nearly all its time holding
// a lock, so under ONE lock it starves whatever it is measured against
// instead of waiting behind it -- the first measurement read exactly
// backwards for that reason.
//
// --during waits until the LATEST `diskbench: progress` line in --log
// names PROFILE, then probes, then reads the log again: a probe that
// outlived the phase measured the next one, so it says so and exits 1.
// On the hardware, where fs_isolation.py's console polling cannot reach,
// this is what puts the probe inside a phase; a fixed sleep cannot,
// because a SEQ-write's speed varies by tens of seconds run to run.
//
// Output, parsed:
//   fslat: calls <n> avg-us <a> p99-us <p> max-us <m> path <P>
//   fslat: during <PROFILE> held yes|no          (with --during only)
// written to stdout AND to --out, because a console capture does not
// outlive a spawn (stdio_bench's reason).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rt/sys.h"

// Latency histogram in microseconds, 1 us buckets up to the cap and
// everything beyond in the last one -- the max is kept exactly.
#define HIST_US 20000

static uint32_t g_hist[HIST_US + 1];

// The profile named by the log's LAST progress line into `out`; 0 when
// there is none yet. The log is a few KB of protocol lines, read whole.
static int last_phase(const char *log, char *out, size_t cap) {
    FILE *f = fopen(log, "r");
    if (!f) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char prof[64];
        if (sscanf(line, "diskbench: progress %63s", prof) == 1) {
            snprintf(out, cap, "%s", prof);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

int main(int argc, char **argv) {
    const char *path = "/tmp";
    const char *out = 0, *during = 0, *log = 0;
    int secs = 5, gap_ms = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--gap-ms") && i + 1 < argc) gap_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--during") && i + 1 < argc) during = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
    }
    if (secs < 1) secs = 1;
    if (!during != !log) {
        fputs("fslat: --during and --log go together\n", stderr);
        return 2;
    }

    // Wait for the phase, bounded: a load that never reaches it is an
    // answer too, and an unbounded wait would hang the session.
    char phase[64] = "";
    if (during) {
        uint64_t give_up = sys_monotonic_ns() + 900ull * 1000000000ull;
        while (!last_phase(log, phase, sizeof phase) || strcmp(phase, during) != 0) {
            if (sys_monotonic_ns() > give_up) {
                fprintf(stderr, "fslat: %s never reached in %s (last: %s)\n",
                        during, log, phase[0] ? phase : "none");
                return 2;
            }
            usleep(50000);
        }
    }

    uint64_t end = sys_monotonic_ns() + (uint64_t)secs * 1000000000ull;
    uint64_t calls = 0, sum_us = 0, max_us = 0;
    struct stat st;
    while (sys_monotonic_ns() < end) {
        uint64_t t0 = sys_monotonic_ns();
        stat(path, &st);
        uint64_t us = (sys_monotonic_ns() - t0) / 1000u;
        g_hist[us < HIST_US ? us : HIST_US]++;
        sum_us += us;
        if (us > max_us) max_us = us;
        calls++;
        if (gap_ms > 0) usleep((unsigned long)gap_ms * 1000u);
    }

    uint64_t p99 = 0, seen = 0, want = calls - calls / 100;
    for (uint32_t b = 0; b <= HIST_US; b++) {
        seen += g_hist[b];
        if (seen >= want) { p99 = b; break; }
    }

    char line[256];
    snprintf(line, sizeof line, "fslat: calls %llu avg-us %llu p99-us %llu max-us %llu path %s\n",
             (unsigned long long)calls,
             (unsigned long long)(calls ? sum_us / calls : 0),
             (unsigned long long)p99, (unsigned long long)max_us, path);
    int held = 1;
    if (during) {
        // diskbench --out REWRITES its snapshot, so one read can land on
        // an emptied file; a few tries before calling the phase gone.
        int seen = 0;
        for (int tries = 0; tries < 5 && !(seen = last_phase(log, phase, sizeof phase)); tries++)
            usleep(10000);
        held = seen && strcmp(phase, during) == 0;
        size_t n = strlen(line);
        snprintf(line + n, sizeof line - n, "fslat: during %s held %s\n",
                 during, held ? "yes" : "no");
    }
    fputs(line, stdout);
    if (out) {
        FILE *f = fopen(out, "w");
        if (f) { fputs(line, f); fclose(f); }
    }
    return held ? 0 : 1;
}
