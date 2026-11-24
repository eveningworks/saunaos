// access() -- see <unistd.h> for why F_OK is the only meaningful mode.
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <fs.h>
#include "rt/sys.h"
#include "syscall_abi.h"   // SYS_STATX, SYS_CHOWN, SYS_FCHOWN

// <limits.h> spells PATH_MAX out rather than including this header,
// because it is pulled in by nearly everything and must stay
// dependency-free. This is where the two are held together.
_Static_assert(PATH_MAX == FS_PATH_MAX, "PATH_MAX and FS_PATH_MAX disagree");

int access(const char *path, int mode) {
    struct sys_statx sx;
    // sys_call(), never the runtime's sys_statx: prctl.c says why.
    int64_t sr = sys_call(SYS_STATX, (uint64_t)(uintptr_t)path, (uint64_t)-1, (uint64_t)(uintptr_t)&sx);
    if (sr < 0) {
        if (sr != -ENOSYS) { errno = (int)-sr; return -1; }
        struct sys_stat st;                             // a kernel from before
        return sys_stat(path, &st) == 0 ? 0 : -1;       // owners: then F_OK's answer
    }
    if (mode == F_OK) return 0;
    unsigned want = (mode & R_OK ? 4u : 0) | (mode & W_OK ? 2u : 0) | (mode & X_OK ? 1u : 0);
    unsigned m = sx.st.mode;
    uid_t uid = getuid();
    if (uid == 0) {
        // Root's rule, the kernel's (kernel/fs/fsperm.c).
        if ((want & 1u) && !sx.st.is_dir && !(m & 0111)) { errno = EACCES; return -1; }
        return 0;
    }
    unsigned got;
    if (sx.uid == uid) {
        got = (m >> 6) & 7u;
    } else {
        int in = sx.gid == getgid();
        int n = in ? 0 : getgroups(0, NULL);
        gid_t *g = n > 0 ? malloc((size_t)n * sizeof *g) : NULL;
        if (g) n = getgroups(n, g);
        for (int k = 0; g && k < n && !in; k++) in = g[k] == sx.gid;
        free(g);
        got = in ? (m >> 3) & 7u : m & 7u;
    }
    if ((got & want) != want) { errno = EACCES; return -1; }
    return 0;
}

// chown and fchown go through sys_call() as stat does (prctl.c says why).
static int call_err(int64_t r) {
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int chown(const char *path, uid_t owner, gid_t group) {
    return call_err(sys_call(SYS_CHOWN, (uint64_t)(uintptr_t)path, (uint64_t)(uint32_t)owner,
                             (uint64_t)(uint32_t)group));
}
int lchown(const char *path, uid_t owner, gid_t group) { return chown(path, owner, group); }
int fchown(int fd, uid_t owner, gid_t group) {
    return call_err(sys_call(SYS_FCHOWN, (uint64_t)(int64_t)fd, (uint64_t)(uint32_t)owner,
                             (uint64_t)(uint32_t)group));
}
