// THREADS FAULTING THE SAME PAGE AT ONCE ALL GO ON.
//
// Under smp=run two threads can take a not-present fault on one page on
// two cores together. The first maps it; the second reaches the handler
// after it, finds the page present, and must retry the access -- it was
// treated as a protection fault and killed the process (telnetd on the
// ASUS, both its threads in exit()'s fflush, 2026-11-20).
//
// So: a child releases its threads onto one fresh page at a time, reading
// and writing, a few thousand pages; the parent wants it reaped with 0
// and no crash report from it.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: any boot with one core.**
//
// It must be SPAWNED, not `run` -- it has a child and threads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <dirent.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/samepage_test"
#define THREADS 2   // + the main thread: three spinners, one per AP of a 4-core guest
#define PAGES 2048

static volatile char *g_page;
static volatile unsigned g_round, g_done;

static void *toucher(void *arg) {
    unsigned seen = 0, id = (unsigned)(uintptr_t)arg;
    for (;;) {
        unsigned r;
        while ((r = __atomic_load_n(&g_round, __ATOMIC_ACQUIRE)) == seen) { }
        if (r == ~0u) return 0;
        seen = r;
        volatile char *p = g_page;
        if ((r + id) & 1) (void)p[0]; else p[id] = 1;   // reads and writes, together
        __atomic_add_fetch(&g_done, 1, __ATOMIC_ACQ_REL);
    }
}

static int child(void) {
    char *base = mmap(NULL, (size_t)PAGES * 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return 2;
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, toucher, (void *)(uintptr_t)i);
    for (unsigned r = 1; r <= PAGES; r++) {
        g_page = base + (size_t)(r - 1) * 4096;
        __atomic_store_n(&g_done, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_round, r, __ATOMIC_RELEASE);
        while (__atomic_load_n(&g_done, __ATOMIC_ACQUIRE) < THREADS) { }
    }
    __atomic_store_n(&g_round, ~0u, __ATOMIC_RELEASE);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    return 0;
}

static int reports(void) {
    int n = 0;
    DIR *d = opendir("/var/crash");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) n += !strncmp(e->d_name, "samepage_test-", 14);
    closedir(d);
    return n;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) return child();
    utest_begin("samepage_test", "threads faulting one page at once all go on",
                UTEST_VERDICT_FILE);
    int before = reports();
    int pid = sys_spawn(SELF, "--child", -1);
    int code = -1;
    int waited = pid > 0 && sys_waitpid(pid, &code) == pid;
    utest_checkf(waited && code == 0, "the child touched %d pages with %d threads and exited 0 "
                 "(it exited %d)", PAGES, THREADS, code);
    utest_checkf(reports() == before, "no crash report: %d before, %d after", before, reports());
    return utest_end();
}
