// <net/unetmount.h> -- ~/.config/mounts.d, one file per saved network share.
#include "net/unetmount.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>

#include "rt/sys.h"
#include "base/uconf.h"
#include "etc_config.h"
#include "query_abi.h"
#include "userfs_abi.h"
#include "base/ugrow.h"
#include "base/ulistdir.h"

int unetmount_name_ok(const char *name) {
    size_t n = strlen(name);
    if (!n || n >= UNETMOUNT_NAME_MAX || name[0] == '.') return 0;
    if (n > 3 && !strcmp(name + n - 3, ".md")) return 0;   // a README beside them is not a share
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

void unetmount_point(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", UNETMOUNT_ROOT, name);
}

static char *g_root_home;   // unetmount_use_home()'s, for root

void unetmount_use_home(const char *home) {
    free(g_root_home);
    g_root_home = home ? strdup(home) : NULL;
}

// The folder, allocated to fit (free() it); NULL when there is none.
static char *dir_alloc(void) {
    char *out = NULL;
    if (geteuid() == 0) {
        if (g_root_home && asprintf(&out, "%s/.config/%s", g_root_home, UNETMOUNT_SUBDIR) < 0) out = NULL;
        return out;
    }
    // XDG_CONFIG_HOME (base/uconf.h's person's layer), else the account's
    // own home -- a shell started by `runuser` may carry no XDG variable.
    char *xdg = malloc(UCONF_USER_PATH_MAX);
    if (xdg && uconf_user_path("/etc/" UNETMOUNT_SUBDIR, xdg, UCONF_USER_PATH_MAX) == 1) return xdg;
    free(xdg);
    struct passwd *pw = getpwuid(geteuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] == '/' &&
        asprintf(&out, "%s/.config/%s", pw->pw_dir, UNETMOUNT_SUBDIR) < 0)
        out = NULL;
    return out;
}

int unetmount_dir(char *out, size_t cap) {
    char *d = dir_alloc();
    int ok = d && strlen(d) < cap;
    if (d && !ok) fprintf(stderr, "netmount: %s does not fit %zu bytes\n", d, cap);
    if (ok) memcpy(out, d, strlen(d) + 1);
    free(d);
    return ok;
}

// The share's file, allocated (free() it); NULL when there is no folder.
static char *path_alloc(const char *name) {
    char *d = dir_alloc(), *out = NULL;
    if (d && asprintf(&out, "%s/%s", d, name) < 0) out = NULL;
    free(d);
    return out;
}

void unetmount_suggest(const char *source, char *out, size_t cap) {
    // //server/share[/...] -- the share's name, with anything a name may
    // not hold dropped.
    const char *p = source;
    while (*p == '/' || *p == '\\') p++;
    if (!strncmp(source, "smb://", 6)) p = source + 6;
    p += strcspn(p, "/\\");
    while (*p == '/' || *p == '\\') p++;
    char base[UNETMOUNT_NAME_MAX];
    size_t n = 0;
    for (; *p && *p != '/' && *p != '\\' && n + 1 < sizeof base - 4; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || (c == '.' && n))
            base[n++] = c;
        else if (c == ' ' && n && base[n - 1] != '-')
            base[n++] = '-';
    }
    while (n && base[n - 1] == '-') n--;
    base[n] = '\0';
    if (!n) snprintf(base, sizeof base, "share");
    snprintf(out, cap, "%s", base);
    struct stat st;
    for (int i = 2; i < 100; i++) {
        char *path = path_alloc(out);
        int taken = path && stat(path, &st) == 0;
        free(path);
        if (!taken) return;
        snprintf(out, cap, "%.24s-%d", base, i);
    }
}

int unetmount_load(const char *name, struct unetmount *out) {
    memset(out, 0, sizeof *out);
    char *path = unetmount_name_ok(name) ? path_alloc(name) : NULL;
    if (!path) return 0;
    struct etc_config_buf doc, *buf = &doc;
    int ok = uconf_load(path, buf);
    free(path);
    if (ok) {
        snprintf(out->name, sizeof out->name, "%s", name);
        ok = etc_config_buf_get(buf, "Source", out->source, sizeof out->source) && out->source[0];
        etc_config_buf_get(buf, "User", out->user, sizeof out->user);
        etc_config_buf_get(buf, "Domain", out->domain, sizeof out->domain);
        etc_config_buf_get(buf, "Hash", out->hash, sizeof out->hash);
        if (strlen(out->hash) != 32) out->hash[0] = '\0';
    }
    etc_config_buf_free(buf);
    return ok;
}

static int save_in(const struct unetmount *m, char *dir, const char *path, const char *tmp);

int unetmount_save(const struct unetmount *m) {
    if (!unetmount_name_ok(m->name) || !m->source[0]) return 0;
    for (const char *f[] = { m->source, m->user, m->domain, m->hash }, **p = f; p < f + 4; p++)
        if (strchr(*p, '\n')) return 0;     // one line per key, or the file says something else
    char *dir = dir_alloc(), *path = NULL, *tmp = NULL;
    int ok = dir && asprintf(&path, "%s/%s", dir, m->name) >= 0 &&
             asprintf(&tmp, "%s/.%s.new", dir, m->name) >= 0 && save_in(m, dir, path, tmp);
    free(dir);
    free(path);
    free(tmp);
    return ok;
}

// unetmount_save()'s work, with its three paths made.
static int save_in(const struct unetmount *m, char *dir, const char *path, const char *tmp) {
    // The person's alone: a mode argument never reaches the kernel (the
    // umask's applies), so each is set after it is made -- the folder
    // before anything is in it, the file before the hash is written.
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); *slash = '/'; }   // ~/.config
    struct stat st;
    if (stat(dir, &st) != 0 && mkdir(dir, 0700) != 0) return 0;
    if (stat(dir, &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0777) != 0700)
        chmod(dir, 0700);   // made by `mkdir` at the umask's 0755, perhaps
    char doc[640];
    int n = snprintf(doc, sizeof doc,
                     "# A network share, mounted at %s/%s by smbfsd. See `doc smbfsd`.\n"
                     "Source=%s\n", UNETMOUNT_ROOT, m->name, m->source);
    if (m->user[0])   n += snprintf(doc + n, sizeof doc - (size_t)n, "User=%s\n", m->user);
    if (m->domain[0]) n += snprintf(doc + n, sizeof doc - (size_t)n, "Domain=%s\n", m->domain);
    if (m->hash[0])   n += snprintf(doc + n, sizeof doc - (size_t)n, "Hash=%s\n", m->hash);
    if (n >= (int)sizeof doc) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    int ok = fchmod(fd, 0600) == 0 && write(fd, doc, (size_t)n) == n;
    close(fd);
    // Published by a rename: smbfsd may be reading the old one.
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

int unetmount_remove(const char *name) {
    char *path = unetmount_name_ok(name) ? path_alloc(name) : NULL;
    int ok = path && unlink(path) == 0;
    free(path);
    return ok;
}

static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }

int unetmount_list(char (**names)[UNETMOUNT_NAME_MAX], int *cap) {
    struct ulist_entry *ents = NULL;
    char *dir = dir_alloc();
    int n = dir ? ulistdir(dir, &ents) : 0, out = 0;   // the whole folder, paged
    free(dir);
    if (n > 0 && !UGROW(*names, *cap, n)) {
        fprintf(stderr, "netmount: out of memory listing %d mounts\n", n);
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir || !unetmount_name_ok(ents[i].name)) continue;
        snprintf((*names)[out++], UNETMOUNT_NAME_MAX, "%s", ents[i].name);
    }
    free(ents);
    if (out) qsort(*names, (size_t)out, UNETMOUNT_NAME_MAX, cmp_name);
    return out;
}

// ---- addresses --------------------------------------------------------

int unetmount_is_address(const char *s) {
    while (*s == ' ') s++;
    return !strncmp(s, "smb://", 6) || (s[0] == '/' && s[1] == '/') || (s[0] == '\\' && s[1] == '\\');
}

int unetmount_normalise(const char *in, char *out, size_t cap) {
    const char *p = in;
    while (*p == ' ') p++;
    if (!strncmp(p, "smb://", 6)) p += 6;
    else if ((p[0] == '/' && p[1] == '/') || (p[0] == '\\' && p[1] == '\\')) p += 2;
    else return 0;
    size_t o = 0;
    if (cap < 3) return 0;
    out[o++] = '/'; out[o++] = '/';
    int seps = 0, after = 0;
    for (; *p; p++) {
        char c = *p == '\\' ? '/' : *p;
        if (c == '/') { if (out[o - 1] == '/') continue; seps++; after = 0; }
        else after++;
        if (o + 2 > cap) return 0;
        out[o++] = c;
    }
    while (o > 2 && out[o - 1] == '/') o--;
    out[o] = '\0';
    // A server, then at least one more component: the share.
    return seps >= 1 && (after > 0 || seps >= 2) && out[2] != '/';
}

// "//server/share" of a normalised address (`root`) and what follows it.
static void split(const char *addr, char *root, size_t cap, const char **rest) {
    const char *p = addr + 2;
    p += strcspn(p, "/");                   // past the server
    if (*p) p++;
    p += strcspn(p, "/");                   // past the share
    size_t n = (size_t)(p - addr);
    if (n >= cap) n = cap - 1;
    memcpy(root, addr, n);
    root[n] = '\0';
    *rest = p;
}

static int same_text_nocase(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

// The point of a network mount whose source is `root`, if one is mounted.
static int mounted_at(const char *root, char *point, size_t cap) {
    struct query_fsinfo fs;
    for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
        if (!(fs.flags & QUERY_FS_REMOTE)) continue;
        char src[160];
        if (!unetmount_normalise(fs.source, src, sizeof src)) continue;
        if (same_text_nocase(src, root)) { snprintf(point, cap, "%s", fs.point); return 1; }
    }
    return 0;
}

static int point_taken(const char *point) {
    struct query_fsinfo fs;
    for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++)
        if ((fs.flags & QUERY_FS_MOUNTED) && !strcmp(fs.point, point)) return 1;
    return 0;
}

static int open_address(const char *addr, char *out, size_t cap, int may_mount) {
    char norm[200], root[160], point[96];
    const char *rest;
    if (!unetmount_normalise(addr, norm, sizeof norm) || strchr(norm, ' ')) return -EINVAL;
    split(norm, root, sizeof root, &rest);
    if (!mounted_at(root, point, sizeof point)) {
        if (!may_mount) return -ENOENT;
        // A name of its own: the share's, free among saved shares (which
        // will want /net/<their name>) and mount points both.
        char name[UNETMOUNT_NAME_MAX];
        unetmount_suggest(root, name, sizeof name);
        unetmount_point(name, point, sizeof point);
        for (int i = 2; point_taken(point) && i < 100; i++) {
            char base[UNETMOUNT_NAME_MAX];
            unetmount_suggest(root, base, sizeof base);
            snprintf(name, sizeof name, "%.24s-%d", base, i);
            unetmount_point(name, point, sizeof point);
        }
        if (point_taken(point)) return -ENOSPC;
        char args[320];
        snprintf(args, sizeof args, "--url %s %s", root, point);
        if (sys_spawn("/bin/smbfsd", args, -1) < 0) return -EIO;
        int up = 0;
        for (int t = 0; t < 60 && !up; t++) {   // it mounts at once; connecting comes later
            up = mounted_at(root, point, sizeof point);
            if (!up) sys_sleep_ms(50);
        }
        if (!up) return -EIO;
    }
    snprintf(out, cap, "%s%s", point, rest);
    return 0;
}

int unetmount_open_address(const char *addr, char *out, size_t cap) { return open_address(addr, out, cap, 1); }
int unetmount_find_address(const char *addr, char *out, size_t cap) { return open_address(addr, out, cap, 0); }

int unetmount_is_session(const char *point) {
    struct query_fsinfo fs;
    for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
        if (!(fs.flags & QUERY_FS_REMOTE) || strcmp(fs.point, point)) continue;
        const char *base = strrchr(point, '/');
        struct unetmount m;
        if (!base || strncmp(point, UNETMOUNT_ROOT "/", strlen(UNETMOUNT_ROOT) + 1)) return 1;
        return !unetmount_load(base + 1, &m);
    }
    return 0;
}

int unetmount_save_session(const char *point, const char *name) {
    size_t n = strlen(name) + 1;
    struct userfs_ctl *c = calloc(1, sizeof *c);   // a 512-byte payload: not on a ring-3 frame
    if (!c) return -ENOMEM;
    if (n > sizeof c->data) { free(c); return -ENAMETOOLONG; }
    strlcpy(c->point, point, sizeof c->point);
    c->code = USERFS_CTL_SAVE;
    c->len = (uint32_t)n;
    memcpy(c->data, name, n);
    int64_t rc = sys_userfs(USERFS_CTL, (uint64_t)(uintptr_t)c, 0);
    free(c);
    return rc < 0 ? -sys_errno() : 0;
}
