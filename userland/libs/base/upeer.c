// base/upeer.h.
#include "base/upeer.h"
#include <string.h>
#include "rt/sys.h"
#include "proc_info.h"
#include "cred_abi.h"

int upeer_of(int pid, struct upeer *out) {
    struct proc_info pi;
    if (pid <= 0) return 0;
    for (int i = 0; sys_proc_info(i, &pi) == 0; i++) {
        if (pi.pid != pid) continue;   // any thread: the cred is its group's
        memset(out, 0, sizeof *out);
        out->uid = pi.uid;
        out->euid = pi.euid;
        out->cred_flags = pi.cred_flags;
        out->seat = (pi.cred_flags & CRED_SEAT) != 0;
        return 1;
    }
    return 0;
}

int upeer_root_or_seat(int pid) {
    struct upeer p;
    return upeer_of(pid, &p) && (p.euid == 0 || p.seat);
}
