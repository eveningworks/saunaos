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
// desktop runs as root or names no account. `User=@session` (users D2b)
// is the account the running desktop has, else the first person's.
int uusername_desktop(unsigned *uid, unsigned *gid);

// THE SESSION AT THE SEAT, now: the account of a person's process holding
// the seat (CRED_SEAT, uid 1000 and up) -- logind's active session. 1 and
// its uid and primary group, or 0 while there is none: the login screen
// (the greeter's account is no person's), a text boot nobody signed in to.
// One session at a time (users D3a), so any such process names it. A walk
// of the process table: call it every second or so, not per frame.
int uusername_session(unsigned *uid, unsigned *gid);

#ifdef __cplusplus
}
#endif

#endif
