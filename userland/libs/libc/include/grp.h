// <grp.h> -- the group database, /etc/group.
//
// `name:x:gid:member,member`, a line each, read on every call as <pwd.h>
// reads /etc/passwd, with the same storage rules: the non-_r forms
// return static storage, the _r forms the caller's (ERANGE when short).
// When the file cannot be read, group 0 (`root`) is still found.
#ifndef _GRP_H
#define _GRP_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct group {
    char   *gr_name;
    char   *gr_passwd;
    gid_t   gr_gid;
    char  **gr_mem;    // NULL-terminated member names
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result);
int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result);

struct group *getgrent(void);
void setgrent(void);
void endgrent(void);

// `user`'s groups: `group` (its primary one) first, then every group
// listing it as a member, into `groups`. Returns how many when they fit
// in *ngroups, else -1 -- and *ngroups is set to how many there are
// either way (glibc's contract).
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups);

#ifdef __cplusplus
}
#endif

#endif
