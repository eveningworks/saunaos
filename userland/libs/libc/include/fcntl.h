#ifndef ULIB_FCNTL_H
#define ULIB_FCNTL_H

// POSIX's <fcntl.h>: open(), and the flags for it.
//
// THE FLAG NAMES ARE THE ONLY NEW THING. SYS_OPEN has always taken
// exactly these bits under SYS_* names (abi/syscall_abi.h); this header
// gives them the spelling every program is written against, one list
// still, aliased rather than re-numbered.
//
// O_RDONLY IS 0, WHICH IS WHY THE KERNEL HAS NO NAME FOR IT. "Not
// SYS_O_WRITE" is read-only, so a program passing O_RDONLY passes 0 and
// gets what it asked for.
#include <syscall_abi.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY 0
#define O_WRONLY SYS_O_WRITE
// Read AND write on one descriptor and one position (abi/syscall_abi.h's
// SYS_O_RDWR, a mode of its own rather than a bit beside O_WRONLY).
#define O_RDWR   SYS_O_RDWR
// The access mode's bits, as `(flags & O_ACCMODE) == O_RDWR` asks.
#define O_ACCMODE (O_WRONLY | O_RDWR)

#define O_CREAT  SYS_O_CREAT
#define O_TRUNC  SYS_O_TRUNC
#define O_APPEND SYS_O_APPEND
#define O_EXCL   SYS_O_EXCL

// **O_NONBLOCK IS A fcntl(F_SETFL) FLAG, NOT AN open() ONE**, and the
// distinction is real rather than pedantic: non-blocking is set on an
// EXISTING descriptor (SYS_SET_NONBLOCK). open() REFUSES it below
// rather than accepting and ignoring it, which is the failure this
// library declines elsewhere -- a caller who passes it at open time
// and is told yes would then block.
#define O_NONBLOCK 0x00000800

// **CREATE, OR FAIL IF IT ALREADY EXISTS** -- only meaningful with
// O_CREAT, as POSIX says. The atomicity is the whole point: it is how a
// program claims a lock file without a window in which two of them both
// see it absent. Enforced in the kernel, not by a stat-then-open here,
// which would have exactly that window.

// Open `path`. The variadic `mode` argument POSIX requires with O_CREAT
// is accepted and IGNORED -- a file is created with the default for its
// type (kernel/fs/tfs3_internal.h's T3_MODE_DEFAULT) because there is no umask
// applied at creation and no chmod afterwards -- and is present so the
// universal `open(p, O_CREAT|O_WRONLY, 0644)` compiles unchanged.
int open(const char *path, int flags, ...);

// --- fcntl ------------------------------------------------------------
//
// **EVERY COMMAND HERE TAKES AN int, WHICH IS WHY THIS IS A FUNCTION
// AND ioctl() IS NOT.** <sys/ioctl.h> explains at length that a single
// entry point taking an untyped POINTER and an integer command cannot
// be checked at a ring boundary. fcntl's commands take integers and
// return integers, so there is nothing to validate and nothing to get
// wrong -- it forwards to typed syscalls exactly as ioctl() does.
//
// **THE LOCKING COMMANDS ARE POSIX RECORD LOCKS, and take a POINTER**
// (SYS_FCNTL_LOCK): advisory, on a byte range of the file, owned by the
// process -- gone when it exits or closes ANY descriptor on that file,
// not inherited by fork, kept across exec. Linux's rules, including the
// close one everybody trips over; kernel/include/kernel/flock.h. F_SETLK
// is -1 with EAGAIN when another process holds a conflicting lock;
// F_SETLKW waits (EINTR for a signal). No deadlock detection.
// "the current directory" for the *at() calls that exist (utimensat).
#define AT_FDCWD (-100)

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

// POSIX's record lock, which the commands above take a pointer to.
struct flock {
    short l_type;      // F_RDLCK, F_WRLCK, F_UNLCK
    short l_whence;    // SEEK_SET, SEEK_CUR, SEEK_END
    off_t l_start;
    off_t l_len;       // 0: to the end of the file
    pid_t l_pid;       // F_GETLK's answer: who holds it
};

// The only descriptor flag there is.
#define FD_CLOEXEC 1

// Returns a value that depends on the command, or -1 with errno.
//   F_DUPFD  arg = the lowest descriptor the copy may take
//   F_GETFD  returns FD_CLOEXEC or 0
//   F_SETFD  arg = FD_CLOEXEC or 0
//   F_GETFL  returns the access mode (O_RDONLY, O_WRONLY or O_RDWR)
//            and O_NONBLOCK, the one status flag this system tracks
//   F_SETFL  arg = the flags to set; only O_NONBLOCK is honoured, the
//            access mode is ignored, and anything else is REFUSED with
//            EINVAL rather than ignored
//   F_GETLK  arg = a struct flock *: the first lock in its way, or
//            l_type F_UNLCK
//   F_SETLK  arg = a struct flock *: take (F_RDLCK/F_WRLCK) or release
//            (F_UNLCK) it, or EAGAIN; F_SETLKW the same, waiting
// Any other command (F_GETOWN and the rest) is EINVAL.
int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif
