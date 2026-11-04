// Resource limits: RLIMIT_DATA is the kernel's (SYS_GETRLIMIT and
// SYS_SETRLIMIT); every other one is unlimited, and setrlimit() says so
// rather than accepting a number nothing enforces. See <sys/resource.h>.
#include <sys/resource.h>
#include <errno.h>
#include "rt/sys.h"

_Static_assert(RLIMIT_DATA == SYS_RLIMIT_DATA && RLIM_INFINITY == SYS_RLIM_INFINITY,
               "<sys/resource.h> passes RLIMIT_DATA straight through");

int getrlimit(int resource, struct rlimit *rlp) {
    if (resource < 0 || resource >= RLIMIT_NLIMITS || !rlp) {
        errno = EINVAL;
        return -1;
    }
    if (resource == RLIMIT_DATA) {
        struct sys_rlimit k;
        if (sys_getrlimit(SYS_RLIMIT_DATA, &k) < 0) return -1;
        rlp->rlim_cur = k.cur;
        rlp->rlim_max = k.max;
        return 0;
    }
    rlp->rlim_cur = RLIM_INFINITY;
    rlp->rlim_max = RLIM_INFINITY;
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlp) {
    if (resource < 0 || resource >= RLIMIT_NLIMITS || !rlp) {
        errno = EINVAL;
        return -1;
    }
    if (resource == RLIMIT_DATA) {
        struct sys_rlimit k = { rlp->rlim_cur, rlp->rlim_max };
        return sys_setrlimit(SYS_RLIMIT_DATA, &k);
    }
    // Setting a limit to the value it already has is not a change --
    // the same rule the settings registry follows, and it is what makes
    // `ulimit -f unlimited` succeed instead of failing pointlessly.
    if (rlp->rlim_cur == RLIM_INFINITY && rlp->rlim_max == RLIM_INFINITY)
        return 0;
    errno = EPERM;
    return -1;
}

int setpriority(int which, int who, int value) {
    return sys_setpriority(which, who, value);
}

int getpriority(int which, int who) {
    return sys_getpriority(which, who);
}
