// <sys/stat.h>: mkdir(), and stat()/lstat()/fstat() over SYS_STAT
// and SYS_FSTAT.
#include <sys/stat.h>
#include <fcntl.h>   // AT_FDCWD
#include "rt/sys.h"
#include <errno.h>
#include <string.h>
#include <time.h>
#include "caltime.h"   // cal_rtc_to_epoch -- shared with the kernel
#include <unistd.h>     // access()
#include <utime.h>
#include <sys/time.h>  // utimes()

int mkdir(const char *path, mode_t mode) {
    // Read and discarded rather than left unnamed: this filesystem has
    // no permission bits, so there is nothing for the mode to mean. See
    // the header for why that is an ignore and not a dishonour.
    (void)mode;
    // sys_mkdir() already returns 0 or -1 with sys_errno() set, which is
    // exactly POSIX's contract -- there is nothing to translate.
    return sys_mkdir(path);
}


// The block size reported to a caller sizing its I/O. Not asked of the
// filesystem: TFS3's block is 4096 and every other path here is a
// memory copy, so a constant is the truthful answer rather than a
// lookup that would always return the same number.
#define STAT_BLKSIZE 4096

static void fill(struct stat *st, const struct sys_stat *s) {
    memset(st, 0, sizeof *st);
    st->st_ino  = s->ino;
    st->st_size = (off_t)s->size;
    // **THE TYPE COMES FROM is_dir, NOT FROM THE MODE.** The kernel
    // reports permission bits only; the type is a separate fact, and
    // folding them together is this boundary's job rather than the
    // filesystem's. A symlink cannot be distinguished here because the
    // path was already resolved through it (see lstat).
    st->st_mode = (mode_t)(s->mode & 07777) | (s->is_dir ? S_IFDIR : S_IFREG);
    st->st_nlink = s->nlink ? s->nlink : 1;
    st->st_uid = st->st_gid = 0;      // single-user, not a placeholder
    st->st_mtim.tv_sec = (time_t)cal_rtc_to_epoch(&s->modified);
    st->st_mtim.tv_nsec = s->modified_ns < 1000000000u ? (long)s->modified_ns : 0;
    st->st_ctim.tv_sec = (time_t)cal_rtc_to_epoch(&s->created);
    st->st_atim = st->st_mtim;        // no access time is recorded
    st->st_blksize = STAT_BLKSIZE;
    // 512-byte units, as every Unix reports regardless of the real
    // block size. Rounded UP: a one-byte file occupies a block.
    st->st_blocks = (blkcnt_t)((s->size + 511) / 512);
}

int stat(const char *path, struct stat *st) {
    if (!path || !st) { errno = EINVAL; return -1; }
    struct sys_stat s;
    int r = sys_stat(path, &s);
    if (r < 0) return -1;             // sys_stat set errno
    fill(st, &s);
    return 0;
}

// IDENTICAL TO stat(), deliberately -- see <sys/stat.h>. Path
// resolution here has no "do not follow the last link" mode, so a
// separate implementation would be the same code with a different name
// and a false promise.
int lstat(const char *path, struct stat *st) { return stat(path, st); }

int fstat(int fd, struct stat *st) {
    if (fd < 0 || !st) { errno = EINVAL; return -1; }
    struct sys_stat s;
    if (sys_fstat(fd, &s) < 0) return -1;
    fill(st, &s);
    // A pipe, a socket and a terminal are not regular files, and saying
    // so is what stops a caller seeking on one because S_ISREG was true.
    if (!(s.flags & SYS_STAT_SEEKABLE) && !s.is_dir)
        st->st_mode = (mode_t)((st->st_mode & 07777) |
                               ((s.flags & SYS_STAT_TTY) ? S_IFCHR : S_IFIFO));
    return 0;
}

// The process's file-creation mask. See <sys/stat.h> for why nothing
// consults it yet and why it is stored anyway.
static mode_t g_umask = 022;

mode_t umask(mode_t mask) {
    mode_t old = g_umask;
    g_umask = mask & 07777;
    return old;
}

int chmod(const char *path, mode_t mode) {
    if (!path) { errno = EINVAL; return -1; }
    // The kernel masks the type bits off; masking here too means a
    // caller passing a whole st_mode back gets what it expects rather
    // than an error.
    return sys_chmod(path, (unsigned)(mode & 07777));
}

// <utime.h> says what is set and what has nowhere to go.
// `t` NULL: now.
static int set_mtime(const char *path, const long long *t) {
    if (t && *t < 0) { errno = EINVAL; return -1; }
    return sys_utime(path, t ? (unsigned long long)*t : SYS_UTIME_NOW);
}

int utime(const char *path, const struct utimbuf *times) {
    long long t = times ? (long long)times->modtime : 0;
    return set_mtime(path, times ? &t : NULL);
}

int utimes(const char *path, const struct timeval tv[2]) {
    if (!tv) return set_mtime(path, NULL);
    // [1] is the modification time, to the microsecond.
    if (tv[1].tv_sec < 0 || tv[1].tv_usec < 0 || tv[1].tv_usec >= 1000000) { errno = EINVAL; return -1; }
    return sys_utimens(path, (unsigned long long)tv[1].tv_sec, (unsigned)tv[1].tv_usec * 1000u);
}

// times[1] is the one there is somewhere to keep. -1 = nothing to do.
static int mtime_of(const struct timespec times[2], unsigned long long *sec, unsigned *nsec) {
    if (!times || times[1].tv_nsec == UTIME_NOW) { *sec = SYS_UTIME_NOW; *nsec = 0; return 0; }
    if (times[1].tv_nsec == UTIME_OMIT) return -1;
    if (times[1].tv_sec < 0 || times[1].tv_nsec < 0 || times[1].tv_nsec >= 1000000000l) {
        errno = EINVAL;
        return -2;
    }
    *sec = (unsigned long long)times[1].tv_sec;
    *nsec = (unsigned)times[1].tv_nsec;
    return 0;
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags) {
    if (!path || flags) { errno = EINVAL; return -1; }
    if (dirfd != AT_FDCWD && path[0] != '/') { errno = ENOTSUP; return -1; }
    unsigned long long sec;
    unsigned nsec;
    int m = mtime_of(times, &sec, &nsec);
    if (m == -2) return -1;
    if (m == -1) { struct stat st; return stat(path, &st); }   // OMIT: still a missing file is ENOENT
    return sys_utimens(path, sec, nsec);
}

int futimens(int fd, const struct timespec times[2]) {
    unsigned long long sec;
    unsigned nsec;
    int m = mtime_of(times, &sec, &nsec);
    if (m == -2) return -1;
    if (m == -1) { struct stat st; return fstat(fd, &st); }
    return sys_futimens(fd, sec, nsec);
}

int fchmod(int fd, mode_t mode) {
    return sys_fchmod(fd, (unsigned)(mode & 07777));
}
