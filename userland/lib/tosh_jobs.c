// The shell's job table -- see tosh_jobs.h for what a job is and why
// this is the shell's problem rather than the kernel's.
//
// Grown on demand, as bash's is. Each job is its own allocation, so a
// pointer from tosh_jobs_get() survives another job being added.
#include "lib/tosh_jobs.h"
#include <stdlib.h>
#include <string.h>

static struct tosh_job **g_jobs; // g_cap of them; NULL until first used
static int g_cap;
static int g_current;   // id, or 0
static int g_previous;  // id, or 0

static struct tosh_job *slot_of(int id) {
    if (id <= 0) return 0;
    for (int i = 0; i < g_cap; i++)
        if (g_jobs[i] && g_jobs[i]->id == id) return g_jobs[i];
    return 0;
}

int tosh_jobs_cap(void) { return g_cap; }

// The lowest id nothing is using. Ids are REUSED once a job is gone,
// which is what keeps them small enough to type -- bash does the same,
// and an ever-increasing counter would have `fg 47` in a shell with two
// jobs in it.
static int next_id(void) {
    for (int id = 1; ; id++)
        if (!slot_of(id)) return id;   // at most g_cap + 1
}

// A free slot, growing the table when none is.
static struct tosh_job *free_slot(void) {
    int i = 0;
    while (i < g_cap && g_jobs[i] && g_jobs[i]->id) i++;
    if (i == g_cap) {
        int cap = g_cap ? g_cap * 2 : 8;
        struct tosh_job **n = realloc(g_jobs, (size_t)cap * sizeof *n);
        if (!n) return 0;
        for (int k = g_cap; k < cap; k++) n[k] = 0;
        g_jobs = n;
        g_cap = cap;
    }
    if (!g_jobs[i] && !(g_jobs[i] = malloc(sizeof *g_jobs[i]))) return 0;
    memset(g_jobs[i], 0, sizeof *g_jobs[i]);
    return g_jobs[i];
}

int tosh_jobs_add(int pgid, const int *pids, int npid, const char *cmd,
                  int stopped) {
    int id = next_id();
    struct tosh_job *j = free_slot();
    if (!j) return 0;

    j->id = id;
    j->pgid = pgid;
    j->npid = 0;
    for (int i = 0; i < npid && i < TOSH_JOB_PIDS_MAX; i++)
        if (pids[i] > 0) j->pid[j->npid++] = pids[i];
    j->stopped = stopped;
    int n = 0;
    for (; cmd && cmd[n] && n < TOSH_JOB_CMD_MAX - 1; n++) j->cmd[n] = cmd[n];
    j->cmd[n] = '\0';

    if (g_current != id) g_previous = g_current;
    g_current = id;
    return id;
}

struct tosh_job *tosh_jobs_get(int id) {
    return slot_of(id ? id : g_current);
}

void tosh_jobs_remove(int id) {
    struct tosh_job *j = slot_of(id);
    if (!j) return;
    j->id = 0;
    j->pgid = j->stopped = j->npid = 0;
    for (int i = 0; i < TOSH_JOB_PIDS_MAX; i++) j->pid[i] = 0;
    j->cmd[0] = '\0';

    // THE MARKERS HAVE TO BE REPAIRED, not just cleared. A `+` pointing
    // at a job that has exited is worse than no marker: a bare `fg`
    // would find nothing and report an empty shell while `jobs` still
    // listed one. So the previous job is promoted, and if there is no
    // previous, whatever is left takes the mark.
    if (g_current == id) {
        g_current = g_previous;
        g_previous = 0;
    }
    if (g_previous == id) g_previous = 0;
    if (!g_current) {
        for (int i = 0; i < g_cap; i++)
            if (g_jobs[i] && g_jobs[i]->id) { g_current = g_jobs[i]->id; break; }
    }
}

int tosh_jobs_any(void) {
    for (int i = 0; i < g_cap; i++) if (g_jobs[i] && g_jobs[i]->id) return 1;
    return 0;
}

int tosh_jobs_current(void) { return g_current; }

void tosh_jobs_each(void (*fn)(void *ctx, const struct tosh_job *job, char marker),
                    void *ctx) {
    if (!fn) return;
    // BY ID, not by slot: the array's order is whichever slots happened
    // to be free, and a list that jumps about between runs reads as a
    // bug in the shell.
    for (int id = 1; id <= g_cap; id++) {
        struct tosh_job *j = slot_of(id);
        if (!j) continue;
        char marker = id == g_current ? '+' : (id == g_previous ? '-' : ' ');
        fn(ctx, j, marker);
    }
}
