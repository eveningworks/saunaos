// <lib/unetmount.h> -- /etc/mounts.d, one file per saved network share.
#include "lib/unetmount.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rt/sys.h"
#include "lib/uconf.h"
#include "etc_config.h"

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

static void path_of(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", UNETMOUNT_DIR, name);
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
    char path[96];
    for (int i = 2; i < 100; i++) {
        path_of(out, path, sizeof path);
        if (stat(path, &st) != 0) return;
        snprintf(out, cap, "%.24s-%d", base, i);
    }
}

int unetmount_load(const char *name, struct unetmount *out) {
    memset(out, 0, sizeof *out);
    if (!unetmount_name_ok(name)) return 0;
    char path[96];
    path_of(name, path, sizeof path);
    struct etc_config_buf *buf = malloc(sizeof *buf);
    if (!buf) return 0;
    int ok = uconf_load(path, buf);
    if (ok) {
        snprintf(out->name, sizeof out->name, "%s", name);
        ok = etc_config_buf_get(buf, "Source", out->source, sizeof out->source) && out->source[0];
        etc_config_buf_get(buf, "User", out->user, sizeof out->user);
        etc_config_buf_get(buf, "Domain", out->domain, sizeof out->domain);
        etc_config_buf_get(buf, "Hash", out->hash, sizeof out->hash);
        if (strlen(out->hash) != 32) out->hash[0] = '\0';
    }
    free(buf);
    return ok;
}

int unetmount_save(const struct unetmount *m) {
    if (!unetmount_name_ok(m->name) || !m->source[0]) return 0;
    for (const char *f[] = { m->source, m->user, m->domain, m->hash }, **p = f; p < f + 4; p++)
        if (strchr(*p, '\n')) return 0;     // one line per key, or the file says something else
    mkdir(UNETMOUNT_DIR, 0755);
    char path[96], tmp[112];
    path_of(m->name, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s/.%s.new", UNETMOUNT_DIR, m->name);
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
    int ok = write(fd, doc, (size_t)n) == n;
    close(fd);
    // Published by a rename: smbfsd may be reading the old one.
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

int unetmount_remove(const char *name) {
    if (!unetmount_name_ok(name)) return 0;
    char path[96];
    path_of(name, path, sizeof path);
    return unlink(path) == 0;
}

static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }

int unetmount_list(char (*names)[UNETMOUNT_NAME_MAX], int max) {
    struct sys_dirent *ents = malloc(sizeof *ents * 64);
    if (!ents) return 0;
    int n = sys_listdir(UNETMOUNT_DIR, ents, 64), out = 0;
    for (int i = 0; i < n && out < max; i++) {
        if (ents[i].is_dir || !unetmount_name_ok(ents[i].name)) continue;
        snprintf(names[out++], UNETMOUNT_NAME_MAX, "%s", ents[i].name);
    }
    free(ents);
    qsort(names, (size_t)out, UNETMOUNT_NAME_MAX, cmp_name);
    return out;
}
