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
    uint32_t flags;      // CRED_* below
};

// CRED_SEAT: the process belongs to the person AT the machine -- the
// desktop, the console shell, a VT shell, and everything they start.
// It is what lets an ordinary account use the SEAT RIGHTS (power off,
// screen off, removable drives; docs/users-design.md, stage B) without
// asking, and what privd requires before it asks at all. Windows'
// INTERACTIVE group in the token; Linux asks logind which session is
// active. Set only through SPAWN_CREDS, so only by root (init's Seat=).
#define CRED_SEAT 1u
#define CRED_FLAGS_ALL CRED_SEAT

// SYS_CRED_GRANT's request (abi/syscall_abi.h): process `pid` may spawn
// `path` as root ONCE, with exactly the `args_len` bytes of `args` as
// its argument vector (SPAWN_ARGV's form, argv[0] included), within 30
// seconds -- the spawn it makes with SPAWN_ELEVATED. privd's half of an
// elevated spawn; root only.
//
// THE CHILD GETS `env`, NOT THE ASKER'S ENVIRONMENT: `env_len` bytes of
// strings ending in an empty one (SYS_SPAWN's form) -- sudo's env_reset,
// so a PATH or HOME the asker chose never reaches a root program.
struct cred_grant {
    int32_t     pid;
    uint32_t    args_len;
    const char *path;
    const char *args;
    const char *env;
    uint32_t    env_len;
    uint32_t    reserved;   // 0
};

// The most supplementary groups one process carries: Linux's
// NGROUPS_MAX. An ABI bound, for the copy SPAWN_CREDS makes; the kernel
// allocates the list to fit.
#define CRED_GROUPS_MAX 65536

#endif
