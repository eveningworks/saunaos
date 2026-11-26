// A person's account added, removed and changed -- see accounts.h.
//
// /etc/passwd and /etc/group are each REPLACED WHOLE: written beside
// themselves, synced, renamed over (shadow-utils' shape), so a power cut
// leaves the old file or the new one. The three files are not one
// transaction, so their ORDER is the invariant: A PASSWD LINE NEVER
// STANDS WITHOUT ITS SHADOW LINE -- a missing one reads as "no password"
// (pass.c), so an account caught between would open with "". An add
// writes shadow, group, then passwd; a removal passwd, group, then
// shadow. A cut between leaves a hash or a group nobody uses.
#include "accounts.h"
#include "pass.h"
#include "rt/sys.h"
#include "proc_info.h"
#include "base/ufileop.h"
#include "base/usetting.h"
#include "base/uusers.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PASSWD "/etc/passwd"
#define GROUP  "/etc/group"
#define SKEL   "/etc/skel"
#define HOMES  "/home/"
#define SHELL  "/bin/tosh"
#define ADMIN_GROUP "wheel"
#define SET_AUTOLOGIN "login.autologin"
#define ID_MIN 1000u
#define ID_MAX 59999u   // useradd's UID_MAX; 65534 stays nobody's

static struct ufileop g_op;   // tens of KB: never on a frame (ufileop.h)

// The whole file, NUL-ended, malloc'd; NULL with errno when unreadable --
// then nothing is written, or the other lines would be lost.
static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 1024, n = 0;
    char *s = malloc(cap);
    while (s) {
        size_t k = fread(s + n, 1, cap - n - 1, f);
        n += k;
        if (k == 0) {
            if (ferror(f)) { free(s); s = NULL; errno = EIO; }
            break;
        }
        if (n + 1 == cap) {
            char *ns = realloc(s, cap * 2);
            if (!ns) { free(s); s = NULL; errno = ENOMEM; break; }
            s = ns;
            cap *= 2;
        }
    }
    fclose(f);
    if (s) s[n] = '\0';
    return s;
}

static int write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t k = write(fd, s, n);
        if (k <= 0) return -1;
        s += k;
        n -= (size_t)k;
    }
    return 0;
}

// `path` replaced by `text`, root's and 0644.
static int replace(const char *path, const char *text) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    // O_EXCL after the unlink: a file someone else made at that name
    // (the desktop may add files to /etc) is never the one written.
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    int rc = fd < 0 ? -errno : 0;
    if (!rc && (fchown(fd, 0, 0) < 0 || fchmod(fd, 0644) < 0)) rc = -errno;
    if (!rc && (write_all(fd, text, strlen(text)) < 0 || fsync(fd) < 0)) rc = -EIO;
    if (fd >= 0) close(fd);
    if (!rc && rename(tmp, path) < 0) rc = -errno;
    if (rc) unlink(tmp);
    return rc;
}

// A growing output buffer.
struct out { char *s; size_t n, cap; int bad; };

static void put(struct out *o, const char *s, size_t n) {
    if (o->bad) return;
    if (o->n + n + 1 > o->cap) {
        size_t nc = o->cap ? o->cap : 1024;
        while (o->n + n + 1 > nc) nc *= 2;
        char *ns = realloc(o->s, nc);
        if (!ns) { o->bad = 1; return; }
        o->s = ns;
        o->cap = nc;
    }
    memcpy(o->s + o->n, s, n);
    o->n += n;
    o->s[o->n] = '\0';
}
static void puts0(struct out *o, const char *s) { put(o, s, strlen(s)); }

// The line's first field is `name`.
static int names(const char *line, size_t len, const char *name) {
    size_t k = strlen(name);
    return len > k && !strncmp(line, name, k) && line[k] == ':';
}

// A group line (`len` bytes, no newline) with `name` added to or taken
// from its member list -- the fourth field, comma-separated.
static void put_group_line(struct out *o, const char *line, size_t len, const char *name, int add) {
    const char *p = line;
    int colons = 0;
    while (p < line + len && colons < 3) if (*p++ == ':') colons++;
    if (colons < 3) { put(o, line, len); return; }   // malformed: kept as it was
    put(o, line, (size_t)(p - line));
    int first = 1, had = 0;
    size_t k = strlen(name);
    for (const char *m = p; m < line + len; ) {
        const char *e = memchr(m, ',', (size_t)(line + len - m));
        if (!e) e = line + len;
        size_t ml = (size_t)(e - m);
        int is = ml == k && !strncmp(m, name, k);
        had |= is;
        if (ml && !is) {
            if (!first) put(o, ",", 1);
            put(o, m, ml);
            first = 0;
        }
        m = e + (e < line + len);
    }
    if (add) {
        if (!first) put(o, ",", 1);
        puts0(o, name);
    }
    (void)had;
}

// /etc/group rewritten: `name` added to (`add` 1) or taken from (0) the
// member list of `group` ("" for every group), and the line of group
// `drop` removed (NULL for none); `append` added at the end (NULL for none).
static int rewrite_group(const char *group, const char *name, int add, const char *drop,
                         const char *append) {
    char *cur = slurp(GROUP);
    if (!cur) return -errno;
    struct out o = { 0 };
    for (char *line = cur; *line; ) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (drop && names(line, len, drop)) {
            // dropped
        } else if (name && (!group[0] || names(line, len, group))) {
            put_group_line(&o, line, len, name, add && group[0]);
            put(&o, "\n", 1);
        } else if (len) {
            put(&o, line, len);
            put(&o, "\n", 1);
        }
        line += len + (nl != NULL);
    }
    if (append) puts0(&o, append);
    free(cur);
    int rc = o.bad ? -ENOMEM : replace(GROUP, o.s ? o.s : "");
    free(o.s);
    return rc;
}

// /etc/passwd rewritten: the line of `name` replaced by `with` (NULL
// removes it), or `with` appended when `name` is NULL.
static int rewrite_passwd(const char *name, const char *with) {
    char *cur = slurp(PASSWD);
    if (!cur) return -errno;
    struct out o = { 0 };
    for (char *line = cur; *line; ) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (name && names(line, len, name)) {
            if (with) puts0(&o, with);
        } else if (len) {
            put(&o, line, len);
            put(&o, "\n", 1);
        }
        line += len + (nl != NULL);
    }
    if (!name && with) puts0(&o, with);
    free(cur);
    int rc = o.bad ? -ENOMEM : replace(PASSWD, o.s ? o.s : "");
    free(o.s);
    return rc;
}

static int uid_taken(uid_t id) { return getpwuid(id) != NULL; }
static int gid_taken(gid_t id) { return getgrgid(id) != NULL; }

// The next id above every person's uid and gid in use -- max+1, as
// useradd picks, so a removed account's files never fall to a newcomer.
static uid_t next_id(void) {
    uid_t top = ID_MIN - 1;
    // A home kept from a removed account still names its uid: above that
    // too, or the next person would be given its files.
    DIR *d = opendir("/home");
    struct dirent *e;
    char p[sizeof HOMES + NAME_MAX];   // HOMES and a d_name
    struct stat st;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || (size_t)snprintf(p, sizeof p, "/home/%s", e->d_name) >= sizeof p) continue;
        if (stat(p, &st) == 0 && st.st_uid >= ID_MIN && st.st_uid <= ID_MAX && st.st_uid > top) top = st.st_uid;
    }
    if (d) closedir(d);
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent())) if (pw->pw_uid >= ID_MIN && pw->pw_uid <= ID_MAX && pw->pw_uid > top) top = pw->pw_uid;
    endpwent();
    struct group *g;
    setgrent();
    while ((g = getgrent())) if (g->gr_gid >= ID_MIN && g->gr_gid <= ID_MAX && g->gr_gid > top) top = g->gr_gid;
    endgrent();
    return top + 1;
}

// People's accounts in wheel, `but` left out of the count; -1 when the
// list cannot be read (a caller refuses then, never assumes).
static int admins_but(const char *but) {
    struct uuser *u;
    int n = uusers_list(&u), k = 0;
    for (int i = 0; i < n; i++) k += u[i].admin && strcmp(u[i].name, but) != 0;
    uusers_free(u, n);
    return n < 0 ? -1 : k;
}

// Any process running as `uid`: signed in, or a service of it.
static int has_processes(uid_t uid) {
    struct proc_info pi;
    for (int i = 0; sys_proc_info(i, &pi) == 0; i++)
        if (pi.pid && (pi.uid == uid || pi.euid == uid)) return 1;
    return 0;
}

// A home privd may make or delete: one folder directly in /home.
static int home_ok(const char *h) {
    size_t k = strlen(HOMES);
    return h && !strncmp(h, HOMES, k) && h[k] && !strchr(h + k, '/') && strcmp(h + k, ".") && strcmp(h + k, "..");
}

// The last-administrator guard: 0 when `name` may stop being one.
static int not_last_admin(const char *name) {
    if (!uusers_is_admin(name)) return 0;
    int k = admins_but(name);
    return k < 0 ? -ENOMEM : k == 0 ? -EMLINK : 0;
}

// /etc/skel's entries copied into `home`. The home is SETUID and the
// account's, so each file root writes there is the account's
// (fs_new_attrs(), FreeBSD's SUIDDIR) -- nothing to chown after.
static int fill_from_skel(const char *home) {
    DIR *d = opendir(SKEL);
    if (!d) return 0;   // no skeleton: an empty home, as useradd -M leaves
    struct dirent *e;
    int rc = 0;
    char src[sizeof SKEL + 1 + NAME_MAX];   // SKEL, "/" and a d_name
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if ((size_t)snprintf(src, sizeof src, "%s/%s", SKEL, e->d_name) >= sizeof src) continue;
        if (ufileop_copy(src, home, &g_op, 0) != UFILEOP_OK) rc = -EIO;
    }
    closedir(d);
    return rc;
}

int acct_add(const char *name, const char *full, int admin, const char *password) {
    if (!uusers_name_ok(name) || !uusers_full_ok(full)) return -EINVAL;
    if (getpwnam(name) || getgrnam(name)) return -EEXIST;
    char home[96];
    snprintf(home, sizeof home, HOMES "%s", name);
    // A HOME KEPT FROM AN ACCOUNT OF THIS NAME takes its owner back, so
    // removing an account and adding it again returns its files; its
    // uid must belong to nobody now.
    struct stat st;
    int kept = stat(home, &st) == 0;
    if (kept && !S_ISDIR(st.st_mode)) return -EEXIST;
    uid_t id = next_id();
    if (kept && st.st_uid >= ID_MIN && st.st_uid <= ID_MAX && !uid_taken(st.st_uid) && !gid_taken(st.st_uid))
        id = st.st_uid;
    else if (kept) return -EEXIST;   // someone else's folder: not handed over
    if (id > ID_MAX) return -ENOSPC;

    // The home first: an account is not made without one, and a home
    // that could not be made leaves nothing behind.
    if (!kept) {
        if (mkdir(home, 0700) < 0) return -errno;
        if (chown(home, id, id) < 0 || chmod(home, 04700) < 0) {
            int e = errno;
            rmdir(home);
            return -e;
        }
    }
    char line[512];
    int rc = pass_set(name, password);   // shadow before passwd: the order's invariant
    if (!rc) {
        snprintf(line, sizeof line, "%s:x:%u:\n", name, (unsigned)id);
        rc = rewrite_group(ADMIN_GROUP, admin ? name : NULL, 1, NULL, line);
        if (rc) pass_drop(name);
    }
    if (!rc) {
        snprintf(line, sizeof line, "%s:x:%u:%u:%s:%s:%s\n", name, (unsigned)id, (unsigned)id, full, home, SHELL);
        if ((rc = rewrite_passwd(NULL, line))) {
            rewrite_group("", name, 0, name, NULL);
            pass_drop(name);
        }
    }
    if (rc) {
        if (!kept) rmdir(home);
        return rc;
    }
    // The skeleton is a convenience: a file of it that would not copy
    // leaves the account usable, and is said in the log.
    if (!kept && fill_from_skel(home)) printf("privd: %s's home is missing some of /etc/skel\n", name);
    return 0;
}

int acct_del(const char *name, int delete_home) {
    struct passwd *pw = getpwnam(name);
    if (!pw) return -ENOENT;
    if (pw->pw_uid < ID_MIN) return -EPERM;
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    char home[256];
    snprintf(home, sizeof home, "%s", pw->pw_dir ? pw->pw_dir : "");
    if (has_processes(uid)) return -EBUSY;
    int rc = not_last_admin(name);
    if (rc) return rc;
    // Its own group goes only when it IS its own: the same name and id.
    struct group *g = getgrgid(gid);
    int own_group = g && !strcmp(g->gr_name, name);
    // passwd, group, then shadow: the order's invariant (top of file).
    if ((rc = rewrite_passwd(name, NULL))) return rc;
    if ((rc = rewrite_group("", name, 0, own_group ? name : NULL, NULL))) return rc;
    if ((rc = pass_drop(name))) return rc;
    char who[256] = "";
    if (usetting_get(SET_AUTOLOGIN, who, sizeof who) && !strcmp(who, name)) usetting_set(SET_AUTOLOGIN, "");
    if (delete_home && home_ok(home) && stat(home, &(struct stat){ 0 }) == 0 &&
        ufileop_remove(home, 1, &g_op, 0) != UFILEOP_OK)
        return -EIO;
    return 0;
}

int acct_mod(const char *name, const char *field, const char *value) {
    struct passwd *pw = getpwnam(name);
    if (!pw || pw->pw_uid < ID_MIN) return -ENOENT;
    if (!strcmp(field, "admin")) {
        int want = !strcmp(value, "1");
        if (!want && strcmp(value, "0")) return -EINVAL;
        int rc = want ? 0 : not_last_admin(name);
        if (rc) return rc;
        if (want == uusers_is_admin(name)) return 0;
        return rewrite_group(ADMIN_GROUP, name, want, NULL, NULL);
    }
    if (!strcmp(field, "real")) {
        if (!uusers_full_ok(value)) return -EINVAL;
        char line[768];
        if ((size_t)snprintf(line, sizeof line, "%s:%s:%u:%u:%s:%s:%s\n", pw->pw_name, pw->pw_passwd,
                             (unsigned)pw->pw_uid, (unsigned)pw->pw_gid, value, pw->pw_dir, pw->pw_shell) >=
            sizeof line)
            return -E2BIG;
        char nm[64];
        snprintf(nm, sizeof nm, "%s", name);
        return rewrite_passwd(nm, line);
    }
    if (!strcmp(field, "password")) {
        int rc = pass_set(name, value);
        // A password nobody is asked for protects nothing: automatic
        // sign-in goes off for the account, as passwd's does (privd.c).
        char who[256] = "";
        if (!rc && value[0] && usetting_get(SET_AUTOLOGIN, who, sizeof who) && !strcmp(who, name))
            usetting_set(SET_AUTOLOGIN, "");
        return rc;
    }
    return -EINVAL;
}
