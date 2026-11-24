#ifndef ABI_CRED_ABI_H
#define ABI_CRED_ABI_H
// Who a process is: its user and group ids, read whole by SYS_GETCRED
// and set for a child by SYS_SPAWN's SPAWN_CREDS (abi/syscall_abi.h).
// docs/users-design.md has the plan; stage A carries the identity and
// enforces nothing but who may start a process as someone else.
//
// Unix's shape -- real, effective and saved ids, getresuid(2)'s three --
// because ported programs call getuid(), geteuid() and getgroups(). Root
// is uid 0. A process the kernel started, and every one descended from
// it without SPAWN_CREDS, is root with no supplementary groups.
#include <stdint.h>

// SYS_GETCRED's record. `ngroups` is how many supplementary groups the
// process HAS, whatever the caller's buffer took.
struct cred_info {
    uint32_t ruid, euid, suid;
    uint32_t rgid, egid, sgid;
    uint32_t ngroups;
    uint32_t reserved;   // 0
};

// The most supplementary groups one process carries: Linux's
// NGROUPS_MAX. An ABI bound, for the copy SPAWN_CREDS makes; the kernel
// allocates the list to fit.
#define CRED_GROUPS_MAX 65536

#endif
