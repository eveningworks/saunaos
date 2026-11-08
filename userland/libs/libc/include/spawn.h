// <spawn.h> -- posix_spawn(), over SYS_SPAWN, which was shaped for it.
//
// This is the call every program here should reach for before fork():
// one syscall, no address-space copy, and the child's streams and
// group named up front (docs/fork-design.md).
//
// **TWO PATHS, ONE CONTRACT.** With no file actions and no attribute
// flags -- what most callers pass -- it is that one SYS_SPAWN. Anything
// else (a dup2 onto the child's stdin, a close, an open, a process
// group, a signal mask) runs as musl and glibc run it: fork(), apply
// each action in order in the child, exec. An exec that fails in the
// child is REPORTED to the caller as posix_spawn()'s error -- over a
// close-on-exec pipe -- rather than as a child that exits 127, which is
// what lets a compiler driver say "cannot find cc1" instead of nothing.
#ifndef _SPAWN_H
#define _SPAWN_H

#include <sys/types.h>
#include <signal.h>
#ifdef __cplusplus
extern "C" {
#endif

// The file actions, in the order they were added. Opaque: the fields are
// the library's.
struct __spawn_action;
typedef struct {
    int __n, __cap;
    struct __spawn_action *__a;
} posix_spawn_file_actions_t;

typedef struct {
    short    __flags;      // POSIX_SPAWN_*
    pid_t    __pgroup;     // with POSIX_SPAWN_SETPGROUP; 0 = a new group led by the child
    sigset_t __mask;       // with POSIX_SPAWN_SETSIGMASK
    sigset_t __def;        // with POSIX_SPAWN_SETSIGDEF: reset to SIG_DFL
} posix_spawnattr_t;

// POSIX's flags. RESETIDS is accepted and has nothing to do (one user);
// the scheduling pair is REFUSED with EINVAL, there being no policy to set.
#define POSIX_SPAWN_RESETIDS      0x01
#define POSIX_SPAWN_SETPGROUP     0x02
#define POSIX_SPAWN_SETSIGDEF     0x04
#define POSIX_SPAWN_SETSIGMASK    0x08
#define POSIX_SPAWN_SETSCHEDPARAM 0x10
#define POSIX_SPAWN_SETSCHEDULER  0x20

// Returns 0 with the child's pid in *pid, or an errno VALUE (POSIX
// returns the error rather than setting errno here).
int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]);
// The same with a PATH walk for a bare name, as execvp() does.
int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]);

// Each returns 0 or an errno value. The list grows as actions are added.
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd);

int posix_spawnattr_init(posix_spawnattr_t *a);
int posix_spawnattr_destroy(posix_spawnattr_t *a);
int posix_spawnattr_getflags(const posix_spawnattr_t *a, short *flags);
int posix_spawnattr_setflags(posix_spawnattr_t *a, short flags);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *a, pid_t *pgroup);
int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t pgroup);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *a, sigset_t *mask);
int posix_spawnattr_setsigmask(posix_spawnattr_t *a, const sigset_t *mask);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *a, sigset_t *def);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *a, const sigset_t *def);

#ifdef __cplusplus
}
#endif

#endif
