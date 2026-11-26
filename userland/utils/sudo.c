// sudo -- run one command as root, through privd (base/upriv.h). NOT
// setuid sudo: no sudoers file, no -u. Once the account has a password
// (docs/users-design.md, D2a) it is asked for HERE, on the terminal, and
// privd checks it; without one privd asks the person -- on the desktop
// when one is running, else this program asks y/N on its terminal. The
// command then runs as this program's own child.
#include "base/uargs.h"
#include "base/uaskpass.h"
#include "base/upriv.h"
#include <pwd.h>
#include "rt/sys.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const struct uargs_prog PROG = {
    "sudo", "COMMAND [ARG]...",
    "Run COMMAND as root, after the person at the machine allows it.\n"
    "With a password set, asks for it on this terminal; without one, asks\n"
    "through privd: on the desktop's prompt, or here when no desktop runs.",
    0, 0,
    "Not setuid: there is no sudoers file. Only an account in the wheel\n"
    "group may ask -- at the machine itself, unless it gives its password.",
    0, 1, 0, 0,
};

static int runnable(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
}

// COMMAND as an absolute path: as given when it names a directory, else
// the first PATH entry holding it -- what privd grants and the kernel
// compares is the path, so it must be the one that runs.
static int resolve(const char *cmd, char *out, size_t cap) {
    if (strchr(cmd, '/')) {
        if (cmd[0] == '/') snprintf(out, cap, "%s", cmd);
        else {
            char cwd[1024];
            if (!getcwd(cwd, sizeof cwd)) return -1;
            snprintf(out, cap, "%s/%s", cwd, cmd);
        }
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

static int ask_here(const char *cmd) {
    if (!isatty(0)) return 0;
    fprintf(stderr, "Run %s as administrator? [y/N] ", cmd);
    fflush(stderr);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) return 0;
    return line[0] == 'y' || line[0] == 'Y';
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc < 1) return uargs_error(&PROG, "missing COMMAND");
    static char path[1024];   // static: main's frame holds the spawn options too
    if (resolve(a.argv[0], path, sizeof path) < 0) {
        fprintf(stderr, "sudo: %s: command not found\n", a.argv[0]);
        return 127;
    }
    char **cargv = calloc((size_t)a.argc + 1, sizeof *cargv);
    if (!cargv) return 1;
    for (int i = 0; i < a.argc; i++) cargv[i] = a.argv[i];

    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.env = environ;
    // A pipe or socket is handed on; a terminal is inherited as it is.
    if (!isatty(0)) o.stdin_fd = 0;
    if (!isatty(1)) o.stdout_fd = 1;

    int pid;
    if (geteuid() == 0) {
        o.argv = cargv;
        o.flags |= SPAWN_ARGV;
        pid = sys_spawn_opts(path, &o);
    } else {
        struct passwd *pw = getpwuid(getuid());
        char me[64] = "";
        if (pw) snprintf(me, sizeof me, "%s", pw->pw_name);
        int has = me[0] ? upriv_has_password(me) : 0;
        if (has == 1 && !isatty(0)) {
            fprintf(stderr, "sudo: a password is needed, and there is no terminal to ask on\n");
            return 1;
        }
        if (has == 1) {
            static char prompt[96], typed[256];   // off the stack: main has path[] too
            snprintf(prompt, sizeof prompt, "[sudo] password for %s: ", me);
            pid = -1;
            for (int i = 0; i < UPRIV_AUTH_TRIES; i++) {
                if (uaskpass(prompt, typed, sizeof typed) < 0) { errno = ECANCELED; break; }
                pid = upriv_spawn_password(path, cargv, &o, typed);
                memset(typed, 0, sizeof typed);
                if (pid >= 0 || errno != EACCES) break;
                if (i + 1 < UPRIV_AUTH_TRIES) fprintf(stderr, "Sorry, try again.\n");
            }
        } else {
            pid = upriv_spawn(path, cargv, &o, 0);
            if (pid < 0 && errno == ENXIO && ask_here(a.argv[0]))
                pid = upriv_spawn(path, cargv, &o, UPRIV_SELF_CONSENT);
        }
    }
    if (pid < 0) {
        int e = errno;
        const char *why = e == EACCES    ? "the password was wrong"
                        : e == EPERM     ? "this account may not run commands as administrator"
                        : e == ECANCELED ? "not allowed"
                        : e == ENXIO     ? "no one to ask: the desktop is not running"
                        : e == ENOENT    ? "privd is not running"
                        : e == ETIMEDOUT ? "no answer"
                        : strerror(e);
        fprintf(stderr, "sudo: %s\n", why);
        return 1;
    }
    int code = 1;
    if (sys_waitpid(pid, &code) < 0) return 1;
    return code;
}
