// MAPPING SIZES ARE BOUNDED, AND THE DATA LIMIT COUNTS WHAT IS REALLY MAPPED.
//
//   readonly  a private anonymous region is charged to RLIMIT_DATA however
//             it is protected -- every page of one gets its own frame on
//             first touch (no shared zero page), so a PROT_READ region
//             was memory past the limit (kernel/mm/mmap.c, is_data())
//   huge      an mmap longer than the arena is refused, not wrapped into
//             a region nothing can find
//   wrap      an munmap whose length wraps past 2^64 is refused
//   shm       an shm object is charged to its creator's data limit: its
//             frames are allocated at creation, and a limited process
//             could otherwise pin memory without bound (kernel/mm/shm.c)
//   shmfirst  ...whichever comes first: an object created before an
//             anonymous map or a heap grow counts against them too
//
// Each probe runs in a child, so a broken unmap takes only it down.
//
// It must be SPAWNED, not `run` -- it has children.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/mmlimit_test"
#define MiB (1024ul * 1024ul)

static int probe_readonly(void) {
    struct rlimit lim = { 8 * MiB, 8 * MiB };
    if (setrlimit(RLIMIT_DATA, &lim) != 0) return 2;
    void *w = mmap(NULL, 16 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (w != MAP_FAILED) return 3;                 // the control: writable is refused
    void *r = mmap(NULL, 64 * MiB, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return r == MAP_FAILED ? 0 : 1;
}

static int probe_huge(void) {
    void *p = mmap(NULL, (size_t)0xFFFFFFFFFFFFF000ull, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : 1;
}

static int probe_wrap(void) {
    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 2;
    p[0] = 'x';
    if (munmap(p, (size_t)0xFFFFFFFFFFFFF000ull) == 0) return 1;
    return p[0] == 'x' ? 0 : 3;                    // and the page is still there
}

static int probe_shm(void) {
    struct rlimit lim = { 8 * MiB, 8 * MiB };
    if (setrlimit(RLIMIT_DATA, &lim) != 0) return 2;
    int small = sys_shm_open("mmlimit-small", 1 * MiB, SHM_CREATE);   // the control
    if (small < 0) return 3;
    int big = sys_shm_open("mmlimit-big", 16 * MiB, SHM_CREATE);
    return big < 0 ? 0 : 1;
}

static int probe_shmfirst(void) {
    struct rlimit lim = { 8 * MiB, 8 * MiB };
    if (setrlimit(RLIMIT_DATA, &lim) != 0) return 2;
    if (sys_shm_open("mmlimit-first", 7 * MiB, SHM_CREATE) < 0) return 3;   // the control
    void *p = mmap(NULL, 7 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) return 1;
    return sys_sbrk((int64_t)(7 * MiB)) == (void *)-1 ? 0 : 4;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--probe")) {
        if (!strcmp(argv[2], "readonly")) return probe_readonly();
        if (!strcmp(argv[2], "huge"))     return probe_huge();
        if (!strcmp(argv[2], "wrap"))     return probe_wrap();
        if (!strcmp(argv[2], "shm"))      return probe_shm();
        if (!strcmp(argv[2], "shmfirst")) return probe_shmfirst();
        return 9;
    }
    utest_begin("mmlimit_test", "mapping sizes are bounded, the data limit counts them",
                UTEST_VERDICT_FILE);
    static const struct { const char *name, *what; } probes[] = {
        { "readonly", "a read-only anonymous map is charged to the data limit" },
        { "huge",     "an mmap longer than the arena is refused" },
        { "wrap",     "an munmap whose length wraps is refused, the page kept" },
        { "shm",      "an shm object counts against its creator's data limit" },
        { "shmfirst", "...made first, it counts against a later map and heap grow" },
    };
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
        char args[32];
        snprintf(args, sizeof args, "--probe %s", probes[i].name);
        int pid = sys_spawn(SELF, args, -1);
        int code = -1;
        if (pid <= 0 || sys_waitpid(pid, &code) != pid) code = -1;
        utest_checkf(code == 0, "%s (child exit %d)", probes[i].what, code);
    }
    return utest_end();
}
