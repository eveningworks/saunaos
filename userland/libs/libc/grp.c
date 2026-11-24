// The group database, /etc/group (<grp.h>).
#include <grp.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "etcdb.h"

#define GROUP_FILE "/etc/group"
enum { F_NAME, F_PASSWD, F_GID, F_MEM, NF };

static char *const ROOT[NF] = { "root", "x", "0", "" };

static size_t members_of(const char *m) {
    if (!*m) return 0;
    size_t n = 1;
    for (; *m; m++) n += *m == ',';
    return n;
}

static size_t need_of(char *const *f) {
    size_t strs = strlen(f[F_NAME]) + strlen(f[F_PASSWD]) + strlen(f[F_MEM]) + 3;
    // The member vector is pointer-aligned at the front of the buffer.
    return (members_of(f[F_MEM]) + 1) * sizeof(char *) + strs + sizeof(char *);
}

// Copies the fields into `buf` -- the member pointers first, aligned,
// then the strings -- and points `grp` at them. ERANGE when short, -1
// for a malformed gid.
static int fill(char *const *f, struct group *grp, char *buf, size_t buflen) {
    unsigned gid;
    if (!etcdb_id(f[F_GID], &gid)) return -1;
    if (need_of(f) > buflen) return ERANGE;
    uintptr_t a = ((uintptr_t)buf + sizeof(char *) - 1) & ~(uintptr_t)(sizeof(char *) - 1);
    char **mem = (char **)a;
    size_t nm = members_of(f[F_MEM]);
    char *s = (char *)(mem + nm + 1);
    size_t n = strlen(f[F_NAME]) + 1;
    memcpy(s, f[F_NAME], n); grp->gr_name = s; s += n;
    n = strlen(f[F_PASSWD]) + 1;
    memcpy(s, f[F_PASSWD], n); grp->gr_passwd = s; s += n;
    n = strlen(f[F_MEM]) + 1;
    memcpy(s, f[F_MEM], n);
    size_t k = 0;
    if (nm) {
        mem[k++] = s;
        for (char *p = s; *p; p++)
            if (*p == ',') { *p = '\0'; mem[k++] = p + 1; }
    }
    mem[k] = NULL;
    grp->gr_mem = mem;
    grp->gr_gid = gid;
    return 0;
}

// A line with a malformed gid names nobody, and the search goes on past it.
static int matches(char *const *f, const char *name, gid_t gid) {
    unsigned g;
    if (!etcdb_id(f[F_GID], &g)) return 0;
    return name ? strcmp(f[F_NAME], name) == 0 : g == gid;
}

static int lookup(const char *name, gid_t gid, struct group *grp, char *buf, size_t buflen,
                  struct group **result) {
    static char *sbuf;
    static size_t scap;
    *result = NULL;
    struct etcdb db = { 0, 0, 0 };
    char *f[NF];
    int rc = 0, found = 0;
    while (etcdb_next(&db, GROUP_FILE, f, NF))
        if (matches(f, name, gid)) { found = 1; break; }
    char *const *src = f;
    if (!found && !db.f && matches(ROOT, name, gid)) { src = ROOT; found = 1; }
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
        rc = fill(src, grp, buf, buflen);
        if (rc == 0) *result = grp;
        else if (rc < 0) rc = 0;
    }
out:
    etcdb_close(&db);
    free(db.line);
    return rc;
}

static struct group g_gr;

struct group *getgrgid(gid_t gid) {
    struct group *r;
    int rc = lookup(NULL, gid, &g_gr, NULL, 0, &r);
    if (rc) errno = rc;
    return r;
}

struct group *getgrnam(const char *name) {
    if (!name) return NULL;
    struct group *r;
    int rc = lookup(name, 0, &g_gr, NULL, 0, &r);
    if (rc) errno = rc;
    return r;
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result) {
    return lookup(NULL, gid, grp, buf, buflen, result);
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result) {
    *result = NULL;
    return name ? lookup(name, 0, grp, buf, buflen, result) : 0;
}

// --- walking the file -------------------------------------------------------

static struct etcdb g_ent;
static char *g_ent_buf;
static size_t g_ent_cap;

void setgrent(void) { etcdb_rewind(&g_ent); }
void endgrent(void) { etcdb_close(&g_ent); }

struct group *getgrent(void) {
    char *f[NF];
    while (etcdb_next(&g_ent, GROUP_FILE, f, NF)) {
        size_t need = need_of(f);
        if (need > g_ent_cap) {
            char *nb = realloc(g_ent_buf, need);
            if (!nb) { errno = ENOMEM; return NULL; }
            g_ent_buf = nb;
            g_ent_cap = need;
        }
        if (fill(f, &g_gr, g_ent_buf, g_ent_cap) == 0) return &g_gr;
    }
    return NULL;
}

static int member(const char *list, const char *user) {
    size_t ul = strlen(user);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == ul && memcmp(p, user, n) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups) {
    int cap = *ngroups, n = 0;
    if (n < cap) groups[n] = group;
    n++;
    struct etcdb db = { 0, 0, 0 };
    char *f[NF];
    while (etcdb_next(&db, GROUP_FILE, f, NF)) {
        unsigned g;
        if (!etcdb_id(f[F_GID], &g) || g == group || !member(f[F_MEM], user)) continue;
        int dup = 0;
        for (int i = 1; i < n && i < cap; i++) dup |= groups[i] == g;
        if (dup) continue;
        if (n < cap) groups[n] = g;
        n++;
    }
    etcdb_close(&db);
    free(db.line);
    *ngroups = n;
    return n <= cap ? n : -1;
}
