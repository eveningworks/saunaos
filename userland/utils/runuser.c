// runuser -- run one command as another account, util-linux's runuser:
// for ROOT only (the spawn is SPAWN_CREDS), asks nothing, and the child
// keeps this program's folder and terminal. What the GUI tests make a
// fixture with, so it belongs to the account the app runs as
// (the debug console's `as`, kernel/core/debug_console.c).
#include "base/uaccount.h"
#include "base/uargs.h"
#include "rt/sys.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *g_user;

static const struct uargs_opt OPTS[] = {
    { "user", 'u', "USER", "the account to run COMMAND as", 0, &g_user },
    { 0 },
};

static const struct uargs_prog PROG = {
    "runuser", "-u USER COMMAND [ARG]...",
    "Run COMMAND as USER: its uid, its groups from /etc/group, and HOME,\n"
    "USER, LOGNAME and SHELL from its line in /etc/passwd.",
    OPTS, 0,
    "Root only, and asks for nothing -- an account runs a command as\n"
    "administrator with sudo instead.",
    0, 1, 0, 0,
};

static int runnable(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
}

// COMMAND as a path: as given when it has a '/', else the first PATH
// entry holding it.
static int resolve(const char *cmd, char *out, size_t cap) {
    if (strchr(cmd, '/')) {
        snprintf(out, cap, "%s", cmd);
        return runnable(out) ? 0 : -1;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";
    for (const char *s = path; *s; ) {
        const char *e = strchr(s, ':');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n) {
            snprintf(out, cap, "%.*s/%s", (int)n, s, cmd);
            if (runnable(out)) return 0;
        }
        if (!e) break;
        s = e + 1;
    }
    return -1;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (!g_user) return uargs_error(&PROG, "missing -u USER");
    if (a.argc < 1) return uargs_error(&PROG, "missing COMMAND");
    if (geteuid() != 0) {
        fprintf(stderr, "runuser: may only be used by root\n");
        return 1;
    }
    char path[1024];
    if (resolve(a.argv[0], path, sizeof path) < 0) {
        fprintf(stderr, "runuser: %s: command not found\n", a.argv[0]);
        return 127;
    }
    char **cargv = calloc((size_t)a.argc + 1, sizeof *cargv);
    if (!cargv) return 1;
    for (int i = 0; i < a.argc; i++) cargv[i] = a.argv[i];

    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    struct uaccount_spawn acct;
    if (uaccount_spawn_init(&acct, &o, g_user, environ) < 0) {
        fprintf(stderr, errno == ENOENT ? "runuser: user %s does not exist\n"
                                        : "runuser: out of memory\n", g_user);
        return 1;
    }
    o.argv = cargv;
    o.flags |= SPAWN_ARGV;
    // A pipe or socket is handed on; a terminal is inherited as it is.
    if (!isatty(0)) o.stdin_fd = 0;
    if (!isatty(1)) o.stdout_fd = 1;
    int pid = sys_spawn_opts(path, &o);
    int e = sys_errno();
    uaccount_spawn_release(&acct);
    if (pid < 0) {
        fprintf(stderr, "runuser: %s: %s\n", a.argv[0], sys_strerror(e));
        return 1;
    }
    int code = 1;
    if (sys_waitpid(pid, &code) < 0) return 1;
    return code;
}
