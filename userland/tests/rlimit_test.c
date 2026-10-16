// RLIMIT_DATA from ring 3: what it counts (the heap and private writable
// mappings, not read-only ones), who may raise what, and that a child
// starts under its parent's limit and is held to it.
//
// `rlimit_test child` is the child: it exits with its limit in MiB if a
// malloc() past that limit came back NULL, and with 255 if it did not.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include "rt/sys.h"

#include "lib/utest.h"

#define MIB (1024ULL * 1024)

static int child_main(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_DATA, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY) return 254;
    if (malloc((size_t)(rl.rlim_cur + 8 * MIB))) return 255;
    return (int)(rl.rlim_cur / MIB);
}

static int run_child(void) {
    int pid = sys_spawn("/tests/rlimit_test", "child", -1);
    if (pid <= 0) return -1;
    int code = -1;
    sys_waitpid(pid, &code);
    return code;
}

static void *map(size_t len, int prot) {
    return mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return child_main();

    utest_begin("rlimit_test", "RLIMIT_DATA: what it counts, who may raise it, and inheritance",
                UTEST_VERDICT_FILE);

    struct rlimit rl;
    utest_check(getrlimit(RLIMIT_DATA, &rl) == 0 && rl.rlim_cur == RLIM_INFINITY &&
                rl.rlim_max == RLIM_INFINITY, "a test starts with no data limit");
    utest_check(malloc(96 * MIB) != NULL, "...so 96 MiB is given before one is set");

    struct rlimit bad = { 64 * MIB, 32 * MIB };
    errno = 0;
    utest_check(setrlimit(RLIMIT_DATA, &bad) == -1 && errno == EINVAL,
                "a soft limit above the hard one is EINVAL");

    // The 96 MiB above is still the heap's: the break never comes down,
    // so the limit is set above what is in use, and the room is measured
    // from there.
    struct rlimit lim = { 160 * MIB, 256 * MIB };
    utest_check(setrlimit(RLIMIT_DATA, &lim) == 0, "setting 160 MiB, at most 256");
    utest_check(getrlimit(RLIMIT_DATA, &rl) == 0 && rl.rlim_cur == 160 * MIB &&
                rl.rlim_max == 256 * MIB, "...reads back");

    utest_check(malloc(96 * MIB) == NULL, "a malloc() past it is NULL, not the machine's last frame");
    void *small = malloc(16 * MIB);
    utest_check(small != NULL, "one that fits is still given");

    errno = 0;
    void *big = map(64 * MIB, PROT_READ | PROT_WRITE);
    utest_check(big == MAP_FAILED && errno == ENOMEM,
                "a private writable mmap() past it is ENOMEM -- it counts as data");
    void *ro = map(64 * MIB, PROT_READ);
    utest_check(ro != MAP_FAILED, "a read-only one is not data, and is given");
    if (ro != MAP_FAILED) munmap(ro, 64 * MIB);

    void *rw = map(24 * MIB, PROT_READ | PROT_WRITE);
    utest_check(rw != MAP_FAILED, "a writable mapping that fits is given");
    utest_check(malloc(40 * MIB) == NULL, "...and it is counted against the heap's room");
    if (rw != MAP_FAILED) munmap(rw, 24 * MIB);
    void *after = malloc(40 * MIB);
    utest_check(after != NULL, "unmapped, its room comes back");

    // A child starts under our limit and is held to it, not just told it.
    int code = run_child();
    utest_checkf(code == 160, "a spawned child inherits the limit and its malloc past it is NULL "
                 "(exit %d; 255 = it was given)", code);

    // Anyone may lower and raise the soft limit within the hard one;
    // only init may raise the hard one.
    lim.rlim_cur = 256 * MIB;
    utest_check(setrlimit(RLIMIT_DATA, &lim) == 0, "raising the soft limit to the hard one is allowed");
    struct rlimit up = { 256 * MIB, 512 * MIB };
    errno = 0;
    utest_check(setrlimit(RLIMIT_DATA, &up) == -1 && errno == EPERM,
                "raising the hard limit is refused with EPERM");
    utest_check(getrlimit(RLIMIT_DATA, &rl) == 0 && rl.rlim_max == 256 * MIB,
                "...and left it where it was");
    code = run_child();
    utest_checkf(code == 256, "a child spawned after the change gets the new limit (exit %d)", code);

    return utest_end();
}
