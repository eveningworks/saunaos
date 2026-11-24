// The user database, /etc/passwd (<pwd.h>).
#include <pwd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "etcdb.h"

#define PASSWD_FILE "/etc/passwd"
enum { F_NAME, F_PASSWD, F_UID, F_GID, F_GECOS, F_DIR, F_SHELL, NF };

// Root, when the file cannot be read at all -- a sandbox that did not
// unveil it, or a disk from before it shipped. Root exists on every Unix;
// no other name is invented.
static char *const ROOT[NF] = { "root", "x", "0", "0", "", "/", "/bin/tosh" };

// Copies the fields into `buf` end to end and points `pwd` at them.
// ERANGE when `buf` is short. 0 for a malformed id, which is skipped.
static int fill(char *const *f, struct passwd *pwd, char *buf, size_t buflen) {
    unsigned uid, gid;
    if (!etcdb_id(f[F_UID], &uid) || !etcdb_id(f[F_GID], &gid)) return -1;
    static const int STR[] = { F_NAME, F_PASSWD, F_GECOS, F_DIR, F_SHELL };
    char **dst[] = { &pwd->pw_name, &pwd->pw_passwd, &pwd->pw_gecos, &pwd->pw_dir, &pwd->pw_shell };
    size_t need = 0;
    for (size_t i = 0; i < sizeof STR / sizeof STR[0]; i++) need += strlen(f[STR[i]]) + 1;
    if (need > buflen) return ERANGE;
    for (size_t i = 0; i < sizeof STR / sizeof STR[0]; i++) {
        size_t n = strlen(f[STR[i]]) + 1;
        memcpy(buf, f[STR[i]], n);
        *dst[i] = buf;
        buf += n;
    }
    pwd->pw_uid = uid;
    pwd->pw_gid = gid;
    return 0;
}

static size_t need_of(char *const *f) {
    return strlen(f[F_NAME]) + strlen(f[F_PASSWD]) + strlen(f[F_GECOS]) +
           strlen(f[F_DIR]) + strlen(f[F_SHELL]) + 5;
}

// A line with a malformed id names nobody, and the search goes on past it.
static int matches(char *const *f, const char *name, uid_t uid) {
    unsigned u, g;
    if (!etcdb_id(f[F_UID], &u) || !etcdb_id(f[F_GID], &g)) return 0;
    return name ? strcmp(f[F_NAME], name) == 0 : u == uid;
}

// The first entry naming `name` (or, with name NULL, `uid`), filled into
// the caller's storage -- or into the static one when `buf` is NULL.
static int lookup(const char *name, uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
                  struct passwd **result) {
    static char *sbuf;
    static size_t scap;
    *result = NULL;
    struct etcdb db = { 0, 0, 0 };
    char *f[NF];
    int rc = 0, found = 0, opened;
    while (etcdb_next(&db, PASSWD_FILE, f, NF)) {
        if (!matches(f, name, uid)) continue;
        found = 1;
        break;
    }
    opened = db.f != NULL;
    char *const *src = f;
    if (!found && !opened && matches(ROOT, name, uid)) { src = ROOT; found = 1; }
    if (found) {
        if (!buf) {
            size_t need = need_of(src);
            if (need > scap) {
                char *nb = realloc(sbuf, need);
                if (!nb) { rc = ENOMEM; goto out; }
                sbuf = nb;
                scap = need;
            }
            buf = sbuf;
            buflen = scap;
        }
        rc = fill(src, pwd, buf, buflen);
        if (rc == 0) *result = pwd;
        else if (rc < 0) rc = 0;   // a malformed line names nobody
    }
out:
    etcdb_close(&db);
    free(db.line);
    return rc;
}

static struct passwd g_pw;

struct passwd *getpwuid(uid_t uid) {
    struct passwd *r;
    int rc = lookup(NULL, uid, &g_pw, NULL, 0, &r);
    if (rc) errno = rc;
    return r;
}

struct passwd *getpwnam(const char *name) {
    if (!name) return NULL;
    struct passwd *r;
    int rc = lookup(name, 0, &g_pw, NULL, 0, &r);
    if (rc) errno = rc;
    return r;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
    return lookup(NULL, uid, pwd, buf, buflen, result);
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
    *result = NULL;
    return name ? lookup(name, 0, pwd, buf, buflen, result) : 0;
}

// --- walking the file -------------------------------------------------------

static struct etcdb g_ent;
static char *g_ent_buf;
static size_t g_ent_cap;

void setpwent(void) { etcdb_rewind(&g_ent); }

void endpwent(void) {
    etcdb_close(&g_ent);
}

struct passwd *getpwent(void) {
    char *f[NF];
    while (etcdb_next(&g_ent, PASSWD_FILE, f, NF)) {
        size_t need = need_of(f);
        if (need > g_ent_cap) {
            char *nb = realloc(g_ent_buf, need);
            if (!nb) { errno = ENOMEM; return NULL; }
            g_ent_buf = nb;
            g_ent_cap = need;
        }
        if (fill(f, &g_pw, g_ent_buf, g_ent_cap) == 0) return &g_pw;
    }
    return NULL;
}
