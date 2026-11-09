// The one-entry user database -- <pwd.h> says why it has one entry.
#include <pwd.h>
#include <errno.h>
#include <string.h>

static struct passwd the_user = {
    .pw_name = "root", .pw_passwd = "x", .pw_uid = 0, .pw_gid = 0,
    .pw_gecos = "", .pw_dir = "/", .pw_shell = "/bin/tosh",
};

struct passwd *getpwuid(uid_t uid) { return uid == 0 ? &the_user : NULL; }

struct passwd *getpwnam(const char *name) {
    return name && strcmp(name, the_user.pw_name) == 0 ? &the_user : NULL;
}

// Copies each string of the entry into buf, end to end.
static int fill(struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) {
    *result = NULL;
    char *const *src[] = { &the_user.pw_name, &the_user.pw_passwd, &the_user.pw_gecos,
                           &the_user.pw_dir, &the_user.pw_shell };
    char **dst[] = { &pwd->pw_name, &pwd->pw_passwd, &pwd->pw_gecos,
                     &pwd->pw_dir, &pwd->pw_shell };
    size_t need = 0;
    for (size_t i = 0; i < sizeof src / sizeof src[0]; i++) need += strlen(*src[i]) + 1;
    if (need > buflen) return ERANGE;
    for (size_t i = 0; i < sizeof src / sizeof src[0]; i++) {
        size_t n = strlen(*src[i]) + 1;
        memcpy(buf, *src[i], n);
        *dst[i] = buf;
        buf += n;
    }
    pwd->pw_uid = the_user.pw_uid;
    pwd->pw_gid = the_user.pw_gid;
    *result = pwd;
    return 0;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
    if (uid != 0) { *result = NULL; return 0; }
    return fill(pwd, buf, buflen, result);
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
    if (!getpwnam(name)) { *result = NULL; return 0; }
    return fill(pwd, buf, buflen, result);
}
