// The sandbox from ring 3: SYS_PLEDGE, SYS_UNVEIL, SPAWN_SANDBOX and the
// shm rules (abi/sandbox_abi.h), each in a forked child so a check that
// is SUPPOSED to kill something kills only that child.
//
// What a broken version would still pass, and what stops it here: a
// gate that never fires still lets every allowed call through, so each
// group is checked both ways -- the allowed call answers and the
// forbidden one ends the child with SIGSYS; a pledge that applied to the
// whole machine would still kill the child, so the parent makes the same
// call afterwards and must survive it; an unveil that hid nothing would
// still open the unveiled file, so a sibling path must answer ENOENT; a
// SPAWN_SANDBOX that was ignored would still start the child, so the
// child itself must die on its first forbidden call.
//
// SPAWNED by tools/usertest_run.py: fork and waitpid need a scheduler
// slot, which the legacy `run` loader has not got.
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "sandbox_abi.h"
#include "signal_abi.h"
#include "net_abi.h"
#include "query_abi.h"
#include "proc_info.h"
#include "lib/utest.h"

#define KILLED_BY_SIGSYS (SIGNAL_EXIT_BASE + SIGSYS)
#define SELF "/tests/sandbox_test"
#define PUB  "t-sb-pub"

static int wait_code(int pid) {
    int code = -1;
    if (sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

// Runs `fn` in a forked child and returns its exit code: 0 when every
// check inside passed, N for the first that did not, KILLED_BY_SIGSYS
// when the sandbox ended it.
static int in_child(int (*fn)(void)) {
    int pid = fork();
    if (pid == 0) _exit(fn());
    return pid < 0 ? -2000 : wait_code(pid);
}

// --- promises --------------------------------------------------------------

static int stdio_then_socket(void) {
    if (pledge("stdio", NULL) != 0) return 1;
    if (getpid() <= 0) return 2;                 // STDIO still answers
    sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);           // INET: never returns
    return 3;
}

// The wall clock is a query record that `stdio` reads; any other record
// is the registry's, `query`.
static int clock_is_stdio(void) {
    if (pledge("stdio", NULL) != 0) return 1;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 1700000000) return 2;
    return 0;
}

static int other_query_needs_query(void) {
    if (pledge("stdio", NULL) != 0) return 1;
    struct query_proclimits q;
    sys_query_record(QUERY_PROCLIMITS, 0, &q, sizeof q);     // never returns
    return 2;
}

static int no_widening(void) {
    if (pledge("stdio rpath", NULL) != 0) return 1;
    if (pledge("stdio rpath inet", NULL) != -1 || errno != EPERM) return 2;
    if (pledge("stdio rpath bogus", NULL) != -1 || errno != EINVAL) return 3;
    if (pledge("stdio", NULL) != 0) return 4;    // narrowing is always allowed
    return 0;
}

static int write_needs_wpath(void) {
    if (pledge("stdio rpath", NULL) != 0) return 1;
    int fd = open(SELF, O_RDONLY);               // reading is RPATH
    if (fd < 0) return 2;
    close(fd);
    open("/tmp/sandbox_test.w", O_WRONLY | O_CREAT, 0644);   // never returns
    return 3;
}

static int grandchild_socket(void) {
    sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    return 9;
}

static int inherited_by_fork(void) {
    if (pledge("stdio proc", NULL) != 0) return 1;
    int code = in_child(grandchild_socket);
    return code == KILLED_BY_SIGSYS ? 0 : 2;
}

static int g_parent;

static int kill_other(void) {
    if (pledge("stdio", NULL) != 0) return 1;
    if (kill(getpid(), SIGCHLD) != 0) return 2;  // itself is STDIO (SIGCHLD: ignored)
    kill(g_parent, SIGCHLD);                     // anyone else is PROC: never returns
    return 3;
}

// --- unveil ----------------------------------------------------------------

static int unveil_hides(void) {
    if (unveil("/tests", "r") != 0) return 1;
    int fd = open(SELF, O_RDONLY);
    if (fd < 0) return 2;
    close(fd);
    struct sys_stat st;
    if (sys_stat("/etc", &st) != -1 || errno != ENOENT) return 3;
    if (open("/bin/ls", O_RDONLY) != -1 || errno != ENOENT) return 4;
    if (unveil("/bin", "r") != 0) return 5;      // more paths, until locked
    int fd2 = open("/bin/ls", O_RDONLY);
    if (fd2 < 0) return 6;
    close(fd2);
    if (unveil(NULL, NULL) != 0) return 7;
    if (unveil("/", "r") != -1 || errno != EPERM) return 8;          // locked
    if (unveil("/tests", "rw") != -1 || errno != EPERM) return 9;
    if (sys_stat("/etc", &st) != -1 || errno != ENOENT) return 10;   // still hidden
    return 0;
}

static int unveil_rights(void) {
    if (unveil("/tmp", "r") != 0 || unveil("/tests", "r") != 0) return 1;
    int fd = open("/tmp/sandbox_test.w", O_WRONLY | O_CREAT, 0644);
    if (fd != -1 || errno != EACCES) return 2;   // there, without the right
    return 0;
}

// --- shm -------------------------------------------------------------------

static int shm_publish(void) {
    if (pledge("stdio shm", NULL) != 0) return 1;
    int fd = sys_shm_open("t-sb-private", 4096, SHM_CREATE);    // private: fine
    if (fd < 0) return 2;
    close(fd);
    sys_shm_unlink("t-sb-private");
    sys_shm_open("t-sb-beacon", 4096, SHM_CREATE | SHM_PUBLIC); // PUBLISH: never returns
    return 3;
}

static int shm_unveil(void) {
    if (unveil("/tests", "r") != 0) return 1;
    if (sys_shm_open(PUB, 0, 0) != -1 || errno != ENOENT) return 2;   // hidden
    if (unveil("shm:" PUB, "r") != 0) return 3;
    int fd = sys_shm_open(PUB, 0, 0);
    if (fd < 0) return 4;
    close(fd);
    if (sys_shm_unlink(PUB) != -1 || errno != EPERM) return 5;        // not ours
    return 0;
}

static int shm_from(void) {
    int fd = sys_shm_open_from(PUB, g_parent);
    if (fd < 0) return 1;
    close(fd);
    if (sys_shm_open_from(PUB, getpid()) != -1 || errno != EPERM) return 2;
    return 0;
}

// --- SPAWN_SANDBOX ---------------------------------------------------------

// The spawned half: started with STDIO|RPATH, it must not be able to
// widen that, and must die on its first socket.
static int spawned_main(void) {
    if (sys_pledge(PROMISE_ALL) != -1 || errno != EPERM) return 1;
    sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    return 2;
}

static int spawn_sandboxed(void) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = "--spawned";
    o.flags = SPAWN_SANDBOX;
    o.promises = PROMISE_STDIO | PROMISE_RPATH;
    int pid = sys_spawn_opts(SELF, &o);
    return pid > 0 ? wait_code(pid) : -3000;
}

// The promises a waiting child reports through proc_info.
static int g_gate[2];
static int pledged_and_waits(void) {
    if (pledge("stdio", NULL) != 0) return 1;
    char c;
    read(g_gate[0], &c, 1);
    return 0;
}

static unsigned promises_of(int pid) {
    struct proc_info pi;
    for (int i = 0; sys_proc_info(i, &pi) == 0; i++)
        if (pi.pid == pid) return pi.promises;
    return 0;
}

static int last_crash_says(const char *prefix) {
    struct query_crash last;
    int found = 0;
    struct query_crash rec;
    QUERY_FOREACH(QUERY_CRASH, rec, qi) { last = rec; found = 1; }
    return found && !strncmp(last.fault, prefix, strlen(prefix));
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--spawned")) return spawned_main();

    utest_begin("sandbox_test", "pledge, unveil, SPAWN_SANDBOX and the shm rules",
                UTEST_VERDICT_FILE);
    g_parent = getpid();

    int code = in_child(stdio_then_socket);
    utest_checkf(code == KILLED_BY_SIGSYS, "pledge(\"stdio\"): socket() ends it with SIGSYS (%d)", code);
    utest_check(last_crash_says("Sandbox: socket needs 'inet'"),
                "...and the crash record names the call and the promise");
    int s = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    utest_check(s >= 0, "the parent, which never pledged, still opens a socket");
    if (s >= 0) close(s);

    code = in_child(clock_is_stdio);
    utest_checkf(code == 0, "stdio reads the wall clock, a query record (%d)", code);
    code = in_child(other_query_needs_query);
    utest_checkf(code == KILLED_BY_SIGSYS, "...and any other query record needs query (%d)", code);
    utest_check(last_crash_says("Sandbox: query needs 'query'"), "...named in the crash record");

    code = in_child(no_widening);
    utest_checkf(code == 0, "a pledge only narrows; a word nobody knows is EINVAL (%d)", code);
    code = in_child(write_needs_wpath);
    utest_checkf(code == KILLED_BY_SIGSYS, "rpath reads; open for writing needs wpath (%d)", code);
    code = in_child(inherited_by_fork);
    utest_checkf(code == 0, "a forked child inherits the pledge (%d)", code);
    code = in_child(kill_other);
    utest_checkf(code == KILLED_BY_SIGSYS, "kill() of itself is stdio, of another is proc (%d)", code);

    code = in_child(unveil_hides);
    utest_checkf(code == 0, "unveil hides the rest (ENOENT), only narrows, and locks (%d)", code);
    code = in_child(unveil_rights);
    utest_checkf(code == 0, "an unveiled path without the right is EACCES (%d)", code);

    code = in_child(shm_publish);
    utest_checkf(code == KILLED_BY_SIGSYS, "shm makes private objects; a beacon needs publish (%d)", code);
    sys_shm_unlink(PUB);
    int pub = sys_shm_open(PUB, 4096, SHM_CREATE | SHM_PUBLIC);
    utest_check(pub >= 0, "made a public object for the unveil checks");
    code = in_child(shm_unveil);
    utest_checkf(code == 0, "a public object is hidden until unveiled as shm:; unlink is the creator's (%d)", code);
    code = in_child(shm_from);
    utest_checkf(code == 0, "SHM_FROM opens only what that pid created (%d)", code);
    utest_check(sys_shm_unlink(PUB) == 0, "...and its creator may unlink it");
    if (pub >= 0) close(pub);

    code = spawn_sandboxed();
    utest_checkf(code == KILLED_BY_SIGSYS, "SPAWN_SANDBOX: the child cannot widen and dies on socket() (%d)", code);

    if (pipe(g_gate) == 0) {
        int pid = fork();
        if (pid == 0) _exit(pledged_and_waits());
        sys_sleep_ms(200);
        unsigned p = promises_of(pid);
        utest_checkf(p == PROMISE_STDIO, "proc_info reports a pledged child's promises (0x%x)", p);
        utest_checkf(promises_of(getpid()) == PROMISE_ALL, "...and PROMISE_ALL for the unpledged parent");
        write(g_gate[1], "x", 1);
        wait_code(pid);
    }
    return utest_end();
}
