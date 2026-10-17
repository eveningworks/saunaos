// O_APPEND from two processes at once: every record of both lands, none
// on top of another. The end is found by the backend under the lock it
// writes under (FS_OFFSET_END); a size read first and written at later
// lets two appenders take one offset, and one record overwrites the other.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "tmppath.h"
#include "lib/utmppath.h"
#include "lib/utest.h"

#define RECS 1500
#define REC  8          // "A:0000\n" plus one pad byte, fixed width

static char g_path[96];

static int worker(char tag) {
    int fd = sys_open(g_path, SYS_O_WRITE | SYS_O_APPEND);
    if (fd < 0) return 2;
    char rec[REC + 1];
    for (int i = 0; i < RECS; i++) {
        snprintf(rec, sizeof rec, "%c:%04d.\n", tag, i);
        if (sys_write(fd, rec, REC) != REC) { sys_close(fd); return 3; }
    }
    sys_close(fd);
    return 0;
}

static char g_file[2 * RECS * REC + 64];

int main(int argc, char **argv) {
    strncpy(g_path, utest_path(TMP_PERSISTENT, "append_test.txt"), sizeof g_path - 1);
    if (argc > 2 && !strcmp(argv[1], "worker")) return worker(argv[2][0]);

    utest_begin("append_test", "two appenders never overwrite each other", UTEST_VERDICT_FILE);
    sys_unlink(g_path);
    int fd = sys_open(g_path, SYS_O_WRITE | SYS_O_CREAT);
    utest_check(fd >= 0, "created the file");
    if (fd >= 0) sys_close(fd);

    int a = sys_spawn("/tests/append_test", "worker A", -1);
    int b = sys_spawn("/tests/append_test", "worker B", -1);
    int ca = -1, cb = -1;
    if (a > 0) sys_waitpid(a, &ca);
    if (b > 0) sys_waitpid(b, &cb);
    utest_checkf(ca == 0 && cb == 0, "both workers wrote every record (exit %d, %d)", ca, cb);

    fd = sys_open(g_path, 0);
    long n = 0;
    if (fd >= 0) {
        long r;
        while (n < (long)sizeof g_file && (r = sys_read(fd, g_file + n, sizeof g_file - (size_t)n)) > 0) n += r;
        sys_close(fd);
    }
    utest_checkf(n == 2L * RECS * REC, "the file is every record long (%ld of %d bytes)", n, 2 * RECS * REC);

    // Each worker's records, whole and in its own order.
    int next[2] = {0, 0}, bad = 0;
    for (long at = 0; at + REC <= n; at += REC) {
        const char *p = g_file + at;
        int w = p[0] == 'A' ? 0 : p[0] == 'B' ? 1 : -1;
        if (w < 0 || p[1] != ':' || p[7] != '\n' || atoi(p + 2) != next[w]) { bad++; continue; }
        next[w]++;
    }
    utest_checkf(bad == 0 && next[0] == RECS && next[1] == RECS,
                 "no record lost or torn (%d bad, A %d, B %d)", bad, next[0], next[1]);
    sys_unlink(g_path);
    return utest_end();
}
