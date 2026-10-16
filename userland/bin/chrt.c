// chrt -- show a process's scheduling class, or ask init for realtime
// for a program started by hand: util-linux's chrt, sized down.
//
// IT ASKS, IT NEVER SETS. Only init may grant RT (abi/syscall_abi.h,
// SYS_SCHED_SETSCHEDULER), and init grants it only to a PROGRAM that a
// service descriptor or /etc/realtime.d lists, at most at the listed
// priority (lib/uinitctl.h, INITCTL_REALTIME) -- rtkit's shape.
//
// THE LAUNCH FORM SPAWNS THE PROGRAM STOPPED (SPAWN_STOPPED), asks, and
// continues it only once init has answered: a refusal kills it before
// its first instruction, as Linux's chrt fails before its exec.
//
// Exit status: the program's own in the launch form; otherwise 0, 1 when
// init refused or could not be asked, 2 a usage error.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "lib/uargs.h"
#include "lib/uinitctl.h"
#include "lib/upath.h"

#define MAX_ARGV 64

static int g_fifo, g_rr, g_pid, g_listed;

static const struct uargs_opt OPTS[] = {
    { "fifo",   'f', 0, "ask for SCHED_FIFO", &g_fifo, 0 },
    { "rr",     'r', 0, "ask for SCHED_RR", &g_rr, 0 },
    { "pid",    'p', 0, "act on the running process PID instead of starting PROGRAM", &g_pid, 0 },
    { "listed", 'l', 0, "ask for exactly what the program's listing gives; no PRIO", &g_listed, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "chrt",
    .usage = "[-f|-r] PRIO PROGRAM [ARG]...\n"
             "-l PROGRAM [ARG]...\n"
             "[-f|-r] -p PRIO PID\n"
             "-l -p PID\n"
             "-p PID",
    .summary = "Run PROGRAM realtime, or make the running process PID realtime, by asking\n"
               "init -- which grants it only to a program a service descriptor or\n"
               "/etc/realtime.d lists, at no more than the listed priority. With -p and\n"
               "only a PID, print that process's scheduling class.",
    .opts = OPTS,
    .notes = "Without -f or -r the policy is the one the listing gives. PROGRAM starts\n"
             "only once init has answered, so a refused one never runs.",
    .first_operand_ends_options = 1,
};

static const char *policy_name(int policy) {
    return policy == SCHED_FIFO ? "SCHED_FIFO" : policy == SCHED_RR ? "SCHED_RR" : "SCHED_OTHER";
}

// A whole decimal number in lo..hi, or -1.
static int number(const char *s, int lo, int hi) {
    char *end = 0;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < lo || v > hi) return -1;
    return (int)v;
}

static int show(int pid) {
    struct sched_param sp = { 0 };
    int policy = sched_getscheduler(pid);
    if (policy < 0 || sched_getparam(pid, &sp) != 0) {
        fprintf(stderr, "chrt: no process %d\n", pid);
        return 1;
    }
    printf("pid %d's current scheduling policy: %s\n", pid, policy_name(policy));
    printf("pid %d's current scheduling priority: %d\n", pid, sp.sched_priority);
    return 0;
}

// Asks init for `pid`; `what` names it in a refusal. 0 when granted.
static int ask(int pid, int prio, const char *what) {
    int policy = g_fifo > g_rr ? SCHED_FIFO : g_rr ? SCHED_RR : 0;
    int r = uinitctl_realtime(pid, policy, prio, 0, 0);
    if (r == INITCTL_OK) return 0;
    if (r < 0)
        fprintf(stderr, "chrt: init did not answer\n");
    else if (r == INITCTL_NO_SUCH)
        fprintf(stderr, "chrt: no process %d\n", pid);
    else if (r == INITCTL_NOT_LISTED)
        fprintf(stderr, "chrt: %s may not run realtime -- no service or "
                        "/etc/realtime.d entry lists it\n", what);
    else if (r == INITCTL_TOO_HIGH)
        fprintf(stderr, "chrt: priority %d is above what %s is allowed\n", prio, what);
    else
        fprintf(stderr, "chrt: init refused realtime for %s\n", what);
    return 1;
}

static int launch(int prio, int argc, char **argv) {
    char path[UPATH_MAX];
    if (upath_find_program(argv[0], path, sizeof path) <= 0) {
        fprintf(stderr, "chrt: %s: no such program\n", argv[0]);
        return 1;
    }
    char *cargv[MAX_ARGV + 1];
    int n = argc < MAX_ARGV ? argc : MAX_ARGV;
    cargv[0] = path;
    for (int i = 1; i < n; i++) cargv[i] = argv[i];
    cargv[n] = 0;

    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = cargv;
    o.env = environ;
    o.flags = SPAWN_STOPPED;
    int pid = sys_spawn_opts(path, &o);
    if (pid <= 0) {
        fprintf(stderr, "chrt: %s: %s\n", path, sys_strerror(sys_errno()));
        return 1;
    }
    int code = 0;
    if (ask(pid, prio, path)) {
        sys_kill(pid, SIGKILL);
        sys_waitpid(pid, &code);
        return 1;
    }
    sys_kill(pid, SIGCONT);
    sys_waitpid(pid, &code);
    return code;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (g_fifo && g_rr) return uargs_error(&PROG, "-f and -r are two different policies");
    if (g_listed && (g_fifo || g_rr)) return uargs_error(&PROG, "-l asks for the listed policy; drop -f/-r");

    int at = 0, prio = 0;
    if (g_pid) {
        // -p PID shows; -p PRIO PID and -l -p PID ask.
        int want = g_listed ? 1 : a.argc == 1 && !g_fifo && !g_rr ? 1 : 2;
        if (a.argc != want) return uargs_error(&PROG, "-p takes %s", want == 1 ? "a PID" : "PRIO PID");
        if (want == 2 && (prio = number(a.argv[at++], SCHED_RT_PRIO_MIN, SCHED_RT_PRIO_MAX)) < 0)
            return uargs_error(&PROG, "PRIO is 1..99, not '%s'", a.argv[0]);
        int pid = number(a.argv[at], 1, 0x7fffffff);
        if (pid < 0) return uargs_error(&PROG, "'%s' is not a PID", a.argv[at]);
        if (want == 1 && !g_listed) return show(pid);
        char what[32];
        snprintf(what, sizeof what, "pid %d's program", pid);
        return ask(pid, prio, what);
    }

    if (!g_listed) {
        if (a.argc < 1) return uargs_error(&PROG, "a PRIO and a PROGRAM to run");
        if ((prio = number(a.argv[at++], SCHED_RT_PRIO_MIN, SCHED_RT_PRIO_MAX)) < 0)
            return uargs_error(&PROG, "PRIO is 1..99, not '%s'", a.argv[0]);
    }
    if (a.argc <= at) return uargs_error(&PROG, "a PROGRAM to run");
    return launch(prio, a.argc - at, a.argv + at);
}
