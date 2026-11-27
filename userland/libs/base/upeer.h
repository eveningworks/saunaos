#ifndef ULIB_UPEER_H
#define ULIB_UPEER_H

// WHO SENT A MESSAGE: the uid, euid and seat right of the process behind
// a uchan sender's pid, for a service deciding whether to do what it was
// asked -- SO_PEERCRED's job, read from the process table (SYS_PROC_INFO).
//
// THE TRAP: a pid is looked up when this is called, not when the message
// was sent. A sender that exited in between is not found (refuse it); one
// whose pid was reused since is someone else. Fine for a client that waits
// for its reply, as every uchan request does, and the reason to call this
// before acting, never after.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct upeer {
    uint32_t uid, euid;
    uint32_t cred_flags;   // CRED_* (abi/cred_abi.h)
    int      seat;         // CRED_SEAT: the person at the machine
};

// 1 and `*out` filled when a thread `pid` is running, else 0.
int upeer_of(int pid, struct upeer *out);

// The rule for a seat's right (netd's card up or down): root, or the
// person at the machine. 0 for a pid that is gone.
int upeer_root_or_seat(int pid);

#ifdef __cplusplus
}
#endif

#endif
