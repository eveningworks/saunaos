// AN ADDRESS RING 3 HANDS THE KERNEL TO RUN AT IS A USER ADDRESS.
//
// The kernel loads some user values straight into RIP, RSP and FS_BASE:
// a thread pointer (SYS_SET_TLS, a new thread's tls), a signal handler
// and its restorer, a new thread's entry and stack, and the RIP and RSP
// a sigreturn frame on the user stack carries. A NON-CANONICAL value
// (0x8000000000000000) faults the wrmsr or the iretq IN RING 0, where the
// fault belongs to no process: one unprivileged call stopped the machine.
// Each is refused now -- -EINVAL, or for a forged frame, SIGSEGV for the
// program alone (uaddr_is_user(), kernel/include/kernel/uaddr.h).
//
// Each probe runs in a child of its own, so a kernel that faults on one
// stops the test there instead of hiding the rest.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: the sigreturn probe under
// TCG**, which may not fault a non-canonical iretq the way a CPU does.
// Run it under KVM (usertest_run.py --kvm) for that one; the three that
// expect -EINVAL go red anywhere.
//
// It must be SPAWNED, not `run` -- it has children.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "base/utest.h"
#include "signal_abi.h"

#define SELF "/tests/canon_test"
#define BAD  0x8000000000000000ULL   // non-canonical: bit 63 set, bits 48-62 clear
#define TF_RIP 17                    // api/scheduler.h's SCHED_TF_RIP
#define TF_RSP 20                    // ...and SCHED_TF_RSP

static int refused(int rc) { return rc == -1 && sys_errno() == EINVAL; }

static void entry(void *arg) { (void)arg; sys_thread_exit(0); }
static void handler(int sig) { (void)sig; }
static void restorer(void) {}

static int probe_tls(void) {
    return refused(sys_set_tls((void *)BAD)) ? 0 : 1;
}

static int probe_sigaction(void) {
    struct k_sigaction a = { BAD, (uint64_t)(uintptr_t)restorer, 0, 0 };
    if (!refused(sys_sigaction(SIGUSR1, &a, 0))) return 1;
    a.handler = (uint64_t)(uintptr_t)handler;     // and a bad restorer
    a.restorer = BAD;
    return refused(sys_sigaction(SIGUSR1, &a, 0)) ? 0 : 2;
}

static int probe_thread(void) {
    static char stack[16384] __attribute__((aligned(16)));
    if (!refused(sys_thread_create((void (*)(void *))(uintptr_t)BAD, stack + sizeof stack, 0, 0, 1,
                                   sizeof stack))) return 1;
    return refused(sys_thread_create(entry, stack + sizeof stack, 0, (void *)BAD, 1,
                                     sizeof stack)) ? 0 : 2;
}

// A frame with the kernel's magic and a non-canonical RIP, and a
// sigreturn pointed at it. The kernel must kill this process, not fault.
static int probe_sigreturn(void) {
    struct sigframe f;
    memset(&f, 0, sizeof f);
    f.magic = SIGFRAME_MAGIC;
    f.regs[TF_RIP] = BAD;
    f.regs[TF_RSP] = (uint64_t)(uintptr_t)&f;
    __asm__ volatile ("mov %0, %%rsp\n\tmov %1, %%eax\n\tsyscall"
                      :: "r"((uint64_t)(uintptr_t)&f + 8), "i"(SYS_SIGRETURN)
                      : "rax", "rcx", "r11", "memory");
    return 3;   // not reached: the frame is either restored or refused
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--probe")) {
        if (!strcmp(argv[2], "tls")) return probe_tls();
        if (!strcmp(argv[2], "sigaction")) return probe_sigaction();
        if (!strcmp(argv[2], "thread")) return probe_thread();
        if (!strcmp(argv[2], "sigreturn")) return probe_sigreturn();
        return 9;
    }
    utest_begin("canon_test", "a RIP, RSP or FS_BASE from ring 3 is a user address",
                UTEST_VERDICT_FILE);
    static const struct { const char *name; int want; const char *what; } probes[] = {
        { "tls",       0, "SYS_SET_TLS refuses a non-canonical thread pointer" },
        { "sigaction", 0, "sigaction refuses a non-canonical handler or restorer" },
        { "thread",    0, "a thread with a non-canonical entry or tls is refused" },
        { "sigreturn", SIGNAL_EXIT_BASE + SIGSEGV,
                          "a forged frame's non-canonical RIP kills the program, not the kernel" },
    };
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
        char args[32];
        snprintf(args, sizeof args, "--probe %s", probes[i].name);
        int pid = sys_spawn(SELF, args, -1);
        int code = -1;
        if (pid <= 0 || sys_waitpid(pid, &code) != pid) code = -1;
        utest_checkf(code == probes[i].want, "%s (child exit %d)", probes[i].what, code);
    }
    return utest_end();
}
