// The Recycle Bin -- see utrash.h.
#include "base/utrash.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <caltime.h>
#include "rt/sys.h"
#include "kpath.h"
#include "query_abi.h"
#include "base/usetting.h"
#include "base/ufileop.h"
#include "base/uwalk.h"
#include "base/udirs.h"
#include "base/ufile.h"
#include "base/ugrow.h"
#include "base/upath.h"

#define HOME_BIN utrash_home_bin()
#define TOP_BIN  ".Trash-0"

const char *utrash_home_bin(void) {
    static char *bin;
    if (!bin) {
        const char *h = udir_home();
        if (asprintf(&bin, "%s/.Trash", strcmp(h, "/") ? h : "") < 0) bin = NULL;
    }
    return bin ? bin : "/.Trash";
}

// The mount `path` is on: the longest mount point that is a prefix of
// it, at a component boundary. 1 with its flags, or 0.
static int volume_of_sized(const char *path, char *point, int cap, uint64_t *flags, uint64_t *total) {
    struct query_fsinfo fs;
    int best = -1;
    for (int i = 0; ; i++) {
        int n = sys_query_record(QUERY_FSINFO, (unsigned)i, &fs, sizeof fs);
        if (n <= 0) break;
        if ((unsigned)n < sizeof fs || !(fs.flags & QUERY_FS_MOUNTED)) continue;
        int len = (int)strlen(fs.point);
        int under = !strcmp(fs.point, "/") ||
                    (!strncmp(path, fs.point, (size_t)len) && (path[len] == '/' || !path[len]));
        if (!under || len <= best) continue;
        best = len;
        strlcpy(point, fs.point, (size_t)cap);
        *flags = fs.flags;
        if (total) *total = fs.total_bytes;
    }
    return best >= 0;
}

static int volume_of(const char *path, char *point, int cap, uint64_t *flags) {
    return volume_of_sized(path, point, cap, flags, 0);
}

char *utrash_bin_for(const char *path) {
    char point[64];
    uint64_t flags = 0;
    if (!path || path[0] != '/' || !volume_of(path, point, sizeof point, &flags)) return 0;
    if (!(flags & QUERY_FS_PERSISTENT) || (flags & QUERY_FS_RDONLY)) return 0;
    char *out = !strcmp(point, "/") ? strdup(HOME_BIN) : upath_join(point, TOP_BIN);
    if (!out) return 0;
    // A bin, or something inside one, is not binned again.
    size_t bl = strlen(out);
    if (!strncmp(path, out, bl) && (path[bl] == '/' || !path[bl])) { free(out); return 0; }
    return out;
}

// --- an item's strings ----------------------------------------------------

// name, bin and orig in one block, `name` at its start.
static int item_set(struct utrash_item *it, const char *name, const char *bin, const char *orig) {
    size_t a = strlen(name) + 1, b = strlen(bin) + 1, c = strlen(orig) + 1;
    char *m = malloc(a + b + c);
    if (!m) return 0;
    memcpy(m, name, a);
    memcpy(m + a, bin, b);
    memcpy(m + a + b, orig, c);
    free(it->name);
    it->name = m;
    it->bin = m + a;
    it->orig = m + a + b;
    return 1;
}

void utrash_item_free(struct utrash_item *it) {
    if (!it) return;
    free(it->name);
    it->name = it->bin = it->orig = 0;
}

// --- the info file ------------------------------------------------------

static int unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
}

// The info file, written in pieces: the encoded path can be three
// times the path, and a ring-3 frame is 2 KiB.
static int write_info(int fd, const char *path, const char *date) {
    static const char hex[] = "0123456789ABCDEF";
    char chunk[96];
    int n = snprintf(chunk, sizeof chunk, "[Trash Info]\nPath=");
    for (const unsigned char *p = (const unsigned char *)path; ; p++) {
        if (!*p || n > (int)sizeof chunk - 4) {
            if (write(fd, chunk, (size_t)n) != n) return 0;
            n = 0;
            if (!*p) break;
        }
        if (unreserved(*p)) {
            chunk[n++] = (char)*p;
        } else {
            chunk[n++] = '%';
            chunk[n++] = hex[*p >> 4];
            chunk[n++] = hex[*p & 15];
        }
    }
    n = snprintf(chunk, sizeof chunk, "\nDeletionDate=%s\n", date);
    return write(fd, chunk, (size_t)n) == n;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// `out` holds strlen(in) + 1: decoding never lengthens.
static int decode(const char *in, char *out, int cap) {
    int n = 0;
    for (const char *p = in; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int hi = hexval(p[1]), lo = hi < 0 ? -1 : hexval(p[2]);
            if (lo < 0) return 0;
            c = hi * 16 + lo;
            p += 2;
        }
        if (!c || n + 1 >= cap) return 0;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n > 0 && out[0] == '/';
}

// DeletionDate is LOCAL time per the spec; every timestamp in SaunaOS is
// UTC, so it is converted both ways here and nowhere else.
static void date_local(const struct rtc_time *utc, char *out, int cap) {
    time_t e = (time_t)cal_rtc_to_epoch(utc);
    struct tm tm;
    localtime_r(&e, &tm);
    strftime(out, (size_t)cap, "%Y-%m-%dT%H:%M:%S", &tm);
}

static int date_parse(const char *s, struct rtc_time *utc) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    time_t e = mktime(&tm);
    if (e == (time_t)-1) return 0;
    cal_epoch_to_rtc((uint64_t)e, utc);
    return 1;
}

static char *info_path(const char *bin, const char *name) {
    char *p;
    return asprintf(&p, "%s/info/%s.trashinfo", bin, name) < 0 ? 0 : p;
}

static char *files_path(const char *bin, const char *name) {
    char *p;
    return asprintf(&p, "%s/files/%s", bin, name) < 0 ? 0 : p;
}

char *utrash_item_path(const struct utrash_item *it) {
    return files_path(it->bin, it->name);
}

// The bound the format sets: a Path= of FS_PATH_MAX (4096) bytes, every
// one percent-encoded, and the rest of the file.
#define INFO_MAX (4096 * 3 + 256)

// Fills `it`, whose strings are NULL; 0 leaves them NULL.
static int read_info(const char *bin, const char *name, struct utrash_item *it) {
    char *p = info_path(bin, name);
    uint8_t *raw = 0;
    size_t n = 0;
    enum ufile_result r = p ? ufile_slurp(p, INFO_MAX, &raw, &n) : UFILE_NOMEM;
    free(p);
    if (r != UFILE_OK) return 0;
    char *buf = realloc(raw, n + 1);
    if (!buf) { free(raw); return 0; }
    buf[n] = '\0';
    char *orig = 0;
    int have_path = 0, have_date = 0, header = 0;
    for (char *line = buf, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r') line[l - 1] = '\0';
        if (!strcmp(line, "[Trash Info]")) header = 1;
        else if (!strncmp(line, "Path=", 5)) {
            free(orig);
            int cap = (int)strlen(line + 5) + 1;
            orig = malloc((size_t)cap);
            have_path = orig && decode(line + 5, orig, cap);
        } else if (!strncmp(line, "DeletionDate=", 13))
            have_date = date_parse(line + 13, &it->deleted);
    }
    free(buf);
    if (!header || !have_path || !item_set(it, name, bin, orig)) { free(orig); return 0; }
    free(orig);
    if (!have_date) memset(&it->deleted, 0, sizeof it->deleted);
    char *fp = utrash_item_path(it);
    struct sys_stat st;
    int ok = fp && sys_stat(fp, &st) == 0;
    free(fp);
    if (!ok) { utrash_item_free(it); return 0; }
    it->is_dir = (int)st.is_dir;
    it->size = st.is_dir ? 0 : st.size;
    return 1;
}

// --- put ------------------------------------------------------------------

static int ensure_dir(const char *p) {
    struct sys_stat st;
    if (sys_stat(p, &st) == 0) return st.is_dir ? 0 : -ENOTDIR;
    return sys_mkdir(p) == 0 ? 0 : -sys_errno();
}

// A name in the bin leaves room for ".trashinfo" in one FS_NAME_MAX
// (255) component. Path= keeps the whole original, so only the bin's
// own name is cut.
#define BIN_NAME_MAX (255 - 10)

// `len` bytes of `s` or fewer, ending on a UTF-8 character's boundary.
static int utf8_cut(const char *s, int len) {
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
    return len;
}

// Claim a unique name: "dusk.jpg", then "dusk.2.jpg", "dusk.3.jpg" --
// the extension kept, so the bin's own listing still knows the type. A
// long stem is cut to fit, the suffix and extension kept whole. The name
// (allocated) in `*name_out`.
static int claim(const char *bin, const char *base, char **name_out, int *fd_out) {
    const char *dot = strrchr(base, '.');
    int stem = (dot && dot != base) ? (int)(dot - base) : (int)strlen(base);
    const char *ext = base + stem;
    if ((int)strlen(ext) > 32) { stem = (int)strlen(base); ext = ""; }   // no real extension
    for (int n = 1; n < 1000; n++) {
        char sfx[16] = "";
        if (n > 1) snprintf(sfx, sizeof sfx, ".%d", n);
        int room = BIN_NAME_MAX - (int)strlen(sfx) - (int)strlen(ext);
        int keep = utf8_cut(base, stem < room ? stem : room);
        char *name, *ip = 0, *fp = 0;
        if (asprintf(&name, "%.*s%s%s", keep, base, sfx, ext) < 0) return -ENOMEM;
        ip = info_path(bin, name);
        fp = files_path(bin, name);
        int rc = 1;
        struct sys_stat st;
        if (!ip || !fp) {
            rc = -ENOMEM;
        } else if (sys_stat(fp, &st) != 0) {   // else an orphan holds it
            int fd = open(ip, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd >= 0) { *fd_out = fd; *name_out = name; name = 0; rc = 0; }
            else if (errno != EEXIST) rc = -errno;
        }
        free(name);
        free(ip);
        free(fp);
        if (rc <= 0) return rc;
    }
    return -EEXIST;
}

static int ensure_sub(const char *bin, const char *sub) {
    char *p = upath_join(bin, sub);
    int rc = p ? ensure_dir(p) : -ENOMEM;
    free(p);
    return rc;
}

static int put(const char *path, const char *bin, struct utrash_item *it) {
    struct sys_stat st;
    if (sys_stat(path, &st) != 0) return -sys_errno();
    int rc = ensure_dir(bin);
    if (!rc) rc = ensure_sub(bin, "files");
    if (!rc) rc = ensure_sub(bin, "info");
    if (rc) return rc;

    char date[32];
    struct rtc_time now;
    sys_gettime(&now);
    date_local(&now, date, sizeof date);
    int fd = -1;
    char *name = 0;
    rc = claim(bin, k_path_basename(path), &name, &fd);
    if (rc) return rc;
    int wrote = write_info(fd, path, date);
    close(fd);

    char *ip = info_path(bin, name), *fp = files_path(bin, name);
    if (!wrote || !ip || !fp || !item_set(it, name, bin, path)) rc = !wrote ? -EIO : -ENOMEM;
    else if (sys_rename(path, fp) != 0) rc = -sys_errno();
    if (rc) {
        if (ip) unlink(ip);
        utrash_item_free(it);
    } else {
        it->deleted = now;
        it->is_dir = (int)st.is_dir;
        it->size = st.is_dir ? 0 : st.size;
    }
    free(name);
    free(ip);
    free(fp);
    return rc;
}

int utrash_put(const char *path, struct utrash_item *out) {
    struct utrash_item local;
    struct utrash_item *it = out ? out : &local;
    memset(it, 0, sizeof *it);
    char *bin = utrash_bin_for(path);
    if (!bin) return -ENOTSUP;
    int rc = put(path, bin, it);
    free(bin);
    if (!out) utrash_item_free(&local);
    return rc;
}

// --- list -----------------------------------------------------------------

// What a listing gathers into: the items so far, or the first one only.
struct gather {
    struct utrash_item *items;
    int n, cap;
    int first_only;   // utrash_any(): stop at one
    int nomem;
};

static int gathered(const struct gather *g) { return g->nomem || (g->first_only && g->n); }

static void list_bin(const char *bin, struct gather *g) {
    char *dir = upath_join(bin, "info");
    if (!dir) { g->nomem = 1; return; }
    char *page = 0;
    uint32_t start = 0, pcap = 0;
    int got;
    while (!gathered(g) && (got = ulistdir_page(dir, &start, &page, &pcap)) > 0) {
        struct ulist_entry e;
        for (int pos = 0; !gathered(g) && ulistdir_next(page, got, &pos, &e); ) {
            char name[ULIST_NAME_MAX];
            size_t l = strlen(e.name);
            static const char ext[] = ".trashinfo";
            if (e.is_dir || l <= sizeof ext - 1 ||
                strcmp(e.name + l - (sizeof ext - 1), ext)) continue;
            strlcpy(name, e.name, sizeof name);
            name[l - (sizeof ext - 1)] = '\0';
            if (!UGROW(g->items, g->cap, g->n + 1)) { g->nomem = 1; break; }
            memset(&g->items[g->n], 0, sizeof g->items[g->n]);
            if (read_info(bin, name, &g->items[g->n])) g->n++;
        }
    }
    free(page);
    free(dir);
}

static int newest_first(const void *a, const void *b) {
    uint64_t ta = cal_rtc_to_epoch(&((const struct utrash_item *)a)->deleted);
    uint64_t tb = cal_rtc_to_epoch(&((const struct utrash_item *)b)->deleted);
    return ta < tb ? 1 : ta > tb ? -1 : 0;
}

static void gather_all(struct gather *g) {
    list_bin(HOME_BIN, g);
    struct query_fsinfo fs;
    for (int i = 0; !gathered(g); i++) {
        int r = sys_query_record(QUERY_FSINFO, (unsigned)i, &fs, sizeof fs);
        if (r <= 0) break;
        if ((unsigned)r < sizeof fs || !(fs.flags & QUERY_FS_MOUNTED) ||
            !(fs.flags & QUERY_FS_PERSISTENT) || !strcmp(fs.point, "/")) continue;
        char *bin = upath_join(fs.point, TOP_BIN);
        if (!bin) { g->nomem = 1; break; }
        list_bin(bin, g);
        free(bin);
    }
}

void utrash_list_free(struct utrash_item *items, int n) {
    for (int i = 0; i < n; i++) utrash_item_free(&items[i]);
    free(items);
}

int utrash_list(struct utrash_item **out) {
    struct gather g = { 0 };
    gather_all(&g);
    if (g.nomem) { utrash_list_free(g.items, g.n); *out = 0; return -ENOMEM; }
    qsort(g.items, (size_t)g.n, sizeof *g.items, newest_first);
    *out = g.items;
    return g.n;
}

int utrash_any(void) {
    struct gather g = { .first_only = 1 };
    gather_all(&g);
    int any = g.n > 0;
    utrash_list_free(g.items, g.n);
    return any;
}

// --- restore and forget ---------------------------------------------------

// 1 when the folder `path` would be created in exists.
static int parent_is_dir(const char *path) {
    char *parent = upath_dirname(path);
    struct sys_stat st;
    int ok = parent && sys_stat(parent, &st) == 0 && st.is_dir;
    free(parent);
    return ok;
}

static int forget(const char *bin, const char *name) {
    char *ip = info_path(bin, name);
    if (!ip) return -ENOMEM;
    int rc = sys_unlink(ip) == 0 ? 0 : -sys_errno();
    free(ip);
    return rc;
}

int utrash_restore(const struct utrash_item *it) {
    struct sys_stat st;
    if (sys_stat(it->orig, &st) == 0) return -EEXIST;
    if (!parent_is_dir(it->orig)) return -ENOENT;
    char *fp = utrash_item_path(it);
    if (!fp) return -ENOMEM;
    int rc = sys_rename(fp, it->orig) == 0 ? 0 : -sys_errno();
    free(fp);
    return rc ? rc : utrash_forget(it);
}

int utrash_forget_path(const char *files_path) {
    char *files = upath_dirname(files_path);
    char *bin = files && !strcmp(k_path_basename(files), "files") ? upath_dirname(files) : 0;
    int rc = bin ? forget(bin, k_path_basename(files_path)) : files ? -EINVAL : -ENOMEM;
    free(files);
    free(bin);
    return rc;
}

int utrash_restore_to(const char *files_path, const char *dest) {
    struct sys_stat st;
    if (sys_stat(dest, &st) == 0) return -EEXIST;
    if (!parent_is_dir(dest)) return -ENOENT;
    if (sys_rename(files_path, dest) != 0) return -sys_errno();
    return utrash_forget_path(files_path);
}

int utrash_forget(const struct utrash_item *it) {
    return forget(it->bin, it->name);
}

// --- limits ---------------------------------------------------------------

static int setting_number(const char *name) {
    char v[16];
    if (!usetting_get(name, v, sizeof v)) return 0;
    return atoi(v);   // "never" and "none" read as 0
}

void utrash_limits_get(struct utrash_limits *l) {
    l->max_age_days = setting_number(UTRASH_SET_AGE);
    l->max_size_pct = setting_number(UTRASH_SET_SIZE);
}

static int add_size(void *ctx, const char *path, const struct ulist_entry *e) {
    (void)path;
    if (!e->is_dir) *(uint64_t *)ctx += e->size;
    return 1;
}

// An item's bytes: a file's size, a folder's files summed.
static uint64_t item_bytes(const struct utrash_item *it, struct uwalk *w) {
    if (!it->is_dir) return it->size;
    char *p = utrash_item_path(it);
    uint64_t sum = 0;
    if (p && uwalk_begin(w, p))
        while (uwalk_step(w, add_size, &sum)) {}
    uwalk_end(w);
    free(p);
    return sum;
}

// The bins and their items, oldest first, with each item's bytes -- ALL
// of them: an expiry that saw only some would purge the wrong ones.
struct census {
    struct utrash_item *items;
    uint64_t *bytes;
    uint8_t *dead;   // utrash_expire(): deleted for good
    int n;
    struct uwalk walk;
};

static void census_free(struct census *c) {
    if (!c) return;
    utrash_list_free(c->items, c->n);
    free(c->bytes);
    free(c->dead);
    free(c);
}

static struct census *census_take(void) {
    struct census *c = calloc(1, sizeof *c);
    if (!c) return 0;
    int n = utrash_list(&c->items);   // newest first
    if (n < 0) { free(c); return 0; }
    c->n = n;
    c->bytes = calloc((size_t)n + 1, sizeof *c->bytes);
    c->dead = calloc((size_t)n + 1, 1);
    if (!c->bytes || !c->dead) { census_free(c); return 0; }
    struct utrash_item *it = c->items;
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        struct utrash_item t = it[i]; it[i] = it[j]; it[j] = t;
    }
    for (int i = 0; i < n; i++) c->bytes[i] = item_bytes(&it[i], &c->walk);
    return c;
}

void utrash_usage_free(struct utrash_usage *u, int n) {
    for (int i = 0; i < n; i++) free(u[i].bin);
    free(u);
}

int utrash_usage(struct utrash_usage **out) {
    *out = 0;
    struct census *c = census_take();
    if (!c) return -ENOMEM;
    struct utrash_usage *u = 0;
    int nb = 0, cap = 0, rc = 0;
    for (int i = 0; i < c->n && !rc; i++) {
        int b = 0;
        while (b < nb && strcmp(u[b].bin, c->items[i].bin)) b++;
        if (b == nb) {
            if (!UGROW(u, cap, nb + 1)) { rc = -ENOMEM; break; }
            memset(&u[b], 0, sizeof u[b]);
            if (!(u[b].bin = strdup(c->items[i].bin))) { rc = -ENOMEM; break; }
            uint64_t flags = 0;
            volume_of_sized(u[b].bin, u[b].point, sizeof u[b].point, &flags, &u[b].volume_bytes);
            nb++;
        }
        u[b].items++;
        u[b].bytes += c->bytes[i];
    }
    census_free(c);
    if (rc) { utrash_usage_free(u, nb); return rc; }
    *out = u;
    return nb;
}

static int purge(const struct utrash_item *it, struct ufileop *op) {
    static const struct ufileop_policy pol = { 0 };
    char *p = utrash_item_path(it);
    int ok = p && ufileop_remove(p, 1, op, &pol) == UFILEOP_OK;
    free(p);
    return ok && utrash_forget(it) == 0;
}
int utrash_expire(const struct utrash_limits *l, struct ufileop *op) {
    if (!l->max_age_days && !l->max_size_pct) return 0;
    struct census *c = census_take();
    if (!c) return -ENOMEM;
    int gone = 0;
    uint8_t *dead = c->dead;
    if (l->max_age_days) {
        struct rtc_time now;
        sys_gettime(&now);
        uint64_t cutoff = cal_rtc_to_epoch(&now) - (uint64_t)l->max_age_days * 86400;
        for (int i = 0; i < c->n; i++) {
            uint64_t when = cal_rtc_to_epoch(&c->items[i].deleted);
            if (when && when < cutoff && purge(&c->items[i], op)) { dead[i] = 1; gone++; }
        }
    }
    if (l->max_size_pct) {
        for (int i = 0; i < c->n; i++) {   // each bin once, from its oldest item
            if (dead[i]) continue;
            int first = 1;
            for (int k = 0; k < i; k++) if (!strcmp(c->items[k].bin, c->items[i].bin)) first = 0;
            if (!first) continue;
            char point[64];
            uint64_t flags = 0, total = 0, held = 0;
            if (!volume_of_sized(c->items[i].bin, point, sizeof point, &flags, &total)) continue;
            uint64_t limit = total / 100 * (uint64_t)l->max_size_pct;
            int newest = -1;
            for (int k = 0; k < c->n; k++)
                if (!dead[k] && !strcmp(c->items[k].bin, c->items[i].bin)) { held += c->bytes[k]; newest = k; }
            for (int k = i; k < c->n && held > limit; k++) {
                if (dead[k] || k == newest || strcmp(c->items[k].bin, c->items[i].bin)) continue;
                if (purge(&c->items[k], op)) { dead[k] = 1; held -= c->bytes[k]; gone++; }
            }
        }
    }
    census_free(c);
    return gone;
}
