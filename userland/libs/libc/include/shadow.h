#ifndef _SHADOW_H
#define _SHADOW_H

// The password database, /etc/shadow -- root's and 0600, so these answer
// nothing (NULL, errno EACCES) to anyone else (docs/users-design.md, D2).
// Linux's nine colon-separated fields; an empty number is -1. An empty
// hash is an account with no password, "!" or "*" one that cannot sign in
// by password. Checking a password is privd's (base/upriv.h): no other
// program links a password hash.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spwd {
    char *sp_namp;     // the account's name
    char *sp_pwdp;     // its hash ("$y$..."), "" for none, "!..." locked
    long  sp_lstchg;   // the day of the last change, days since 1970
    long  sp_min, sp_max, sp_warn, sp_inact, sp_expire;
    unsigned long sp_flag;
};

// The account's entry, in static storage the next call overwrites. NULL
// with errno ENOENT (no file, or no line for it), EACCES (not root), or
// EPERM: a file root does not own with no group or other bits is NOT
// read -- /etc is the desktop's to add files to until users D3.
struct spwd *getspnam(const char *name);

#ifdef __cplusplus
}
#endif

#endif
