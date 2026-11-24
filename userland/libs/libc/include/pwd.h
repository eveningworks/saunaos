// <pwd.h> -- the user database, /etc/passwd.
//
// Read from the file on every call, as glibc's files backend does, so an
// edit is seen at once. `name:x:uid:gid:gecos:home:shell`, a line each;
// a malformed line names nobody. When the file cannot be read at all (a
// sandbox that did not unveil it), root -- uid 0, home `/` -- is still
// found, and nobody else. No such entry is NULL with errno untouched,
// POSIX's "not found", not an error.
//
// The non-_r forms return static storage overwritten by the next call
// of any of them; the _r forms copy into the caller's buffer (ERANGE
// when it is short).
#ifndef _PWD_H
#define _PWD_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
    char  *pw_name;
    char  *pw_passwd;   // "x": there are no passwords yet (docs/users-design.md, stage D)
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

// Every entry in turn; setpwent() starts over, endpwent() closes the file.
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);

#ifdef __cplusplus
}
#endif

#endif
