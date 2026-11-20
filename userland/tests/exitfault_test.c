// A THREAD STILL RUNNING WHEN ITS GROUP EXITS DIES OF THE EXIT, NOT OF A
// PAGE FAULT.
//
// Under smp=run a group exit finds a sibling in ring 3 on another core,
// raises its SIGKILL and kicks that core -- and until the kick lands, the
// sibling runs on. A page fault it takes in that window must still be
// served (the address space stays until the last thread has left), and
// one the kernel cannot serve must end in the SIGKILL. The leader's
// zombie used to hide the address space from the fault handler, so the
// sibling's next demand-paged touch became a "RING-3 CRASH" and a crash
// report (telnetd on the ASUS, 2026-11-20).
//
// So: children whose threads touch fresh pages without pause while the
// main thread exits; each must be reaped with the main thread's code,
// and none may leave a crash report -- the code alone cannot tell, since
// a group keeps its FIRST death's reason (red with the fix reverted:
// 309 crashes logged, every code still right).
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: any boot with one core** --
// there a killed sibling never runs again. It is a check for smp=run.
//
// It must be SPAWNED, not `run` -- it has children and threads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <dirent.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/exitfault_test"
#define CHILD_CODE 42
#define TOUCHERS 3
#define SPAN (256u << 20)   // reserved, never all touched: each page is a fault

static volatile int g_started;

// The crash reports this program has left (kernel/proc/crash_report.c
// names them <name>-<pid>.crash), as a sum of their pids -- a new one
// changes the sum even if an old one was pruned meanwhile, unless the
// two cancel, which a fresh pid never does.
static long reports(int *count) {
    long sum = 0;
    *count = 0;
    DIR *d = opendir("/var/crash");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "exitfault_test-", 15)) continue;
        sum += atol(e->d_name + 15);
        (*count)++;
    }
    closedir(d);
    return sum;
}

static void *toucher(void *arg) {
    (void)arg;
    char *p = mmap(NULL, SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    __atomic_add_fetch(&g_started, 1, __ATOMIC_RELEASE);
    if (p == MAP_FAILED) for (;;) { }
    for (size_t off = 0;; off = (off + 4096) % SPAN) p[off] = 1;
    return 0;
}

static int child(void) {
    pthread_t t;
    for (int i = 0; i < TOUCHERS; i++) pthread_create(&t, NULL, toucher, NULL);
    while (__atomic_load_n(&g_started, __ATOMIC_ACQUIRE) < TOUCHERS) { }
    sys_sleep_ms(2);   // let them fault for a while, then leave mid-touch
    exit(CHILD_CODE);
}

int main(int argc, char **argv) {
    int rounds = 40;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--child")) return child();
        if (!strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = atoi(argv[++i]);
    }
    utest_begin("exitfault_test", "a group exit outruns its threads' page faults",
                UTEST_VERDICT_FILE);

    int before_n, after_n;
    long before = reports(&before_n);
    int reaped = 0, wrong = 0, first_wrong = 0;
    for (int i = 0; i < rounds; i++) {
        int pid = sys_spawn(SELF, "--child", -1);
        int code = -1;
        if (pid <= 0 || sys_waitpid(pid, &code) != pid) { wrong++; continue; }
        if (code == CHILD_CODE) reaped++;
        else if (!wrong++) first_wrong = code;
    }
    utest_checkf(reaped == rounds && wrong == 0,
                 "every child is reaped with the main thread's code: %d of %d, "
                 "%d wrong (first: %d)", reaped, rounds, wrong, first_wrong);
    long after = reports(&after_n);
    utest_checkf(after == before && after_n == before_n,
                 "no thread left a crash report: %d before, %d after", before_n, after_n);
    return utest_end();
}
