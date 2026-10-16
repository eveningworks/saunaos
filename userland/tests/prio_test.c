// Nice and the scheduling class from ring 3: who may change what, and
// what a child starts at.
//
// A slot reused by a new process kept its LAST tenant's level, so a
// program started after a driver exited ran at the driver's level
// without asking -- which only shows through a real spawn, so this is a
// program and not a KTEST. (A negative nice reading back as -1, the
// other bug it was written for, needs init to set one now.)
//
// `prio_test child` is the child: it exits with 20 - its nice, the
// same offset the syscall uses, so every level is a positive code.
//
// REALTIME FOR A PROGRAM STARTED BY HAND (INITCTL_REALTIME) is here too:
// the test lists ITSELF in /etc/realtime.d for the length of the run, so
// the refusal before and the grant after differ only by that entry.
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/uinitctl.h"

#include "lib/utest.h"

// What a child spawned now starts at, or -99 if it could not be run.
static int child_level(void) {
    int pid = sys_spawn("/tests/prio_test", "child", -1);
    if (pid <= 0) return -99;
    int code = -1;
    sys_waitpid(pid, &code);
    return code < 0 ? -99 : 20 - code;
}

#define SELF     "/tests/prio_test"
#define RT_ENTRY "/etc/realtime.d/prio_test"

static int write_entry(void) {
    FILE *f = fopen(RT_ENTRY, "w");
    if (!f) return 0;
    fputs("Exec=" SELF "\nCPUSchedulingPolicy=rr\nCPUSchedulingPriority=5\n", f);
    return fclose(f) == 0;
}

// Asks init for realtime, then execs itself as `class`: the exit code is
// the policy the exec'd image started with, or 90+ when a step failed.
static int child_rt_then_exec(void) {
    if (uinitctl_realtime(0, 0, 0, 0, 0) != INITCTL_OK) return 90;
    if (sched_getscheduler(0) != SCHED_RR) return 91;
    char *av[] = { SELF, "class", 0 };
    execv(SELF, av);
    return 92;
}

static void realtime_by_hand(void) {
    unlink(RT_ENTRY);   // a run that died half-way left it behind
    int got_policy = -1, got_prio = -1;
    int r = uinitctl_realtime(0, 0, 0, 0, 0);
    utest_checkf(r == INITCTL_NOT_LISTED, "an unlisted program is refused realtime (got %d)", r);
    utest_check(sched_getscheduler(0) == SCHED_OTHER, "and stays SCHED_OTHER");

    if (!write_entry()) {
        utest_check(0, "could not write " RT_ENTRY);
        return;
    }
    r = uinitctl_realtime(0, SCHED_RR, 6, 0, 0);
    utest_checkf(r == INITCTL_TOO_HIGH, "above the listed priority is refused, not lowered (got %d)", r);
    utest_check(sched_getscheduler(0) == SCHED_OTHER, "and leaves us SCHED_OTHER");
    r = uinitctl_realtime(0, 0, 0, &got_policy, &got_prio);
    utest_checkf(r == INITCTL_OK && got_policy == SCHED_RR && got_prio == 5,
                 "listed, the listed class is granted (got %d: %d %d)", r, got_policy, got_prio);
    struct sched_param sp = { 0 };
    utest_check(sched_getscheduler(0) == SCHED_RR && sched_getparam(0, &sp) == 0 &&
                sp.sched_priority == 5, "and reads back SCHED_RR 5");
    sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);

    // What init granted was a PROGRAM; an exec replaces it.
    int pid = sys_spawn(SELF, "rtexec", -1), code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    utest_checkf(code == SCHED_OTHER, "an exec ends a granted realtime class (got %d)", code);
    unlink(RT_ENTRY);
}

// SPAWN_STOPPED: the child runs nothing until a SIGCONT.
static void spawn_stopped(void) {
    char *av[] = { SELF, "child", 0 };
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = av;
    o.flags = SPAWN_STOPPED;
    int pid = sys_spawn_opts(SELF, &o), code = -1;
    utest_checkf(pid > 0, "SPAWN_STOPPED spawns (got %d)", pid);
    if (pid <= 0) return;
    sys_sleep_ms(300);
    utest_check(sys_waitpid_nohang(pid, &code) != pid, "a SPAWN_STOPPED child has not run after 300 ms");
    sys_kill(pid, SIGCONT);
    sys_waitpid(pid, &code);
    int want = 20 - getpriority(PRIO_PROCESS, 0);
    utest_checkf(code == want, "and runs to completion once continued (got %d, want %d)", code, want);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "rtexec")) return child_rt_then_exec();
    if (argc > 1 && !strcmp(argv[1], "class")) return sched_getscheduler(0);
    if (argc > 1 && !strcmp(argv[1], "child")) {
        int mine = getpriority(PRIO_PROCESS, 0);
        // The leak's victim: a child that changes itself and exits leaves
        // its slot at that level for whoever gets the slot next.
        if (argc > 2 && !strcmp(argv[2], "lower")) setpriority(PRIO_PROCESS, 0, 12);
        return 20 - mine;
    }

    utest_begin("prio_test", "nice and the scheduling class, and what a child inherits",
                UTEST_VERDICT_FILE);

    int start = getpriority(PRIO_PROCESS, 0);
    utest_checkf(start == 0, "a test starts at nice 0 (got %d)", start);

    // ONLY INIT MAY MAKE ANYTHING MORE IMPORTANT. Every refusal below is
    // checked by its errno, and the state after it read back, since a
    // call that failed half-way is the bug this would miss.
    errno = 0;
    utest_check(setpriority(PRIO_PROCESS, 0, -5) == -1 && errno == EPERM,
                "lowering our own nice is refused with EPERM");
    utest_check(getpriority(PRIO_PROCESS, 0) == 0, "and left it alone");
    errno = 0;
    utest_check(setpriority(PRIO_PROCESS, 1, 5) == -1 && errno == EPERM,
                "renicing another process (init) is refused with EPERM");
    struct sched_param sp = { 10 };
    errno = 0;
    utest_check(sched_setscheduler(0, SCHED_FIFO, &sp) == -1 && errno == EPERM,
                "asking for SCHED_FIFO is refused with EPERM");
    utest_check(sched_getscheduler(0) == SCHED_OTHER, "and left us SCHED_OTHER");
    sp.sched_priority = 0;
    utest_check(sched_setscheduler(0, SCHED_OTHER, &sp) == 0,
                "dropping ourselves to SCHED_OTHER is allowed");
    utest_check(setpriority(PRIO_PROCESS, 0, 25) == -1, "a nice outside -20..19 is refused");

    // The reuse: a child raises itself to 12 and exits, then the next
    // spawn -- which takes the lowest free slot, very likely the same
    // one -- must start at OUR level, not the dead child's.
    int pid = sys_spawn("/tests/prio_test", "child lower", -1);
    int code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    int kid = child_level();
    utest_checkf(kid == 0, "a reused slot does not keep its last tenant's level (got %d)", kid);

    // Anyone may make itself LESS important, and its children follow.
    utest_checkf(setpriority(PRIO_PROCESS, 0, 5) == 0, "raising our own nice to 5");
    int got = getpriority(PRIO_PROCESS, 0);
    utest_checkf(got == 5, "it reads back (got %d)", got);
    kid = child_level();
    utest_checkf(kid == 5, "a child starts at its parent's nice (got %d)", kid);
    errno = 0;
    utest_check(setpriority(PRIO_PROCESS, 0, 0) == -1 && errno == EPERM,
                "and cannot take it back");

    // Reading is not changing: anyone may ask about anyone.
    sp.sched_priority = -1;
    utest_check(sched_getparam(1, &sp) == 0 && sp.sched_priority == 0,
                "init itself is SCHED_OTHER, read through sched_getparam");

    realtime_by_hand();
    spawn_stopped();
    return utest_end();
}
