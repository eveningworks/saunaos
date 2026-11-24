// What an identity may DO, from ring 3 (docs/users-design.md, stage B):
// the admin rows need euid 0, the seat may mount and unmount only under
// /media/ and /net/, a signal needs the target's owner, and an elevated
// spawn needs privd's grant for exactly that command line, once.
//
// What a broken version would still pass, and what stops it: a gate that
// let everyone through passes every check made as root, so each refusal
// is asked of a 1000 child; a gate that refused everyone passes the
// refusals, so the seat child's /media unmount must get PAST it (EINVAL,
// nothing mounted there); a seat rule matched as a prefix passes
// "/media/x", so "/media/../boot" must be refused; a grant that checked
// only the path passes the right argv, so a different argv must fail;
// a grant that was not spent passes once, so the second spawn must fail.
//
// SPAWNED by tools/tests/gate/usertest_run.py as root.
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "cred_abi.h"
#include "signal_abi.h"
#include "proc_info.h"
#include "base/utest.h"

#define SELF "/tests/priv_test"

static const uint32_t GROUPS[] = { 1000, 10 };

// What the grant says the elevated child's environment is -- the asker
// passes none, so seeing it proves the kernel used the grant's.
static char *g_env[] = { "PRIVTEST=granted", 0 };

static int wait_code(int pid) {
    int code = -1;
    if (sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

static int spawn_as(const char *mode, unsigned cred_flags, int extra) {
    char *argv[] = { SELF, (char *)mode, 0 };
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.flags = SPAWN_ARGV | SPAWN_CREDS | extra;
    o.uid = o.gid = 1000;
    o.groups = GROUPS;
    o.ngroups = 2;
    o.cred_flags = cred_flags;
    return sys_spawn_opts(SELF, &o);
}

static int refused(int rc) { return rc < 0 && sys_errno() == EPERM; }

// Every refusal 1000 meets with or without the seat. A distinct code per
// check, so a failure names it.
static int common_refusals(void) {
    if (!refused(sys_modunload("priv_test_none"))) return 10;    // a root row
    if (!refused(sys_kill(1, 0))) return 11;                     // init is root's
    if (!refused(sys_kill(getppid(), 0))) return 12;             // so is the test
    int pid = fork();                                            // its own child is its own
    if (pid == 0) { for (;;) sys_sleep_ms(1000); }
    if (pid < 0 || sys_kill(pid, SIGKILL) != 0) return 13;
    wait_code(pid);
    char *argv[] = { SELF, "--whoami", 0 };
    if (!refused(sys_cred_grant(getpid(), SELF, argv, environ))) return 14; // root's call
    struct sys_spawn_opts o;                                     // no grant, no root
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.flags = SPAWN_ARGV | SPAWN_ELEVATED;
    if (!refused(sys_spawn_opts(SELF, &o))) return 15;
    return 0;
}

static int noseat_main(void) {
    int rc = common_refusals();
    if (rc) return rc;
    if (!refused(sys_umount("/media/priv_test_none"))) return 20;  // the seat's, not ours
    return 0;
}

static int seat_main(void) {
    if (sys_umount("/media/priv_test_none") == 0 || sys_errno() == EPERM) return 30;
    if (!refused(sys_umount("/boot"))) return 31;
    if (!refused(sys_umount("/media/../boot"))) return 32;
    if (!refused(sys_umount("/media/"))) return 33;
    return common_refusals();
}

static int spawn_elevated(char *const *argv) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.flags = SPAWN_ARGV | SPAWN_ELEVATED;
    return sys_spawn_opts(SELF, &o);
}

// Started stopped; the test grants it `--whoami` before continuing it.
static int elevate_main(void) {
    char *wrong[] = { SELF, "--whoami", "extra", 0 };
    if (!refused(spawn_elevated(wrong))) return 40;            // not the granted argv
    char *argv[] = { SELF, "--whoami", 0 };
    int pid = spawn_elevated(argv);
    if (pid <= 0) return 41;
    if (wait_code(pid) != 7) return 42;                         // it ran as root
    if (!refused(spawn_elevated(argv))) return 43;              // the grant is spent
    if (geteuid() != 1000) return 44;                           // and we are still 1000
    return 0;
}

static int row_of(int pid, struct proc_info *out) {
    for (int i = 0; sys_proc_info(i, out) == 0; i++)
        if (out->pid == pid) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--noseat")) return noseat_main();
    if (argc > 1 && !strcmp(argv[1], "--seat")) return seat_main();
    if (argc > 1 && !strcmp(argv[1], "--elevate")) return elevate_main();
    if (argc > 1 && !strcmp(argv[1], "--whoami")) {
        const char *e = getenv("PRIVTEST");
        return geteuid() != 0 ? 8 : (e && !strcmp(e, "granted")) ? 7 : 9;
    }

    utest_begin("priv_test", "the admin gate, the seat's mounts, the kill rule, elevated spawns",
                UTEST_VERDICT_FILE);

    int pid = spawn_as("--noseat", 0, 0);
    int code = pid > 0 ? wait_code(pid) : -3000;
    utest_checkf(code == 0, "1000 without the seat: root's calls, root's processes, the seat's "
                            "unmounts and an ungranted elevated spawn are refused (%d)", code);

    pid = spawn_as("--seat", CRED_SEAT, 0);
    code = pid > 0 ? wait_code(pid) : -3000;
    utest_checkf(code == 0, "1000 with the seat: unmounts under /media/ pass the gate, "
                            "/boot and /media/../boot do not, root's calls still refused (%d)", code);

    pid = spawn_as("--elevate", CRED_SEAT, SPAWN_STOPPED);
    struct proc_info pi;
    utest_check(pid > 0 && row_of(pid, &pi) && (pi.cred_flags & CRED_SEAT),
                "proc_info says a seat process holds the seat");
    char *wargv[] = { SELF, "--whoami", 0 };
    int g = pid > 0 ? sys_cred_grant(pid, SELF, wargv, g_env) : -1;
    utest_checkf(g == 0, "root grants it one elevated spawn (%d)", g);
    if (pid > 0) sys_kill(pid, SIGCONT);
    code = pid > 0 ? wait_code(pid) : -3000;
    utest_checkf(code == 0, "the granted command line runs as root, with the grant's environment, "
                            "once, and no other does (%d)", code);

    return utest_end();
}
