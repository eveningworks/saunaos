// env -- print the environment, or run a program in a changed one.
//
// POSIX's env: NAME=value operands are set first, -i starts from an
// empty environment, and the first operand without '=' is the program,
// run by execvp() so the changed PATH is the one it is looked up on.
// With no program, the environment is printed one NAME=value per line.
//
// The exit codes are POSIX's too: 127 for a program that is not there,
// 126 for one that is but could not be run -- what a shell reports.
#include "base/uargs.h"
#include "base/cmd.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ignore;

static const struct uargs_opt OPTS[] = {
    { "ignore-environment", 'i', 0, "start from an empty environment", &ignore, 0 },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "env",
    .usage = "[-i] [NAME=VALUE]... [PROGRAM [ARG]...]",
    .summary = "Print the environment, or run PROGRAM with NAME=VALUE added to it.\n"
               "Every process starts with what init read from /etc/environment.",
    .opts = OPTS,
    .first_operand_ends_options = 1,
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (ignore) clearenv();

    int i = 0;
    for (; i < a.argc; i++) {
        char *eq = strchr(a.argv[i], '=');
        if (!eq) break;
        if (eq == a.argv[i]) return uargs_error(&PROG, "'%s' has no name", a.argv[i]);
        *eq = '\0';
        if (setenv(a.argv[i], eq + 1, 1) != 0) {
            cmd_fail("env", a.argv[i]);
            return 125;
        }
    }

    if (i == a.argc) {
        extern char **environ;
        for (char **e = environ; e && *e; e++) printf("%s\n", *e);
        return 0;
    }
    // uargs compacts the operands in place and leaves what was after them,
    // so the vector execvp() reads is cut at the last one here (there is
    // room: argv has argc + 1 slots and a.argc <= argc).
    a.argv[a.argc] = 0;
    execvp(a.argv[i], &a.argv[i]);
    int err = errno;
    cmd_fail_err("env", a.argv[i], err);
    return err == ENOENT ? 127 : 126;
}
