// A known workload for the kernel event trace's test
// (tools/tests/sweep/ktrace_test.py): what it does is what the trace must
// then show, and nothing it does is heavy enough to fill a ring.
//
//     ktrace_load [CALLS]     (default 1000)
//
//   - CALLS getpids, each of whose exits must return this pid;
//   - a forked child and 50 one-byte round trips over two pipes, so the
//     two hand the core to each other and each is woken by the other;
//   - 5 spawns of /bin/hello, each waited for.
// SPAWN it, not `run`: it forks. It ends with "ktrace_load: done PID" on
// stderr, which reaches dmesg.
#include <stdint.h>
#include "rt/sys.h"

#define TRIPS  50
#define SPAWNS 5

static void say(const char *s) {
    int n = 0;
    while (s[n]) n++;
    sys_write(2, s, (uint64_t)n);
}

static void say_num(const char *what, long v) {
    char b[64];
    int n = 0;
    for (const char *p = what; *p && n < 40; p++) b[n++] = *p;
    char d[24];
    int k = 0;
    if (v == 0) d[k++] = '0';
    while (v > 0 && k < 20) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) b[n++] = d[--k];
    b[n++] = '\n';
    b[n] = 0;
    say(b);
}

int main(int argc, char **argv) {
    long calls = 1000;
    if (argc > 1) {
        calls = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) calls = calls * 10 + (*p - '0');
        if (calls < 1) calls = 1000;
    }
    int me = sys_getpid();
    for (long i = 0; i < calls; i++)
        if (sys_getpid() != me) { say("ktrace_load: getpid disagreed\n"); return 1; }

    int to[2], from[2];
    if (sys_pipe(to) < 0 || sys_pipe(from) < 0) { say("ktrace_load: pipe failed\n"); return 1; }
    int pid = sys_fork();
    if (pid < 0) { say("ktrace_load: fork failed -- SPAWN this, not run\n"); return 1; }
    if (pid == 0) {
        sys_close(to[1]);
        sys_close(from[0]);
        uint8_t b;
        while (sys_read(to[0], &b, 1) == 1) sys_write(from[1], &b, 1);
        sys_exit(0);
    }
    sys_close(to[0]);
    sys_close(from[1]);
    for (int i = 0; i < TRIPS; i++) {
        uint8_t b = (uint8_t)i, back = 0;
        if (sys_write(to[1], &b, 1) != 1 || sys_read(from[0], &back, 1) != 1 || back != b) {
            say("ktrace_load: the round trip broke\n");
            return 1;
        }
    }
    sys_close(to[1]);
    sys_waitpid(pid, 0);
    say_num("ktrace_load: partner ", pid);

    for (int i = 0; i < SPAWNS; i++) {
        int c = sys_spawn("/bin/hello", 0, -1);
        if (c < 0) { say("ktrace_load: cannot spawn /bin/hello\n"); return 1; }
        sys_waitpid(c, 0);
    }
    say_num("ktrace_load: done ", me);
    return 0;
}
