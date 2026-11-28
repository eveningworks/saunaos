// lsshm -- the shared-memory namespace: every named object, who made it,
// and how many references it has.
//
// It exists because a namespace nothing can list is a namespace you
// reason about instead of reading. `soundd`'s clients ARE this table
// (each is an object named snd.<pid>), so "which programs is the daemon
// mixing, and did a dead one leave its ring behind" is one command
// rather than an inference from the daemon's log.
//
// The same shape as lsdrv and lsblk: one QUERY class, one row each.
#include <stdio.h>
#include "rt/sys.h"
#include "query_abi.h"

int main(void) {
    struct query_shm r;
    int n = 0;

    QUERY_FOREACH(QUERY_SHM, r, i) {
        if (!n++)
            printf("NAME                            BYTES  REFS  CREATOR  OPEN\n");
        // WHO MAY OPEN IT: what a refused shm_open asks first.
        char open[24];
        if (r.flags & QUERY_SHM_PUBLIC) snprintf(open, sizeof open, "anyone");
        else if (r.flags & QUERY_SHM_ACCOUNT) snprintf(open, sizeof open, "uid %u", (unsigned)r.uid);
        else snprintf(open, sizeof open, "creator");
        printf("%-28s %8llu  %4u  %7d  %s%s\n", r.name,
               (unsigned long long)r.bytes, r.refs, r.creator_pid, open,
               (r.flags & QUERY_SHM_UNLINKED) ? "  (unlinked)" : "");
    }
    if (!n) printf("no shared-memory objects\n");
    return 0;
}
