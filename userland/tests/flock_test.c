// fcntl() record locks between two processes (kernel/fs/flock.c), from
// ring 3: what make --output-sync and every lock-file user relies on.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, and what is checked instead:
//  - "F_SETLK returned 0" passes a lock that locks nothing: a forked
//    CHILD, a different process, must be refused while the parent holds
//    it, and F_GETLK must name the parent;
//  - "F_SETLKW returned" passes one that never waited: the child's wait
//    must outlast a delay the parent takes before letting go, measured
//    by a byte the parent writes just before its unlock;
//  - POSIX's close rule, the trap of the API: the parent opening and
//    closing a SECOND descriptor on the file must drop its lock;
//  - a process's exit must drop its locks.
//
// SPAWNED by tools/tests/gate/usertest_run.py: a fork needs a scheduler slot.
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"

#define LPATH "/tmp/flock_test.lock"

static int wait_code(int pid) {
    int code = -1;
    if (sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

static int lock(int fd, int cmd, int type) {
    struct flock fl = { .l_type = (short)type, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    return fcntl(fd, cmd, &fl);
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(void) {
    utest_begin("flock_test", "fcntl record locks between processes", UTEST_VERDICT_FILE);
    unlink(LPATH);
    int fd = open(LPATH, O_RDWR | O_CREAT, 0644);
    utest_check(fd >= 0, "the lock file opens read-write");
    utest_check(lock(fd, F_SETLK, F_WRLCK) == 0, "the parent takes a write lock on the whole file");
    int me = getpid();

    // 1. Another process is refused, and told who holds it.
    int pid = fork();
    if (pid == 0) {
        int cfd = open(LPATH, O_RDWR);
        errno = 0;
        if (lock(cfd, F_SETLK, F_WRLCK) != -1 || errno != EAGAIN) _exit(1);
        if (lock(cfd, F_SETLK, F_RDLCK) != -1) _exit(2);
        struct flock fl = { .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 5, .l_len = 1 };
        if (fcntl(cfd, F_GETLK, &fl) != 0 || fl.l_type != F_WRLCK || fl.l_pid != me) _exit(3);
        _exit(0);
    }
    int c = wait_code(pid);
    utest_checkf(c == 0, "a child is refused (EAGAIN) and F_GETLK names the parent (child said %d)", c);

    // 2. F_SETLKW waits until the holder lets go.
    pid = fork();
    if (pid == 0) {
        int cfd = open(LPATH, O_RDWR);
        long t0 = now_ms();
        if (lock(cfd, F_SETLKW, F_WRLCK) != 0) _exit(1);
        long waited = now_ms() - t0;
        char b = 0;
        if (lseek(cfd, 0, SEEK_SET) != 0 || read(cfd, &b, 1) != 1 || b != 'U') _exit(2);   // the parent's last act before unlocking
        _exit(waited >= 100 ? 0 : 3);   // of a 400 ms hold, however late the child ran
    }
    struct timespec nap = { 0, 400 * 1000000L };
    nanosleep(&nap, 0);
    lseek(fd, 0, SEEK_SET);
    utest_check(write(fd, "U", 1) == 1, "the parent marks the file before letting go");
    utest_check(lock(fd, F_SETLK, F_UNLCK) == 0, "the parent unlocks");
    c = wait_code(pid);
    utest_checkf(c == 0, "F_SETLKW waits for the unlock, then has it (child said %d)", c);

    // 3. POSIX's close rule: ANY descriptor closed drops the lock.
    utest_check(lock(fd, F_SETLK, F_WRLCK) == 0, "the parent locks again");
    int other = open(LPATH, O_RDONLY);
    close(other);
    pid = fork();
    if (pid == 0) {
        int cfd = open(LPATH, O_RDWR);
        _exit(lock(cfd, F_SETLK, F_WRLCK) == 0 ? 0 : 1);
    }
    c = wait_code(pid);
    utest_checkf(c == 0, "closing a second descriptor on the file dropped the lock (child said %d)", c);

    // 4. The child's exit dropped ITS lock (it took one just above).
    utest_check(lock(fd, F_SETLK, F_WRLCK) == 0, "a lock the exited child held is gone");

    // 5. A fork does not inherit the lock: the child holds none of its own.
    pid = fork();
    if (pid == 0) {
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        _exit(fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type == F_WRLCK && fl.l_pid == me ? 0 : 1);
    }
    c = wait_code(pid);
    utest_checkf(c == 0, "a forked child sees the parent's lock as someone else's (child said %d)", c);

    close(fd);
    unlink(LPATH);
    return utest_end();
}
