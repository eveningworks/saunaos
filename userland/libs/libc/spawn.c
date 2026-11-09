// posix_spawn() and posix_spawnp() -- see <spawn.h> for the two paths.
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "rt/sys.h"

enum { ACT_OPEN, ACT_CLOSE, ACT_DUP2 };
struct __spawn_action {
    int kind, fd, newfd, oflag;
    char *path;   // ACT_OPEN's, the list's own copy
};

// --- the file actions -------------------------------------------------

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) {
    if (!fa) return EINVAL;
    fa->__n = fa->__cap = 0;
    fa->__a = 0;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
    if (!fa) return EINVAL;
    for (int i = 0; i < fa->__n; i++) free(fa->__a[i].path);
    free(fa->__a);
    fa->__n = fa->__cap = 0;
    fa->__a = 0;
    return 0;
}

static struct __spawn_action *add(posix_spawn_file_actions_t *fa) {
    if (fa->__n == fa->__cap) {
        int cap = fa->__cap ? fa->__cap * 2 : 4;
        struct __spawn_action *a = realloc(fa->__a, (size_t)cap * sizeof *a);
        if (!a) return 0;
        fa->__a = a;
        fa->__cap = cap;
    }
    struct __spawn_action *a = &fa->__a[fa->__n++];
    memset(a, 0, sizeof *a);
    return a;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *path, int oflag, mode_t mode) {
    (void)mode;   // open() ignores it here (<fcntl.h>)
    if (!fa || !path || fd < 0) return fd < 0 ? EBADF : EINVAL;
    char *copy = strdup(path);
    if (!copy) return ENOMEM;
    struct __spawn_action *a = add(fa);
    if (!a) { free(copy); return ENOMEM; }
    a->kind = ACT_OPEN;
    a->fd = fd;
    a->oflag = oflag;
    a->path = copy;
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
    if (!fa) return EINVAL;
    if (fd < 0) return EBADF;
    struct __spawn_action *a = add(fa);
    if (!a) return ENOMEM;
    a->kind = ACT_CLOSE;
    a->fd = fd;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd) {
    if (!fa) return EINVAL;
    if (fd < 0 || newfd < 0) return EBADF;
    struct __spawn_action *a = add(fa);
    if (!a) return ENOMEM;
    a->kind = ACT_DUP2;
    a->fd = fd;
    a->newfd = newfd;
    return 0;
}

// --- the attributes ---------------------------------------------------

int posix_spawnattr_init(posix_spawnattr_t *a) {
    if (!a) return EINVAL;
    memset(a, 0, sizeof *a);
    return 0;
}
int posix_spawnattr_destroy(posix_spawnattr_t *a) { return a ? 0 : EINVAL; }

int posix_spawnattr_getflags(const posix_spawnattr_t *a, short *flags) {
    if (!a || !flags) return EINVAL;
    *flags = a->__flags;
    return 0;
}
int posix_spawnattr_setflags(posix_spawnattr_t *a, short flags) {
    if (!a) return EINVAL;
    if (flags & ~(POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
                  POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER))
        return EINVAL;
    a->__flags = flags;
    return 0;
}
int posix_spawnattr_getpgroup(const posix_spawnattr_t *a, pid_t *pg) {
    if (!a || !pg) return EINVAL;
    *pg = a->__pgroup;
    return 0;
}
int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t pg) {
    if (!a) return EINVAL;
    a->__pgroup = pg;
    return 0;
}
int posix_spawnattr_getsigmask(const posix_spawnattr_t *a, sigset_t *m) {
    if (!a || !m) return EINVAL;
    *m = a->__mask;
    return 0;
}
int posix_spawnattr_setsigmask(posix_spawnattr_t *a, const sigset_t *m) {
    if (!a || !m) return EINVAL;
    a->__mask = *m;
    return 0;
}
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *a, sigset_t *d) {
    if (!a || !d) return EINVAL;
    *d = a->__def;
    return 0;
}
int posix_spawnattr_setsigdefault(posix_spawnattr_t *a, const sigset_t *d) {
    if (!a || !d) return EINVAL;
    a->__def = *d;
    return 0;
}

// --- spawning ---------------------------------------------------------

static int spawn_one(pid_t *pid, const char *path, char *const argv[], char *const envp[]) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.env = envp ? (char **)envp : environ;
    int r = sys_spawn_opts(path, &o);
    if (r < 0) return errno;
    if (pid) *pid = r;
    return 0;
}

// Calls `try` on each PATH candidate for a bare `file` (or on `file`
// itself when it has a slash) until one does not answer ENOENT.
static int walk_path(const char *file, int (*try)(const char *, void *), void *ctx) {
    if (strchr(file, '/')) return try(file, ctx);
    const char *path = getenv("PATH");
    if (!path || !*path) path = _PATH_DEFPATH;
    size_t flen = strlen(file);
    char *full = malloc(strlen(path) + flen + 2);
    if (!full) return ENOMEM;
    int last = ENOENT;
    for (const char *p = path; ; ) {
        const char *end = strchr(p, ':');
        size_t dlen = end ? (size_t)(end - p) : strlen(p);
        memcpy(full, p, dlen);
        full[dlen] = '/';
        memcpy(full + dlen + 1, file, flen + 1);
        int r = try(full, ctx);
        if (r != ENOENT) { last = r; break; }
        if (!end) break;
        p = end + 1;
    }
    free(full);
    return last;
}

struct spawn_req {
    pid_t *pid;
    char *const *argv, *const *envp;
};

static int try_spawn(const char *path, void *ctx) {
    struct spawn_req *q = ctx;
    return spawn_one(q->pid, path, q->argv, q->envp);
}

static int try_exec(const char *path, void *ctx) {
    struct spawn_req *q = ctx;
    execve(path, q->argv, q->envp ? q->envp : environ);
    return errno;
}

// In the child, before the exec: the attributes, then each action in the
// order it was added. 0, or the errno that stops the spawn.
static int child_setup(const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at) {
    if (at) {
        if ((at->__flags & POSIX_SPAWN_SETPGROUP) && setpgid(0, at->__pgroup) < 0) return errno;
        if (at->__flags & POSIX_SPAWN_SETSIGDEF)
            for (int s = 1; s < NSIG; s++)
                if (sigismember(&at->__def, s)) signal(s, SIG_DFL);
        if ((at->__flags & POSIX_SPAWN_SETSIGMASK) &&
            sigprocmask(SIG_SETMASK, &at->__mask, 0) < 0) return errno;
    }
    for (int i = 0; fa && i < fa->__n; i++) {
        const struct __spawn_action *a = &fa->__a[i];
        switch (a->kind) {
        case ACT_CLOSE:
            close(a->fd);   // POSIX: closing a descriptor that is not open is not an error
            break;
        case ACT_DUP2:
            // dup2 onto itself keeps the descriptor and must CLEAR its
            // close-on-exec flag, or the action has no effect at all.
            if (a->fd == a->newfd) {
                if (fcntl(a->fd, F_SETFD, 0) < 0) return errno;
            } else if (dup2(a->fd, a->newfd) < 0) {
                return errno;
            }
            break;
        case ACT_OPEN: {
            int fd = open(a->path, a->oflag, 0);
            if (fd < 0) return errno;
            if (fd != a->fd) {
                if (dup2(fd, a->fd) < 0) return errno;
                close(fd);
            }
            break;
        }
        }
    }
    return 0;
}

static int spawn_forked(pid_t *pid, const char *file, int search,
                        const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                        char *const argv[], char *const envp[]) {
    if (at && (at->__flags & (POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER)))
        return EINVAL;
    int report[2];
    if (pipe(report) < 0) return errno;
    fcntl(report[1], F_SETFD, FD_CLOEXEC);   // an exec that works closes it
    pid_t c = fork();
    if (c < 0) {
        int e = errno;
        close(report[0]);
        close(report[1]);
        return e;
    }
    if (c == 0) {
        close(report[0]);
        // THE REPORT PIPE MUST OUTLIVE THE ACTIONS: its descriptor was
        // the lowest free one, so it can be the very number an action
        // closes or dup2s onto. Moved above every number they name.
        int top = report[1];
        for (int i = 0; fa && i < fa->__n; i++) {
            if (fa->__a[i].fd > top) top = fa->__a[i].fd;
            if (fa->__a[i].newfd > top) top = fa->__a[i].newfd;
        }
        int w = fcntl(report[1], F_DUPFD, top + 1);
        if (w >= 0) {
            fcntl(w, F_SETFD, FD_CLOEXEC);
            close(report[1]);
            report[1] = w;
        }
        struct spawn_req q = { 0, argv, envp };
        int e = child_setup(fa, at);
        if (!e) e = search ? walk_path(file, try_exec, &q) : try_exec(file, &q);
        // Only reached when no exec worked: tell the parent why.
        (void)!write(report[1], &e, sizeof e);
        _exit(127);
    }
    close(report[1]);
    int e = 0;
    ssize_t n;
    do n = read(report[0], &e, sizeof e); while (n < 0 && errno == EINTR);
    close(report[0]);
    if (n == (ssize_t)sizeof e) {   // the child said why, and is gone
        waitpid(c, 0, 0);
        return e;
    }
    if (pid) *pid = c;
    return 0;
}

// SYS_SPAWN serves a spawn with nothing to do in the child.
static int trivial(const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at) {
    return (!fa || fa->__n == 0) && (!at || !(at->__flags & ~POSIX_SPAWN_RESETIDS));
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]) {
    if (!path || !argv) return EINVAL;
    if (trivial(file_actions, attrp)) return spawn_one(pid, path, argv, envp);
    return spawn_forked(pid, path, 0, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]) {
    if (!file || !argv) return EINVAL;
    if (!trivial(file_actions, attrp))
        return spawn_forked(pid, file, 1, file_actions, attrp, argv, envp);
    struct spawn_req q = { pid, argv, envp };
    return walk_path(file, try_spawn, &q);
}
