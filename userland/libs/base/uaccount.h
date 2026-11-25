#ifndef ULIB_UACCOUNT_H
#define ULIB_UACCOUNT_H
// Starting a program AS AN ACCOUNT: its uid, primary group and every
// group /etc/group names it in, with HOME, USER, LOGNAME and SHELL from
// its passwd line and XDG's four base directories under that HOME --
// what `runuser -u NAME` and login(1) (with pam_systemd) hand a child.
// The spawn is SPAWN_CREDS, so only root's succeeds. init builds the
// same identity for a `User=` service in its own copy (it links no
// libraries, userland/services/init/component.mk).
#include "rt/sys.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UACCOUNT_NVARS 8   // HOME USER LOGNAME SHELL + XDG_{CONFIG,DATA,STATE,CACHE}_HOME

struct uaccount_spawn {
    gid_t *groups;
    char **env;        // `base` with the login variables replaced
    char  *strs[UACCOUNT_NVARS];
    const char *home;  // the account's home (inside strs[0])
};

// Fill `o`'s identity and environment for account `name`; `base` is the
// environment the other variables come from (environ, usually). 0, or
// -1 with errno ENOENT (no such account) or ENOMEM -- then nothing is
// held. Release with uaccount_spawn_release() after the spawn.
int uaccount_spawn_init(struct uaccount_spawn *s, struct sys_spawn_opts *o, const char *name,
                        char **base);
void uaccount_spawn_release(struct uaccount_spawn *s);

#ifdef __cplusplus
}
#endif
#endif
