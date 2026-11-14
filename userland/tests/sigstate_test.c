// A SIGNAL HANDLER LEAVES THE INTERRUPTED CODE AS IT FOUND IT, AND A
// SIGNAL NOBODY WANTS ANY MORE IS DISCARDED.
//
//   fpu   the frame carries the FPU/SSE state: a handler that writes an
//         XMM register does not change it for the code it interrupted
//         (every XMM register is caller-saved, so any C handler may)
//   ign   a SIGCHLD that arrived blocked, under a handler, is dropped
//         when the disposition goes back to SIG_DFL -- whose action for
//         SIGCHLD is to ignore it -- rather than killing the process
//
// Each probe runs in a child, so a broken one takes only it down. It must
// be SPAWNED, not `run` -- it has children.
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/sigstate_test"
#define MAGIC 0x5eed5eed5eed5eedull

static void clobber(int sig) {
    (void)sig;
    __asm__ volatile("pxor %%xmm7, %%xmm7" ::: "xmm7");
}

// The value in xmm7 across a kill() of itself: the signal is delivered on
// that syscall's way out, so the handler runs between the two moves.
static int probe_fpu(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = clobber;
    if (sigaction(SIGUSR1, &sa, 0) != 0) return 2;
    uint64_t in = MAGIC, out = 0;
    int64_t ret;
    __asm__ volatile(
        "movq %[in], %%xmm7\n\t"
        "syscall\n\t"
        "movq %%xmm7, %[out]\n\t"
        : [out] "=r"(out), "=a"(ret)
        : [in] "r"(in), "a"((uint64_t)SYS_KILL), "D"((uint64_t)sys_getpid()),
          "S"((uint64_t)SIGUSR1)
        : "rcx", "r11", "xmm7", "memory");
    if (ret != 0) return 3;
    return out == MAGIC ? 0 : 1;
}

static volatile int g_noted;
static void noted(int sig) { (void)sig; g_noted++; }

// A child's exit while SIGCHLD is blocked: it is then pending.
static int child_exit_blocked(const sigset_t *set) {
    if (sigprocmask(SIG_BLOCK, set, 0) != 0) return 0;
    int pid = sys_spawn(SELF, "--probe exit", -1);
    int code = -1;
    return pid > 0 && sys_waitpid(pid, &code) == pid;
}

static int probe_ign(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = noted;
    if (sigaction(SIGCHLD, &sa, 0) != 0) return 2;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    // The fixture, first: with the handler kept, the unblock runs it once.
    if (!child_exit_blocked(&set)) return 3;
    sigprocmask(SIG_UNBLOCK, &set, 0);
    if (g_noted != 1) return 4;
    // Then the case: the same pending SIGCHLD, the disposition back to SIG_DFL.
    if (!child_exit_blocked(&set)) return 3;
    signal(SIGCHLD, SIG_DFL);
    sigprocmask(SIG_UNBLOCK, &set, 0);
    return g_noted == 1 ? 0 : 5;   // reached only if the unblock did not kill us
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--probe")) {
        if (!strcmp(argv[2], "fpu"))  return probe_fpu();
        if (!strcmp(argv[2], "ign"))  return probe_ign();
        if (!strcmp(argv[2], "exit")) return 0;
        return 9;
    }
    utest_begin("sigstate_test", "a handler keeps the FPU state, an unwanted signal is dropped",
                UTEST_VERDICT_FILE);
    static const struct { const char *name, *what; } probes[] = {
        { "fpu", "a handler that writes xmm7 leaves the interrupted code's xmm7" },
        { "ign", "a pending SIGCHLD reset to SIG_DFL is dropped, not fatal" },
    };
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
        char args[32];
        strcpy(args, "--probe ");
        strcat(args, probes[i].name);
        int pid = sys_spawn(SELF, args, -1);
        int code = -1;
        if (pid <= 0 || sys_waitpid(pid, &code) != pid) code = -1;
        utest_checkf(code == 0, "%s (child exit %d)", probes[i].what, code);
    }
    return utest_end();
}
