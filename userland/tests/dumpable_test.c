// SYS_PRCTL's PR_*_DUMPABLE: a process that asks for no crash report
// dies of its fault with no QUERY_CRASH record (what the desktop's
// "closed unexpectedly" card reads) and no /var/crash file. Fork copies
// the setting; exec starts again at 1.
//
// NO LOUD CHILD BESIDE IT, though one would show the record check can
// see a crash: its card would come up on every run, on a machine someone
// is watching -- what this exists to stop. faulttest_run.py proves an
// ordinary crash IS recorded (crash_test), and this test went red with
// crash_report.c's check removed when it was written.
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>      // fork, _exit
#include <sys/prctl.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "query_abi.h"
#include "signal_abi.h"  // SIGNAL_EXIT_BASE, SIGSEGV
#include "base/utest.h"

// The crash record for `pid`, if the kernel noted one.
static int crash_noted(int pid) {
    struct query_crash c;
    for (unsigned i = 0; sys_query_record(QUERY_CRASH, i, &c, sizeof c) > 0; i++)
        if (c.pid == pid) return 1;
    return 0;
}

// A child that asks for no report, then faults at 0x1000 (crash_test's
// address): its pid, its exit code in *code.
static int quiet_faulting_child(int *code) {
    int pid = fork();
    if (pid == 0) {
        prctl(PR_SET_DUMPABLE, 0);
        *(volatile int *)0x1000 = 1;
        _exit(0);
    }
    *code = -1;
    if (pid > 0) sys_waitpid(pid, code);
    return pid;
}

// The exit code of a child that runs `fn`.
static int child_exit(int (*fn)(void)) {
    int pid = fork();
    if (pid == 0) _exit(fn());
    int code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    return code;
}

static int set_then_get(void) {
    prctl(PR_SET_DUMPABLE, 0);
    return prctl(PR_GET_DUMPABLE);
}

// A grandchild's reading, after its parent set 0.
static int fork_get(void) {
    int pid = fork();
    if (pid == 0) _exit(prctl(PR_GET_DUMPABLE));
    int code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    return code;
}

static int get_in_grandchild(void) {
    prctl(PR_SET_DUMPABLE, 0);
    return fork_get();
}

static int exec_get(void) {
    prctl(PR_SET_DUMPABLE, 0);
    char *argv[] = { "/tests/dumpable_test", "get", NULL };
    char *envp[] = { NULL };
    sys_execve("/tests/dumpable_test", argv, envp);
    return 99;   // the exec failed
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == 'g') return prctl(PR_GET_DUMPABLE);   // exec_get()'s probe

    utest_begin("dumpable_test", "PR_SET_DUMPABLE: a crash that asked for no report", UTEST_VERDICT_FILE);
    const int segv = SIGNAL_EXIT_BASE + SIGSEGV;

    utest_check(prctl(PR_GET_DUMPABLE) == 1, "a process starts dumpable");
    errno = 0;
    utest_check(prctl(PR_SET_DUMPABLE, 2) == -1 && errno == EINVAL, "PR_SET_DUMPABLE 2 is EINVAL");
    errno = 0;
    utest_check(prctl(1234, 0) == -1 && errno == EINVAL, "an unknown option is EINVAL");
    utest_check(child_exit(set_then_get) == 0, "a child that set 0 reads 0");
    utest_check(prctl(PR_GET_DUMPABLE) == 1, "...and its parent still reads 1");
    utest_check(child_exit(get_in_grandchild) == 0, "a fork copies the setting");
    utest_check(child_exit(exec_get) == 1, "an exec starts at 1 again");

    int code;
    int quiet = quiet_faulting_child(&code);
    utest_checkf(quiet > 0 && code == segv, "a quiet child still dies of SIGSEGV (exit %d)", code);
    utest_check(!crash_noted(quiet), "...with no crash record for the desktop's card");
    char path[96];
    snprintf(path, sizeof path, "/var/crash/dumpable_test-%d.crash", quiet);
    struct sys_stat st;
    utest_check(sys_stat(path, &st) < 0, "...and no report file");
    return utest_end();
}
