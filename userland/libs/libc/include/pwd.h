// <pwd.h> -- the user database, which has ONE entry.
//
// This system has one user and no /etc/passwd (<unistd.h>'s "who we
// are"): uid 0, which every Unix names `root`, with HOME's directory (`/`,
// what init sets) and tosh as its login shell. So getpwuid(0) and
// getpwnam("root") find it, and any other uid or name finds NOTHING --
// NULL with errno untouched, which is POSIX's "no such entry", not an
// error. A database that answered every name would expand `~bob` to a
// home nobody has.
//
// The entry's strings are static and the same on every call; the _r
// forms copy them into the caller's buffer (ERANGE when it is short).
#ifndef _PWD_H
#define _PWD_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
    char  *pw_name;
    char  *pw_passwd;   // "x": there is no password, and no shadow file
    uid_t  pw_uid;
    gid_t  pw_gid;
    char  *pw_gecos;
    char  *pw_dir;
    char  *pw_shell;
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
// 0 with *result set (NULL when there is no such entry), or an errno.
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result);
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result);

#ifdef __cplusplus
}
#endif

#endif
