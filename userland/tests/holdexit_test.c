// A THREAD BLOCKED IN read() GIVES ITS FILE BACK WHEN ITS PROCESS EXITS.
//
// A read holds the open-file description for the call (fd_get_held(),
// Linux's fdget()), and the hold is dropped on the way out of the call.
// A sibling that calls exit() while the reader is parked in that read
// used to have the reader freed where it stood -- its frame, and the
// fd_put() in it, never ran -- so the description kept a reference
// forever: a pipe's read end stayed open, a socket never sent its FIN.
// telnetd's sessions stopped closing exactly so (docs/bugs.md, fixed).
//
// The child: stdin is a pipe's read end; a thread parks reading it and
// the main thread exits. The parent then writes the pipe: with the
// child's read end gone it must fail EPIPE, and with a hold leaked it
// succeeds into a buffer nobody will read.
//
// It must be SPAWNED, not `run` -- it runs a thread.
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"

static void *reader(void *arg) {
    (void)arg;
    char c;
    (void)read(0, &c, 1);   // parks: nothing is ever written before the exit
    return 0;
}

static int child(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, reader, NULL) != 0) return 2;
    sys_sleep_ms(50);   // the reader is parked in the kernel by now
    _exit(0);           // exit_group, with the reader still inside read()
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return child();
    utest_begin("holdexit_test", "a thread blocked in read() gives its file back at exit",
                UTEST_VERDICT_FILE);
    signal(SIGPIPE, SIG_IGN);   // the check is the errno, not the default death

    int p[2];
    utest_check(sys_pipe(p) == 0, "a pipe opens");
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = "child";
    o.stdin_fd = p[0];
    int pid = sys_spawn_opts("/tests/holdexit_test", &o);
    utest_checkf(pid > 0, "the child spawns with the read end as stdin (%d)", pid);
    sys_close(p[0]);   // the child's is now the only read end
    int code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    utest_checkf(code == 0, "the child exited with its reader still parked (status %d)", code);

    errno = 0;
    long long n = sys_write(p[1], "x", 1);
    utest_checkf(n < 0 && errno == EPIPE,
                 "the pipe has no reader left: a write fails EPIPE (got %lld, errno %d)",
                 n, errno);
    sys_close(p[1]);
    return utest_end();
}
