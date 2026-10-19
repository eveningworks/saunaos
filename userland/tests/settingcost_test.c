// WHAT A SETTING COSTS, COUNTED IN SYSCALLS -- not timed.
//
// `config set` once resolved one qualified name by walking the whole
// merged registry, twice, reading every declaration in /etc/settings.d
// on the way: ~3000 syscalls a call, seconds under load, and a GUI test
// that timed out on it intermittently. A time wobbles with whatever else
// the machine is doing; the syscall count of one command does not, so
// this asserts CEILINGS -- several times today's count, far below the
// walk's. `strace -c` does the counting (lib/utrace.h).
//
// The setting it writes is put back (unset) on every path.
//
// Prints one line per check and exits with the number of failures.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/utest.h"

#define COUNTS "/tmp/settingcost.counts"

// Every syscall `config ARGS` made, from strace's `total` row; -1 when
// it could not be run or counted.
static int syscalls_of(const char *args) {
    char line[200];
    snprintf(line, sizeof line, "-c -o " COUNTS " /bin/config %s", args);
    // stdout inherited: a spawn takes a pipe or a socket there, not a
    // file, and config's one line in this test's output is harmless.
    int pid = sys_spawn("/bin/strace", line, -1);
    if (pid <= 0) return -1;
    int status = 0;
    sys_waitpid(pid, &status);

    static char buf[2048];   // static: a ring-3 frame is budgeted at 2 KiB
    int fd = open(COUNTS, O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *t = strstr(buf, " total");
    if (!t) return -1;
    while (t > buf && t[-1] != '\n') t--;   // the row's start: "  2968     65 total"
    return atoi(t);
}

static void ceiling(const char *args, int max, int was) {
    int n = syscalls_of(args);
    utest_checkf(n > 0 && n <= max, "`config %s`: %d syscalls (ceiling %d; the walk made %d)",
                 args, n, max, was);
}

int main(void) {
    utest_begin("settingcost_test", "one setting by name costs a handful of syscalls, not the registry",
                UTEST_VERDICT_FILE);
    // A declared setting (one file) and a kernel one; a leaf get; unset.
    ceiling("set desktop.start.power restart", 150, 2968);
    ceiling("get desktop.start.power", 600, 2144);
    ceiling("unset desktop.start.power", 150, 784);
    ceiling("get system.font.size", 600, 2144);
    unlink(COUNTS);
    return utest_end();
}
