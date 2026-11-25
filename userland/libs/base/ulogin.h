#ifndef ULIB_ULOGIN_H
#define ULIB_ULOGIN_H

// An ACCOUNT'S IDENTITY FOR A SPAWN: its uid, primary group, every
// /etc/group line naming it, and the login variables -- HOME, USER,
// LOGNAME, SHELL and XDG's four base directories under its home --
// replacing any the caller's environment had. pam_systemd's and a login
// shell's job on Linux; here init's (a `User=` service) and /bin/login's
// (a text or telnet sign-in), from this one place. ROOT ALONE may spawn
// with it (SPAWN_CREDS).

#include "rt/sys.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ULOGIN_VARS 8   // HOME USER LOGNAME SHELL + XDG_{CONFIG,DATA,STATE,CACHE}_HOME

struct ulogin_creds {
    gid_t *groups;
    char **env;               // environ with the login variables replaced
    char  *strs[ULOGIN_VARS];
    char  *home;              // the account's home, owned
    char  *shell;             // its shell, owned
};

// Fills `o` (SPAWN_CREDS, uid, gid, groups, env, `cred_flags`) and `c` for
// `account`. 0, or -1 when the account is unknown or memory ran out --
// then nothing may be spawned in its name. Release `c` after the spawn.
int  ulogin_creds(const char *account, unsigned cred_flags, struct sys_spawn_opts *o,
                  struct ulogin_creds *c);
void ulogin_creds_release(struct ulogin_creds *c);

#ifdef __cplusplus
}
#endif

#endif
