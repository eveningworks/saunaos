// open(), the variadic POSIX form over the kernel's fixed-arity one.
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include "rt/sys.h"
#include <errno.h>
#include <unistd.h>   // SEEK_SET

int open(const char *path, int flags, ...) {
    // The mode argument is READ AND DISCARDED rather than left on the
    // stack: a variadic function that never calls va_start is legal,
    // but doing it here documents that the argument is expected and
    // deliberately unused, instead of looking like an oversight.
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        (void)va_arg(ap, int);
        va_end(ap);
    }
    // **/dev/tty IS THE CONTROLLING TERMINAL**, and there are no device
    // nodes to find it by, so the name is the call (SYS_OPEN_CTTY). What a
    // ported program opens to ask for a password or read keys while its
    // stdin is a pipe. The flags have nothing to add: a terminal is read
    // and written.
    if (path && !strcmp(path, "/dev/tty")) return sys_open_ctty();
    return sys_open(path, flags);
}


_Static_assert(F_RDLCK == SYS_LK_RD && F_WRLCK == SYS_LK_WR && F_UNLCK == SYS_LK_UN,
               "<fcntl.h>'s lock types are the kernel's numbers");

// The record-lock commands, over SYS_FCNTL_LOCK (struct flock is libc's
// shape; struct sys_flock the kernel's).
static int lock_cmd(int fd, int cmd, struct flock *fl) {
    if (!fl) { errno = EFAULT; return -1; }
    struct sys_flock k = { (int16_t)fl->l_type, (int16_t)fl->l_whence, 0,
                           (int64_t)fl->l_start, (int64_t)fl->l_len };
    int kcmd = cmd == F_GETLK ? SYS_LK_GET : cmd == F_SETLK ? SYS_LK_SET : SYS_LK_SETW;
    if (sys_fcntl_lock(fd, kcmd, &k) < 0) return -1;
    if (cmd == F_GETLK) {
        fl->l_type = k.type;
        if (k.type != F_UNLCK) {
            fl->l_whence = SEEK_SET;
            fl->l_start = (off_t)k.start;
            fl->l_len = (off_t)k.len;
            fl->l_pid = (pid_t)k.pid;
        }
    }
    return 0;
}

// fcntl -- see <fcntl.h> for why this is a function and ioctl() is not.
// Every command forwards to a typed syscall.
int fcntl(int fd, int cmd, ...) {
    // The lock commands' argument is a pointer, never read as an int.
    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
        va_list ap;
        va_start(ap, cmd);
        struct flock *fl = va_arg(ap, struct flock *);
        va_end(ap);
        return lock_cmd(fd, cmd, fl);
    }
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);

    switch (cmd) {
    case F_DUPFD:
        return sys_dupfd(fd, arg);
    case F_GETFD: {
        int r = sys_fd_cloexec(fd, -1);      // -1 queries
        return r < 0 ? -1 : (r ? FD_CLOEXEC : 0);
    }
    case F_SETFD: {
        // Only one flag exists, and anything else is refused rather
        // than silently dropped -- a caller setting a flag this system
        // does not have must not be told it worked.
        if (arg & ~FD_CLOEXEC) { errno = EINVAL; return -1; }
        int r = sys_fd_cloexec(fd, (arg & FD_CLOEXEC) ? 1 : 0);
        return r < 0 ? -1 : 0;
    }
    case F_GETFL: {
        // The access mode, as the kernel reports the description's, and
        // O_NONBLOCK -- the one status flag this system tracks. A
        // descriptor that can do neither (none today) reads O_RDONLY.
        struct sys_stat st;
        if (sys_fstat(fd, &st) < 0) return -1;
        int r = O_RDONLY;
        if (st.flags & SYS_STAT_WRITES) r = (st.flags & SYS_STAT_READS) ? O_RDWR : O_WRONLY;
        return r | ((st.flags & SYS_STAT_NONBLOCK) ? O_NONBLOCK : 0);
    }
    case F_SETFL:
        // The access mode is IGNORED, as POSIX says -- it is what F_GETFL
        // hands back, so `F_SETFL, F_GETFL | O_NONBLOCK` must work. Any
        // other flag is refused: fixed at open time (O_APPEND) or absent,
        // and accepting it would be the silent ignore this library
        // declines elsewhere.
        arg &= ~O_ACCMODE;
        if (arg & ~O_NONBLOCK) { errno = EINVAL; return -1; }
        return sys_set_nonblock(fd, (arg & O_NONBLOCK) ? 1 : 0);
    default:
        errno = EINVAL;
        return -1;
    }
}
