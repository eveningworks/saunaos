// <unistd.h>'s POSIX names, each one line over rt/sys.h.
//
// OUT OF LINE, NOT static inline IN THE HEADER, because a name is only
// FOUND by a real symbol: autoconf decides whether a system has getcwd()
// or dup2() by linking a call to it, and a configure that answers "no"
// compiles its own replacement over the header's declaration -- which is
// how GNU code fails to build. docs/conventions/build.md has the rule.
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>   // getcwd(NULL, 0) allocates
#include "rt/sys.h"
#include "syscall_abi.h"   // SYS_GETCRED

ssize_t read(int fd, void *buf, size_t n)        { return (ssize_t)sys_read(fd, buf, n); }
ssize_t write(int fd, const void *buf, size_t n) { return (ssize_t)sys_write(fd, buf, n); }
int   close(int fd)                 { return sys_close(fd); }
int   dup(int fd)                   { return sys_dup(fd); }
int   dup2(int o, int n)            { return sys_dup2(o, n); }
off_t lseek(int fd, off_t off, int whence) { return (off_t)sys_lseek(fd, off, whence); }
int   fsync(int fd)                 { return sys_fsync(fd); }
int   fdatasync(int fd)             { return sys_fsync(fd); }
int   unlink(const char *p)         { return sys_unlink(p); }
int   link(const char *from, const char *to) { return sys_link(from, to); }
int   rmdir(const char *p)          { return sys_unlink(p); }
int   chdir(const char *p)          { return sys_chdir(p); }
int   truncate(const char *p, off_t size) {
    if (size < 0) { errno = EINVAL; return -1; }
    return sys_truncate(p, (unsigned long long)size);
}
int   ftruncate(int fd, off_t size) {
    if (size < 0) { errno = EINVAL; return -1; }
    return sys_ftruncate(fd, (unsigned long long)size);
}
// sys_getcwd() returns the LENGTH, not 0 -- so this tests for the
// failure value rather than for success.
char *getcwd(char *b, size_t n) {
    if (b) {
        if (n == 0) { errno = EINVAL; return 0; }
        return sys_getcwd(b, n) < 0 ? 0 : b;
    }
    // glibc's extension, which POSIX leaves open: a NULL buffer is
    // allocated -- `n` bytes when given, else grown until the path fits.
    for (size_t cap = n ? n : 256;; cap *= 2) {
        char *p = malloc(cap);
        if (!p) { errno = ENOMEM; return 0; }
        if (sys_getcwd(p, cap) >= 0) return p;
        free(p);
        if (n || errno != ERANGE) return 0;
    }
}

// A terminal is a FLAG of SYS_FSTAT rather than a call of its own. 1 or
// 0, as POSIX says, and 0 for a bad fd (sys_fstat set errno).
int isatty(int fd) {
    struct sys_stat st;
    if (sys_fstat(fd, &st) != 0) return 0;
    return (st.flags & SYS_STAT_TTY) ? 1 : 0;
}

pid_t getpid(void)                  { return sys_getpid(); }
pid_t fork(void)                    { return sys_fork(); }
pid_t vfork(void)                   { return sys_fork(); }
int   pipe(int fds[2])              { return sys_pipe(fds); }
int   setpgid(pid_t pid, pid_t pgid) { return sys_setpgid(pid, pgid); }
pid_t getpgid(pid_t pid)            { return sys_getpgid(pid); }
pid_t getpgrp(void)                 { return sys_getpgid(0); }
pid_t setsid(void)                  { return sys_setsid(); }
pid_t getsid(pid_t pid)             { return sys_getsid(pid); }
pid_t tcgetpgrp(int fd)             { return sys_tcgetpgrp(fd); }
int   tcsetpgrp(int fd, pid_t pgid) { return sys_tcsetpgrp(fd, pgid); }
void  _exit(int code)               { sys_exit(code); }

// One SYS_GETCRED each: an identity never changes under a process in
// stage A, but a cache here would be wrong the day one does. THROUGH
// sys_call(), as prctl.c says why: a program built before sys_getcred
// could not load this libc otherwise.
static int64_t getcred(struct cred_info *c, gid_t *groups, uint32_t cap) {
    return sys_call(SYS_GETCRED, (uint64_t)(uintptr_t)c, (uint64_t)(uintptr_t)groups, cap);
}

static struct cred_info cred(void) {
    struct cred_info c;
    if (getcred(&c, NULL, 0) < 0) {
        c.ruid = c.euid = c.suid = 0;   // a kernel from before it: everything was root
        c.rgid = c.egid = c.sgid = 0;
        c.ngroups = 0;
    }
    return c;
}
uid_t getuid(void)  { return cred().ruid; }
uid_t geteuid(void) { return cred().euid; }
gid_t getgid(void)  { return cred().rgid; }
gid_t getegid(void) { return cred().egid; }

int getresuid(uid_t *r, uid_t *e, uid_t *s) {
    struct cred_info c = cred();
    if (r) *r = c.ruid;
    if (e) *e = c.euid;
    if (s) *s = c.suid;
    return 0;
}

int getresgid(gid_t *r, gid_t *e, gid_t *s) {
    struct cred_info c = cred();
    if (r) *r = c.rgid;
    if (e) *e = c.egid;
    if (s) *s = c.sgid;
    return 0;
}

// POSIX: size 0 asks for the count; a list too short for them all is
// EINVAL, not a short answer.
int getgroups(int size, gid_t *list) {
    if (size < 0) { errno = EINVAL; return -1; }
    struct cred_info c;
    if (getcred(&c, size ? list : NULL, (uint32_t)size) < 0) return 0;
    if (size && c.ngroups > (uint32_t)size) { errno = EINVAL; return -1; }
    return (int)c.ngroups;
}
char *getlogin(void) { errno = ENXIO; return NULL; }

unsigned sleep(unsigned sec)  { sys_sleep_ms((int)(sec * 1000)); return 0; }
int usleep(unsigned long us)  { return sys_sleep_ms((int)(us / 1000)); }
