#ifndef ULIB_UNISTD_H
#define ULIB_UNISTD_H

// The POSIX names for calls that already exist, and nothing else.
//
// EVERY FUNCTION HERE IS ONE LINE over rt/sys.h. The header earns its
// place because ported code writes `read(fd, buf, n)` and `#include
// <unistd.h>`, not because anything is implemented here -- and where a
// POSIX call has no SaunaOS equivalent it is ABSENT rather than stubbed.
// A fork() that always failed would be worse than a link error: the
// link error says "this program needs something this OS does not have",
// which is exactly true and exactly what a porter needs to read.
//
// So there is no pipe2/select/poll here, and sleep() is here but cannot
// be INTERRUPTED by a signal -- it returns 0 always, rather than POSIX's
// seconds-remaining, because there is no case in which that number
// could be anything else. fork() and exec*() ARE here (below), built
// for a ported shell; SYS_SPAWN and <spawn.h> stay the way a program
// written for SaunaOS starts another (docs/fork-design.md).
#include <stddef.h>
#include <stdint.h>
#include "rt/sys.h"

// POSIX's option macros, for what <time.h> provides: clock_gettime(),
// clock_getres() and nanosleep(), with a monotonic clock. NOT timer_*()
// -- POSIX files those under the same option, and code reaching for
// them gets a link error naming the gap.
#define _POSIX_TIMERS          200809L
#define _POSIX_MONOTONIC_CLOCK 200809L

// THE THREE DESCRIPTORS EVERY PROCESS STARTS WITH. POSIX fixes the
// numbers and this kernel matches them; the names exist because ported
// code writes STDOUT_FILENO and would otherwise not compile over a
// difference that is not real.
//
// fd 2 is worth one clause: it is the terminal's when there is one and
// the kernel log when there is not (a service, a desktop app), which is
// why sys_eprint() is the channel a GUI test tool asserts on. See
// docs/decisions.md, "stderr is the terminal's, and the kernel log only
// without one".
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// The POSIX type names live in <sys/types.h> now, guarded so that
// whichever header arrives first defines them -- code including only
// this one keeps working, which is what most of userland/ does.
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
int dup(int fd);
int dup2(int o, int n);
off_t lseek(int fd, off_t off, int whence);
int fsync(int fd);
// THE SAME CALL, and the duplication is honest rather than lazy.
// fdatasync(2) may skip metadata not needed to retrieve the data -- and
// here what a deferred write holds back IS the inode, so the metadata
// it is allowed to skip is exactly what has to land for the bytes to be
// findable. There is no cheaper subset to offer.
int fdatasync(int fd);
int unlink(const char *p);
// A second name for the file `from` names (a hard link): 0, or -1 with
// EPERM on a filesystem without link counts, EISDIR for a directory.
int link(const char *from, const char *to);
int rmdir(const char *p);
int chdir(const char *p);
// Grow (with zeroes) or shrink a file to `size` bytes, by path or by a
// descriptor open for writing. 0, or -1 with errno.
int truncate(const char *p, off_t size);
int ftruncate(int fd, off_t size);
// sys_getcwd() returns the LENGTH, not 0 -- so the POSIX wrapper tests
// for the failure value rather than for success. Getting that backwards
// made getcwd() report failure on every call that worked, which is the
// shape of mistake a one-line wrapper is most prone to.
char *getcwd(char *b, size_t n);
// A terminal is a FLAG of SYS_FSTAT rather than a call of its own --
// see abi/syscall_abi.h. Returns 1 or 0, as POSIX does, and 0 for a bad
// fd (POSIX also sets errno, which sys_fstat already did).
int isatty(int fd);


// --- processes --------------------------------------------------------

pid_t getpid(void);
pid_t getppid(void);   // 0 for a process the kernel started

// --- entropy ------------------------------------------------------------

// POSIX 2024's getentropy(): `len` bytes from the kernel's generator
// (SYS_GETRANDOM), all or nothing; 0, or -1 with EINVAL for more than
// GETENTROPY_MAX. Unguessable, unlike rand() -- the hash salt a parser of
// hostile input wants (expat's).
#define GETENTROPY_MAX 256
int getentropy(void *buf, size_t len);

// fork() and exec*() exist for the program that cannot be written any
// other way -- a ported POSIX shell. Everything else should keep using
// sys_spawn()/posix_spawn(): one syscall, no address-space copy, and the
// child's streams and group named up front (docs/fork-design.md).
// A fork shares memory copy-on-write and copies the WHOLE descriptor
// table, close-on-exec marks included; an exec keeps the descriptors
// (closing those marked FD_CLOEXEC), the cwd,
// the group and the parent, and resets caught signals.
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);   // environ
int execvp(const char *file, char *const argv[]);  // PATH walk, then environ

int pipe(int fds[2]);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
// A session is what a controlling terminal belongs to; setsid() starts
// one and drops the caller's. Refused for a process-group leader.
pid_t setsid(void);
pid_t getsid(pid_t pid);

// The FOREGROUND process group of the terminal `fd` names -- what
// Ctrl-C interrupts. POSIX puts these in <unistd.h> rather than
// <termios.h>, and so do we.
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgid);

// Exit WITHOUT running atexit handlers or flushing streams -- which is
// what makes it different from exit(), and the reason a child that has
// inherited a parent's buffered output should call this one.
__attribute__((noreturn)) void _exit(int code);

// --- does this path exist? --------------------------------------------
//
// **F_OK IS THE ONLY MODE THAT MEANS ANYTHING HERE.** access() asks
// whether a path is reachable AND whether the caller may read, write or
// execute it; this filesystem has no permission bits and no user to
// check them against (docs/filesystem-layout.md), so R_OK/W_OK/X_OK can
// only be answered as "yes, if it exists". They are accepted and treated
// as F_OK rather than refused, because a caller asking "can I read
// this?" on a system where every existing file is readable is asking a
// question with a correct answer.
//
// Returns 0 if the path exists, -1 with errno ENOENT if it does not.
int access(const char *path, int mode);

// THE SANDBOX, OpenBSD's two calls and strings (abi/sandbox_abi.h).
// pledge() keeps only the named promises -- "stdio rpath shm", the words
// in libc/pledge.c -- and a syscall outside them ENDS the process with
// SIGSYS. unveil() hides every path not unveiled ("r", "w", "c", "x"),
// which then answers ENOENT; unveil(NULL, NULL) locks the list, which
// is open until then. Promises only shrink, and both survive fork, spawn
// and exec, so `execpromises` must be NULL (EINVAL otherwise).
int pledge(const char *promises, const char *execpromises);
int unveil(const char *path, const char *permissions);

// --- who we are -------------------------------------------------------
//
// **ALL FIVE ANSWER 0 OR EMPTY, AND THAT IS A FACT RATHER THAN A STUB.**
// This system has no login, no /etc/passwd and no second user. Zero is
// root's id on every Unix, which is also the truthful answer here -- the
// one user can do everything -- so a program branching on
// `geteuid() == 0` takes the branch that is correct for this machine.
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);

// The supplementary group list, which is empty. Returns the COUNT, 0,
// for any size -- including the size-0 form that asks how many there
// are.
int getgroups(int size, gid_t *list);

// The session's login name, which does not exist: there is no login and
// no record of one, so this is always NULL with ENXIO -- glibc's answer
// with no utmp entry. The user's NAME is <pwd.h>'s, getpwuid(getuid()).
char *getlogin(void);

// **vfork() IS fork(), and the distinction it names does not exist
// here.** vfork promised a cheaper fork by sharing the parent's address
// space and suspending it until exec -- an optimisation for systems
// without copy-on-write. This fork IS copy-on-write
// (docs/conventions/kernel.md), so the promise is already kept, and the
// dangerous half of vfork's contract (a child that must touch nothing
// before exec) would buy nothing in exchange. POSIX removed it in 2008.
pid_t vfork(void);

// SEEK_SET/CUR/END are POSIX's here as well as <stdio.h>'s, since
// lseek() is declared in this header and code reaching for the constants
// includes whichever it saw first.
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

// --- sysconf ----------------------------------------------------------
//
// Runtime system values. Only the names below are answered; anything
// else is -1 with EINVAL, which is how POSIX says "this implementation
// has no limit for that" AND how it says "no such name" -- the two are
// distinguished by whether errno was set, so this sets it.
#define _SC_CLK_TCK      1
#define _SC_PAGESIZE     2
#define _SC_PAGE_SIZE    _SC_PAGESIZE
#define _SC_OPEN_MAX     3
#define _SC_ARG_MAX      4
#define _SC_NPROCESSORS_ONLN 5

// **_SC_CLK_TCK IS 1000000 HERE, NOT glibc's 100**, so that times()'s
// unit and clock()'s CLOCKS_PER_SEC are the same number and the one
// real trap in <sys/times.h> costs nothing. The kernel measures CPU
// time in nanoseconds (abi/proc_info.h), so a coarser tick would
// throw away precision it already has to buy compatibility with a
// constant no correct program hardcodes.
long sysconf(int name);
// BSD's spelling of sysconf(_SC_PAGESIZE), which older code still calls.
int getpagesize(void);

// A configuration STRING, sysconf()'s companion. Only _CS_PATH is
// answered: <paths.h>'s _PATH_DEFPATH, where the system's programs are
// (make and execvp() search it when PATH is unset). Returns the size the
// whole value needs, NUL included, after copying as much as fits into
// `buf` (none with len 0); any other name is 0 with EINVAL.
#define _CS_PATH 0
size_t confstr(int name, char *buf, size_t len);

// A path's limits, POSIX's pathconf()/fpathconf(): the same answer for
// every path and descriptor, since every filesystem here shares the
// path and name bounds (<limits.h>). The names are glibc's numbers;
// anything else is -1 with EINVAL, as for sysconf().
#define _PC_LINK_MAX  0
#define _PC_MAX_CANON 1
#define _PC_MAX_INPUT 2
#define _PC_NAME_MAX  3
#define _PC_PATH_MAX  4
#define _PC_PIPE_BUF  5
long pathconf(const char *path, int name);
long fpathconf(int fd, int name);

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

// --- sleeping ---------------------------------------------------------
//
// Both return 0 and never return early. POSIX's sleep() returns the
// seconds REMAINING when interrupted -- here nothing interrupts a
// sleep, so the answer is always 0 rather than a number a caller would
// have to check.
unsigned sleep(unsigned sec);
int usleep(unsigned long us);

// --- fd flags ---------------------------------------------------------

// Make a read that would block return -1/EAGAIN instead. This is
// fcntl(F_SETFL, O_NONBLOCK) on a real system; a named call here,
// because ioctl/fcntl's whole shape is a dispatch chain over an untyped
// argument and <fcntl.h> says why it is absent.
static inline int set_nonblock(int fd, int on) { return sys_set_nonblock(fd, on); }

// --- argument parsing -------------------------------------------------
//
// POSIX getopt. Parsing STOPS at the first non-option argument (BSD's
// and POSIX's behaviour), rather than permuting argv the way glibc
// does -- see userland/libs/libc/getopt.c.
extern char *optarg;
extern int optind, opterr, optopt;
// BSD's scanner reset: set it to 1 to begin a fresh getopt() loop, which
// optind alone cannot do (it cannot say "and forget where you were
// inside a cluster like -la"). Cleared by getopt().
extern int optreset;
int getopt(int argc, char *const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif

#endif
