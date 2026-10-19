// check.c -- running `w16 --check`, and the short name a program goes by
// (win16.h).
#include "win16/win16.h"

#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"

char *win16_check(const char *path) {
    int fds[2];
    if (sys_pipe(fds) < 0) return 0;
    char *argv[] = { "w16", "--check", (char *)path, 0 };
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.stdout_fd = fds[1];
    o.stderr_fd = SPAWN_FD_LOG;
    int pid = sys_spawn_opts(W16_BIN, &o);
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

char *win16_short_title(const char *d) {
    for (int again = 1; again; ) {
        again = 0;
        static const char *const PREFIX[] = { "Microsoft ", "Windows " };
        for (int i = 0; i < 2; i++) {
            size_t n = strlen(PREFIX[i]);
            if (!strncmp(d, PREFIX[i], n) && d[n]) { d += n; again = 1; }
        }
    }
    char *t = strdup(d);
    if (!t) return 0;
    for (char *c = t; *c; c++)
        if (c > t && c[-1] == ' ' &&
            (!strncmp(c, "(c)", 3) || !strncmp(c, "(C)", 3) || !strncmp(c, "Copyright", 9) ||
             (c[0] == 'v' && c[1] >= '0' && c[1] <= '9'))) {
            c[-1] = 0;
            break;
        }
    return t;
}
