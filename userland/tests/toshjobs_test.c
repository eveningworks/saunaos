// tosh's job table (userland/lib/tosh_jobs.c), past what it used to hold.
//
// It grows (bee9612), and two promises ride on that: a
// pointer from tosh_jobs_get() survives more jobs being added (each job
// is its own allocation), and ids stay the LOWEST free ones, so `fg 3`
// still means something in a shell with twenty jobs. Exits with the
// number of failures.
#include "lib/tosh_jobs.h"
#include "lib/utest.h"

#define N 20

static int g_seen[N + 2];

static void count(void *ctx, const struct tosh_job *j, char marker) {
    (void)ctx;
    if (j->id > 0 && j->id <= N + 1) g_seen[j->id]++;
    if (marker == '+') g_seen[0] = j->id;
}

int main(void) {
    utest_begin("toshjobs_test", "tosh's job table grows past eight", UTEST_VERDICT_FILE);

    int ok = 1;
    for (int i = 1; i <= N; i++) {
        int pid = 1000 + i;
        char cmd[16];
        snprintf(cmd, sizeof cmd, "job %d", i);
        int id = tosh_jobs_add(pid, &pid, 1, cmd, 1);
        if (id != i) { ok = 0; utest_notef("job %d got id %d", i, id); }
    }
    utest_check(ok, "twenty jobs are added, with ids 1..20");
    utest_checkf(tosh_jobs_current() == N, "the newest is current (%d)", tosh_jobs_current());

    struct tosh_job *first = tosh_jobs_get(1);
    int first_pgid = first ? first->pgid : -1;
    for (int i = 0; i < 40; i++) {             // grow the table under it
        int pid = 5000 + i;
        tosh_jobs_add(pid, &pid, 1, "filler", 0);
    }
    utest_check(first && first == tosh_jobs_get(1) && first->pgid == first_pgid,
                "a job's pointer survives the table growing");
    for (int i = 0; i < 40; i++) tosh_jobs_remove(N + 1 + i);

    tosh_jobs_each(count, 0);
    int each_ok = 1;
    for (int i = 1; i <= N; i++) if (g_seen[i] != 1) each_ok = 0;
    utest_check(each_ok, "jobs lists every one of the twenty, once");
    utest_checkf(g_seen[0] > 0, "one of them carries the + mark (%d)", g_seen[0]);

    tosh_jobs_remove(3);
    int pid = 9999;
    utest_checkf(tosh_jobs_add(pid, &pid, 1, "again", 1) == 3,
                 "a freed id is reused, the lowest first");

    for (int i = 1; i <= N; i++) tosh_jobs_remove(i);
    utest_check(!tosh_jobs_any() && !tosh_jobs_current(), "removing them all leaves none");
    return utest_end();
}
