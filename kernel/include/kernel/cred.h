#ifndef KERNEL_CRED_H
#define KERNEL_CRED_H
// A process's identity -- the kernel half of SYS_GETCRED and SPAWN_CREDS
// (abi/cred_abi.h has the contract, kernel/proc/cred.c the code).
//
// NULL MEANS ROOT WITH NO GROUPS, as a NULL sandbox means unrestricted:
// the kernel context and everything it starts carry none. A cred lives
// on the thread-group LEADER, is SHARED (refcounted) between a process
// and the children that inherited it, and is NEVER CHANGED once made --
// a different identity is a new cred (Linux's struct cred rule), so a
// reader holding the pointer needs no lock against a writer.
#include <stdint.h>

struct cred;
struct cred_info;

struct cred *cred_get(struct cred *c);   // +1 reference; NULL passes through
void cred_put(struct cred *c);           // -1; frees at zero

// The record SYS_GETCRED answers for `c`; `groups`, when not NULL, gets
// the first `cap` group ids. NULL `c` is root.
void cred_describe(const struct cred *c, struct cred_info *out, uint32_t *groups, uint32_t cap);
uint32_t cred_euid(const struct cred *c);
uint32_t cred_ruid(const struct cred *c);
uint32_t cred_egid(const struct cred *c);
uint32_t cred_rgid(const struct cred *c);

// The current PROCESS's cred (its leader's); NULL in the kernel context.
struct cred *cred_current(void);

// SPAWN_CREDS's half, the sandbox arm's shape (kernel/sandbox.h): armed
// by the asking THREAD with an identity already built, consumed by its
// own spawn. -ENOMEM, or -EBUSY when every arm is taken -- the caller
// then starts no child.
int  cred_arm_spawn(uint32_t uid, uint32_t gid, const uint32_t *groups, uint32_t ngroups);
void cred_disarm(void);
// The same for a thread that is GONE (slot_free()): pids cycle, so an arm
// its spawn never consumed would make some later thread's child someone
// else -- root, from a root broker's arm.
void cred_disarm_tid(int tid);

// What a child spawned by the CURRENT thread starts as: the armed cred
// if this thread armed one, the caller's otherwise. A reference the
// child's slot owns. Never fails -- the arm was built before the spawn.
struct cred *cred_for_child(void);

#endif
