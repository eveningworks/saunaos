#ifndef ULIB_UUSERNAME_H
#define ULIB_UUSERNAME_H

// A user id's NAME for a listing -- `ps --user`'s and Task Manager's User
// column: getpwuid() once per uid, then from a cache, since <pwd.h> reads
// /etc/passwd on every call and a listing has a few uids across many rows.
// A uid with no name, or a name of `width` bytes or more, is its number
// (procps' rule: a cut name could be someone else's).
//
// The cache lives for the process: an account renamed while a program
// runs keeps its old name in that program, as procps' does between reads.

#ifdef __cplusplus
extern "C" {
#endif

// Static storage owned by the cache; valid until the process ends.
// `width` 0 means no limit.
const char *uusername(unsigned uid, unsigned width);

// THE DESKTOP'S ACCOUNT -- the `User=` init starts toywm as
// (/etc/services.d/toywm), looked up in /etc/passwd: whom a root service
// mounts a stick or a share FOR, as udisks2 mounts for the session's
// user (users stage C). 1 and its uid and primary group, or 0 when the
// desktop runs as root or names no account. Stage D replaces it with the
// active session's.
int uusername_desktop(unsigned *uid, unsigned *gid);

#ifdef __cplusplus
}
#endif

#endif
