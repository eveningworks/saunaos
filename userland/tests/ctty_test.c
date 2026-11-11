// The controlling terminal (docs/tty-design.md, "The controlling
// terminal"): a session's terminal, opened again by its name, and what
// happens to the session when its leader dies or the terminal does.
//
// Runs itself three ways. The PARENT makes a pty and starts the other
// two on its slave, each leading a NEW SESSION (SPAWN_SETSID), as a
// Terminal window starts its shell:
//
//   --leader   checks tcgetsid() and writes through open("/dev/tty"),
//              puts a JOB of its own in front and EXITS; the job's
//              SIGHUP handler reports on the terminal. Before the leader
//              took its session with it, the job ran on with nobody to
//              continue or end it.
//   --waiter   a shell waiting on a job: when the parent closes the
//              master, the waiter itself must get SIGHUP (exit 42), not
//              only the job in front -- Linux's hangup signals both.
//   --noctty   a session with no terminal: open("/dev/tty") is ENXIO.
//
// SPAWNED by usertest_run.py: it spawns, waits and reads ptys.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/ctty_test"

static void say(int fd, const char *s) { sys_write(fd, s, strlen(s)); }

// --- the children ------------------------------------------------------

static void on_hup_job(int sig) {
    (void)sig;
    say(1, "job-hup\n");
    sys_exit(0);
}

static int job_main(void) {
    signal(SIGHUP, on_hup_job);
    say(1, "job-ready\n");
    for (int i = 0; i < 100; i++) sys_sleep_ms(100);   // 10 s: the hangup ends it
    say(1, "job-timeout\n");
    return 1;
}

static int leader_main(void) {
    int sid = getsid(0);
    say(1, tcgetsid(0) == sid && sid == getpid() ? "sid-ok\n" : "sid-bad\n");
    int fd = open("/dev/tty", O_RDWR);
    if (fd >= 0) { say(fd, "via-ctty\n"); close(fd); }
    else say(1, "ctty-open-failed\n");

    // A job of its own in front, as a shell starts one -- then the
    // leader goes, as a shell whose window closes does.
    int job = sys_spawn_flags(SELF, "--job", -1, environ, PGID_NEW, 0);
    if (job < 0) { say(1, "job-spawn-failed\n"); return 1; }
    tcsetpgrp(0, job);
    sys_sleep_ms(500);   // its handler installed, its "ready" written
    return 0;
}

static void on_hup_waiter(int sig) { (void)sig; sys_exit(42); }

static int waiter_main(void) {
    signal(SIGHUP, on_hup_waiter);
    int job = sys_spawn_flags(SELF, "--job", -1, environ, PGID_NEW, 0);
    if (job < 0) return 1;
    tcsetpgrp(0, job);
    say(1, "waiter-ready\n");
    int status = 0;
    sys_waitpid(job, &status);
    sys_sleep_ms(500);   // a SIGHUP pending from the same hangup lands here
    return 0;            // only reached if the waiter itself was missed
}

static int noctty_main(void) {
    int fd = open("/dev/tty", O_RDWR);
    return fd < 0 && errno == ENXIO ? 0 : 1;
}

// --- the parent --------------------------------------------------------

// Spawns SELF `args` with 0/1/2 on `slave`, leading a new session.
static int spawn_on(int slave, const char *args) {
    int in0 = sys_dup(0), out1 = sys_dup(1), err2 = sys_dup(2);
    sys_dup2(slave, 0);
    sys_dup2(slave, 1);
    sys_dup2(slave, 2);
    int pid = sys_spawn_flags(SELF, args, -1, environ, PGID_NEW, SPAWN_SETSID);
    if (in0 >= 0)  { sys_dup2(in0, 0);  sys_close(in0); }
    if (out1 >= 0) { sys_dup2(out1, 1); sys_close(out1); }
    if (err2 >= 0) { sys_dup2(err2, 2); sys_close(err2); }
    return pid;
}

// Reads the master until `want` has arrived, or 8 s pass.
static int await(int master, char *acc, int cap, int *len, const char *want) {
    for (int tries = 0; tries < 160; tries++) {
        int64_t n = sys_read(master, acc + *len, (size_t)(cap - 1 - *len));
        if (n > 0) { *len += (int)n; acc[*len] = '\0'; }
        if (strstr(acc, want)) return 1;
        if (*len >= cap - 1) return 0;
        sys_sleep_ms(50);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--job"))    return job_main();
    if (argc > 1 && !strcmp(argv[1], "--leader")) return leader_main();
    if (argc > 1 && !strcmp(argv[1], "--waiter")) return waiter_main();
    if (argc > 1 && !strcmp(argv[1], "--noctty")) return noctty_main();

    utest_begin("ctty_test", "the controlling terminal, and its hangup", UTEST_VERDICT_FILE);
    static char acc[2048];
    int len = 0, master = -1, slave = -1;

    // 1. The leader's session, its terminal by name, and its exit.
    if (sys_openpty(&master, &slave) < 0) {
        utest_check(0, "openpty");
        return utest_end();
    }
    sys_set_nonblock(master, 1);
    int leader = spawn_on(slave, "--leader");
    sys_close(slave);
    utest_check(leader > 0, "the leader starts on a pty of its own");
    utest_check_detail(await(master, acc, sizeof acc, &len, "sid-ok"),
                       "tcgetsid() on its terminal is its own session", acc);
    utest_check_detail(strstr(acc, "via-ctty") != NULL,
                       "open(\"/dev/tty\") reaches the same terminal", acc);
    int status = 0;
    if (leader > 0) sys_waitpid(leader, &status);
    utest_check_detail(await(master, acc, sizeof acc, &len, "job-hup"),
                       "the leader's exit hangs up the job in front (SIGHUP)", acc);
    sys_close(master);

    // 2. A hangup reaches a session leader that is not in front.
    len = 0; acc[0] = '\0';
    if (sys_openpty(&master, &slave) == 0) {
        sys_set_nonblock(master, 1);
        int waiter = spawn_on(slave, "--waiter");
        sys_close(slave);
        int ready = await(master, acc, sizeof acc, &len, "waiter-ready");
        sys_sleep_ms(200);
        sys_close(master);   // the window closes
        status = 0;
        if (waiter > 0) sys_waitpid(waiter, &status);
        char why[96];
        snprintf(why, sizeof why, "ready %d, exit status %d (42 = its SIGHUP handler)", ready, status);
        utest_check_detail(ready && status == 42,
                           "closing the master hangs up the session leader too", why);
    } else {
        utest_check(0, "openpty (second)");
    }

    // 3. No terminal at all: stdin a pipe, in a session of its own.
    int p[2];
    if (sys_pipe(p) == 0) {
        int in0 = sys_dup(0);
        sys_dup2(p[0], 0);
        int lone = sys_spawn_flags(SELF, "--noctty", -1, environ, PGID_NEW, SPAWN_SETSID);
        if (in0 >= 0) { sys_dup2(in0, 0); sys_close(in0); }
        sys_close(p[0]);
        sys_close(p[1]);
        status = -1;
        if (lone > 0) sys_waitpid(lone, &status);
        char why[48];
        snprintf(why, sizeof why, "exit status %d", status);
        utest_check_detail(lone > 0 && status == 0,
                           "with no terminal, open(\"/dev/tty\") is ENXIO", why);
    }
    return utest_end();
}
