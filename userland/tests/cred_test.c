// Who a process is, from ring 3: SYS_GETCRED through libc, SPAWN_CREDS,
// its inheritance, proc_info's ids, and /etc/passwd + /etc/group through
// <pwd.h>/<grp.h> (docs/users-design.md, stage A).
//
// What a broken version would still pass, and what stops it here: a
// getuid() that answered 0 forever passes every check made as root, so
// the child started as 1000 must SEE 1000; a SPAWN_CREDS that set only
// the child would pass that, so the child's own fork must see it too; a
// check that let anyone use SPAWN_CREDS still starts the child, so the
// unprivileged child's attempt must be EPERM; a proc_info that always
// reported 0 would pass for root, so the waiting child's row must say 1000.
//
// SPAWNED by tools/tests/gate/usertest_run.py as root.
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "proc_info.h"
#include "base/utest.h"

#define SELF "/tests/cred_test"

static const uint32_t GROUPS[] = { 1000, 10, 4242 };

static int wait_code(int pid) {
    int code = -1;
    if (sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

static int spawn_as(uint32_t uid, uint32_t gid, const char *args, int extra_flags) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = args;
    o.flags = SPAWN_CREDS | extra_flags;
    o.uid = uid;
    o.gid = gid;
    o.groups = GROUPS;
    o.ngroups = sizeof GROUPS / sizeof GROUPS[0];
    return sys_spawn_opts(SELF, &o);
}

// The ids every check below expects of a process started as 1000.
static int is_user(void) {
    uid_t r, e, s;
    gid_t gr, ge, gs;
    if (getresuid(&r, &e, &s) || r != 1000 || e != 1000 || s != 1000) return 0;
    if (getresgid(&gr, &ge, &gs) || gr != 1000 || ge != 1000 || gs != 1000) return 0;
    if (getuid() != 1000 || geteuid() != 1000 || getgid() != 1000 || getegid() != 1000) return 0;
    gid_t g[8];
    if (getgroups(0, NULL) != 3 || getgroups(8, g) != 3) return 0;
    if (g[0] != 1000 || g[1] != 10 || g[2] != 4242) return 0;
    errno = 0;
    if (getgroups(2, g) != -1 || errno != EINVAL) return 0;
    return 1;
}

// The spawned half, running as 1000.
static int spawned_main(void) {
    if (!is_user()) return 1;
    int pid = fork();                                  // a fork keeps the identity
    if (pid == 0) _exit(is_user() ? 0 : 1);
    if (pid < 0 || wait_code(pid) != 0) return 2;
    struct sys_spawn_opts o;                           // and may not choose another
    sys_spawn_opts_init(&o);
    o.args = "--spawned";
    o.flags = SPAWN_CREDS;
    if (sys_spawn_opts(SELF, &o) != -1 || errno != EPERM) return 3;
    sys_spawn_opts_init(&o);                           // an ordinary spawn inherits it
    o.args = "--grandchild";
    pid = sys_spawn_opts(SELF, &o);
    if (pid <= 0 || wait_code(pid) != 4) return 4;
    return 0;
}

// What an ordinary child of the 1000 process runs: it must be 1000 too,
// and must still be refused -- 4 says both.
static int grandchild_main(void) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.flags = SPAWN_CREDS;
    if (!is_user()) return 1;
    if (sys_spawn_opts(SELF, &o) != -1 || errno != EPERM) return 2;
    return 4;
}

static int row_of(int pid, struct proc_info *out) {
    for (int i = 0; sys_proc_info(i, out) == 0; i++)
        if (out->pid == pid) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--spawned")) return spawned_main();
    if (argc > 1 && !strcmp(argv[1], "--grandchild")) return grandchild_main();
    if (argc > 1 && !strcmp(argv[1], "--wait")) {
        char c;
        return read(0, &c, 1) >= 0 ? 0 : 1;
    }

    utest_begin("cred_test", "getuid, SPAWN_CREDS, proc_info's ids, /etc/passwd and /etc/group",
                UTEST_VERDICT_FILE);

    uid_t r = 1, e = 1, s = 1;
    utest_check(getresuid(&r, &e, &s) == 0 && r == 0 && e == 0 && s == 0,
                "a process the debug console started is root");
    utest_checkf(getgroups(0, NULL) == 0, "...with no supplementary groups (%d)", getgroups(0, NULL));

    int code = spawn_as(1000, 1000, "--spawned", 0);
    code = code > 0 ? wait_code(code) : -3000;
    utest_checkf(code == 0, "SPAWN_CREDS: the child is 1000, its fork and spawn too, "
                            "and it may not choose another (%d)", code);

    // proc_info: a child held stopped, so its row is read before it exits.
    int pid = spawn_as(1000, 1000, "--wait", SPAWN_STOPPED);
    struct proc_info pi;
    utest_check(pid > 0 && row_of(pid, &pi) && pi.uid == 1000 && pi.euid == 1000 &&
                pi.gid == 1000 && pi.egid == 1000, "proc_info reports the child's ids");
    utest_check(row_of(getpid(), &pi) && pi.uid == 0 && pi.euid == 0, "...and 0 for this root process");
    if (pid > 0) { sys_kill(pid, SIGKILL); wait_code(pid); }

    // The databases, as shipped.
    struct passwd *pw = getpwnam("user");
    utest_check(pw && pw->pw_uid == 1000 && pw->pw_gid == 1000 && !strcmp(pw->pw_dir, "/home/user"),
                "getpwnam(\"user\"): uid 1000, gid 1000, home /home/user");
    pw = getpwuid(0);
    utest_check(pw && !strcmp(pw->pw_name, "root") && !strcmp(pw->pw_dir, "/"), "getpwuid(0) is root, home /");
    errno = 0;
    utest_check(!getpwnam("nobody-at-all") && errno == 0, "an unknown name is NULL, errno untouched");
    char buf[16];
    struct passwd pwb, *res = &pwb;
    utest_check(getpwnam_r("user", &pwb, buf, 8, &res) == ERANGE && !res, "getpwnam_r: a short buffer is ERANGE");
    struct group *gr = getgrnam("wheel");
    utest_check(gr && gr->gr_gid == 10 && gr->gr_mem[0] && !strcmp(gr->gr_mem[0], "user") && !gr->gr_mem[1],
                "getgrnam(\"wheel\"): gid 10, one member, user");
    gid_t gl[4];
    int ng = 4;
    utest_checkf(getgrouplist("user", 1000, gl, &ng) == 2 && gl[0] == 1000 && gl[1] == 10,
                 "getgrouplist(\"user\"): its own group, then wheel (%d)", ng);
    ng = 1;
    utest_check(getgrouplist("user", 1000, gl, &ng) == -1 && ng == 2, "...and -1 with the count when short");
    return utest_end();
}
