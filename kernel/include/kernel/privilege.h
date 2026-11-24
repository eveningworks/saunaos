#ifndef KERNEL_PRIVILEGE_H
#define KERNEL_PRIVILEGE_H
// What an identity may do -- stage B of docs/users-design.md: the
// PROMISE_ADMIN syscalls need euid 0, bar the SEAT RIGHTS the person at
// the machine has (CRED_SEAT, abi/cred_abi.h); a signal needs the
// target's owner; and privd's one-shot ELEVATED SPAWN (SYS_CRED_GRANT,
// SPAWN_ELEVATED). kernel/proc/privilege.c has the code.
#include <stdint.h>

struct block_device;

int priv_is_root(void);    // the calling process's euid is 0
int priv_has_seat(void);   // root, or its cred holds CRED_SEAT

// The dispatcher's gate for a PROMISE_ADMIN row: 0, or -EPERM. A seat
// row passes here and its handler checks the rest (priv_seat_mount()).
int priv_admin_check(uint64_t nr);

// A mount, unmount or userfs mount by a caller that is not root: only
// at a point under /media/ or /net/, and -- for a block device -- only a
// removable one (`dev` NULL: an image or a network share). 0 or -EPERM.
int priv_seat_mount(const struct block_device *dev, const char *point);
int priv_seat_point(const char *point);

// May the caller signal the process `tgid`? Linux's rule: root, or the
// caller's real or effective uid is the target's real uid.
int priv_may_signal(int tgid);

// SPAWN_ELEVATED's half: takes the caller's grant for exactly `path` and
// the argument vector `args` (`len` bytes, SPAWN_ARGV's form). 0 when a
// grant matched (it is used up) and *env/*env_len are the environment the
// child gets, the caller's to kfree; -EPERM otherwise.
int priv_grant_take(const char *path, const char *args, uint64_t len, char **env,
                    uint64_t *env_len);

// A process that is gone keeps no grant: pids cycle (slot_free()).
void priv_grant_drop(int tgid);

#endif
