// ucapture.c -- run a program and take its standard output (ucapture.h).
#include "lib/ucapture.h"

#include <stdlib.h>
#include "rt/sys.h"

char *ucapture(const char *bin, char *const argv[]) {
    int fds[2];
    if (sys_pipe(fds) < 0) return 0;
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = (char **)argv;
    o.stdout_fd = fds[1];
    o.stderr_fd = SPAWN_FD_LOG;
    int pid = sys_spawn_opts(bin, &o);
    sys_close(fds[1]);
    if (pid < 0) { sys_close(fds[0]); return 0; }
    char *out = 0;
    size_t n = 0, cap = 0;
    for (;;) {
        if (cap - n < 4096) {
            char *g = realloc(out, cap = cap ? cap * 2 : 16384);
            if (!g) { free(out); out = 0; break; }
            out = g;
        }
        int got = sys_read(fds[0], out + n, (int)(cap - n - 1));
        if (got <= 0) break;
        n += (size_t)got;
    }
    sys_close(fds[0]);
    sys_waitpid(pid, 0);
    if (out) out[n] = 0;
    return out;
}
