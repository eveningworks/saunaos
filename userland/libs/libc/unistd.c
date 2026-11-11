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
#include "rt/sys.h"

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
char *getcwd(char *b, size_t n)     { return sys_getcwd(b, n) < 0 ? 0 : b; }

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

uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }
int getgroups(int size, gid_t *list) { (void)size; (void)list; return 0; }
char *getlogin(void) { errno = ENXIO; return NULL; }

unsigned sleep(unsigned sec)  { sys_sleep_ms((int)(sec * 1000)); return 0; }
int usleep(unsigned long us)  { return sys_sleep_ms((int)(us / 1000)); }
