// Game data fetched on the machine -- see udatafetch.h.
#include "base/udatafetch.h"
#include "base/uarchive.h"
#include "base/uconf.h"
#include "base/ufileop.h"
#include "base/ulog.h"
#include "base/uzip.h"
#include <fcntl.h>
#include <kcrc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LICENCE_DIR "/usr/share/licenses"
#define SCRATCH_DIR "/var/tmp"

char *udata_path(const char *dir, const struct udata_item *it) {
    char *p = NULL;
    return asprintf(&p, "%s/%s", dir, it->id) < 0 ? NULL : p;
}

int udata_present(const char *dir, const struct udata_item *it) {
    char *p = udata_path(dir, it);
    int ok = p && access(p, F_OK) == 0;
    free(p);
    return ok;
}

unsigned long udata_size(const struct udata_item *it) {
    unsigned long n = 0;
    for (int i = 0; i < it->part_count; i++) n += it->parts[i].size;
    return n;
}

char *udata_url(const struct udata_part *p, const char *mirror) {
    if (!p->url) return NULL;
    char *out = NULL;
    const char *name = strrchr(p->url, '/');
    if (mirror && *mirror && name) {
        if (asprintf(&out, "%s%s", mirror, name) < 0) return NULL;
    } else {
        out = strdup(p->url);
    }
    return out;
}

void udata_host(const struct udata_part *p, const char *mirror, char *out, int cap) {
    out[0] = 0;
    char *url = udata_url(p, mirror);
    if (!url) return;
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    snprintf(out, (size_t)cap, "%.*s", (int)strcspn(h, "/"), h);
    free(url);
}

// --- the licence ----------------------------------------------------------

static void licence_key(const struct udata_item *it, char *key, int cap, char *val, int vcap) {
    snprintf(key, (size_t)cap, "licence_%.*s", (int)strcspn(it->id, "."), it->id);
    snprintf(val, (size_t)vcap, "%08x", (unsigned)kcrc32(it->notice, strlen(it->notice)));
}

int udata_licence_agreed(const char *conf, const struct udata_item *it) {
    if (!it->notice) return 1;
    char key[64], want[16], have[16];
    licence_key(it, key, sizeof key, want, sizeof want);
    return uconf_get(conf, key, have, sizeof have) && !strcmp(have, want);
}

void udata_licence_agree(const char *conf, const struct udata_item *it) {
    if (!it->notice) return;
    char key[64], val[16];
    licence_key(it, key, sizeof key, val, sizeof val);
    uconf_set(conf, key, val);
}

static int put(int fd, const char *s) {
    size_t n = strlen(s);
    return write(fd, s, n) == (long)n;
}

// The archive's own text when it has one, else the notice that was
// shown. Best effort: the data is good either way, and the log says when
// the licence could not be written.
static void save_licence(const struct udata_item *it, const char *zip) {
    if (!it->licence_file) return;
    char *path = NULL, err[96];
    if (asprintf(&path, LICENCE_DIR "/%s", it->licence_file) < 0) return;
    mkdir("/usr", 0755);
    mkdir("/usr/share", 0755);
    mkdir(LICENCE_DIR, 0755);
    if (zip && it->licence_member) {
        if (uzip_extract(zip, it->licence_member, path, 0, 0, err, sizeof err) != 0)
            ulogf("udatafetch: could not save %s: %s\n", path, err);
        free(path);
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        ulogf("udatafetch: could not save %s\n", path);
        free(path);
        return;
    }
    // Written piece by piece: no buffer to outgrow.
    const char *lic = it->licence ? it->licence : "";
    int ok = put(fd, it->name) && put(fd, "\n") && put(fd, lic) && put(fd, "\n\n");
    for (const char *p = it->notice; ok && p && *p;) {
        size_t len = strcspn(p, "\n");
        ok = write(fd, p, len) == (long)len && put(fd, "\n\n");
        p += len + (p[len] == '\n');
    }
    close(fd);
    if (!ok) ulogf("udatafetch: could not write all of %s\n", path);
    free(path);
}

// --- the fetch --------------------------------------------------------------

static int failf(struct udata_fetch *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->err, sizeof f->err, fmt, ap);
    va_end(ap);
    return -1;
}

struct run {
    struct udata_fetch *f;
    int part;
    unsigned long base, total;   // an unpack's bytes before this member, and all of them
    unsigned long last;          // the member being written's size, added to base at the next
    const char *prefix;          // UDATA_TREE: the folder taken from the zip
    const char *into;            // ...and where it goes
};

static void dl_progress(void *ctx, unsigned long got, unsigned long total) {
    struct run *r = ctx;
    if (r->f->progress) r->f->progress(r->f->ctx, 0, r->part, got, total);
}
static int dl_cancelled(void *ctx) { struct run *r = ctx; return r->f->cancel && *r->f->cancel; }

static int unpack_progress(void *ctx, unsigned long long done, unsigned long long total) {
    struct run *r = ctx;
    (void)total;
    if (r->f->progress) r->f->progress(r->f->ctx, 1, r->part, r->base + (unsigned long)done, r->total);
    return r->f->cancel && *r->f->cancel;
}
static int member_progress(void *ctx, unsigned long done, unsigned long total) {
    return unpack_progress(ctx, done, total);
}

// Every folder above `path`, made (mkdir -p's walk, the leaf left).
static void make_parents(char *path) {
    for (char *s = path + 1; (s = strchr(s, '/')); s++) {
        *s = 0;
        mkdir(path, 0755);
        *s = '/';
    }
}

static int under(const struct run *r, const char *name) {
    size_t n = strlen(r->prefix);
    return !strncmp(name, r->prefix, n) && name[n];
}

static int sum_tree(void *ctx, const struct uarchive_entry *e) {
    struct run *r = ctx;
    if (!e->is_dir && under(r, e->name)) r->total += (unsigned long)e->size;
    return 0;
}

static int route_tree(void *ctx, const struct uarchive_entry *e, char *out, int cap) {
    struct run *r = ctx;
    if (!under(r, e->name)) return 0;
    if (snprintf(out, (size_t)cap, "%s/%s", r->into, e->name + strlen(r->prefix)) >= cap) return 0;
    make_parents(out);
    if (e->is_dir) { mkdir(out, 0755); return 0; }
    // This member's bytes are counted from where the last one ended.
    r->base += r->last;
    r->last = (unsigned long)e->size;
    return 1;
}

static void remove_tree(const char *path) {
    static struct ufileop op;    // tens of KB of walk state: never a local
    struct ufileop_policy pol = { 0 };
    if (access(path, F_OK) == 0) ufileop_remove(path, 1, &op, &pol);
}

int udata_fetch_run(struct udata_fetch *f, const struct udata_item *it) {
    f->err[0] = 0;
    if (it->part_count <= 0) return failf(f, "%s has no download", it->name);
    char *dest = udata_path(f->dir, it), *part = NULL;
    if (!dest || asprintf(&part, "%s.part", dest) < 0) {
        free(dest);
        return failf(f, "out of memory");
    }
    // Each part's download, kept until the end for the licence.
    char **scratch = calloc((size_t)it->part_count, sizeof *scratch);
    int tree = it->parts[0].how == UDATA_TREE, rc = -1;
    if (!scratch) {
        failf(f, "out of memory");
        goto out;
    }
    make_parents(dest);
    mkdir("/var", 0755);
    mkdir(SCRATCH_DIR, 0755);
    remove_tree(part);
    if (tree && mkdir(part, 0755) != 0) {
        failf(f, "could not make %s", part);
        goto out;
    }
    for (int i = 0; i < it->part_count; i++) {
        const struct udata_part *p = &it->parts[i];
        struct run r = { f, i, 0, 0, 0, p->member ? p->member : "", part };
        char *url = udata_url(p, f->mirror);
        const char *base = strrchr(p->url, '/');
        if (!url || asprintf(&scratch[i], SCRATCH_DIR "%s", base ? base : "/download") < 0) {
            free(url);
            failf(f, "out of memory");
            goto out;
        }
        struct uhttp_download d = {
            .url = url,
            .path = p->how == UDATA_FILE ? part : scratch[i],
            .sha256 = p->sha256,
            .insecure = 1,             // the checksum is the authority (the header)
            .allow_weak_entropy = 1,
            .max_redirects = 5,
            .progress = dl_progress,
            .cancelled = dl_cancelled,
            .ctx = &r,
        };
        ulogf("udatafetch: downloading %s\n", url);
        int got = f->download(&d);
        free(url);
        if (got != 0) {
            failf(f, "%s", d.err);
            goto out;
        }
        if (p->how == UDATA_MEMBER) {
            if (f->progress) f->progress(f->ctx, 1, i, 0, 0);
            if (uzip_extract(scratch[i], p->member, part, member_progress, &r, f->err,
                             (int)sizeof f->err) != 0)
                goto out;
        } else if (p->how == UDATA_TREE) {
            char err[96];
            if (uarchive_list(scratch[i], sum_tree, &r, err, sizeof err) != 0) {
                failf(f, "%s", err);
                goto out;
            }
            if (!r.total) {
                failf(f, "%s holds no %s", base ? base + 1 : "the download", r.prefix);
                goto out;
            }
            if (f->progress) f->progress(f->ctx, 1, i, 0, r.total);
            if (uarchive_extract_many(scratch[i], route_tree, &r, unpack_progress, &r, f->err,
                                      (int)sizeof f->err) != 0)
                goto out;
        }
        if (f->cancel && *f->cancel) {
            failf(f, "Cancelled.");
            goto out;
        }
    }
    if (rename(part, dest) != 0) {
        failf(f, "could not move it into %s", f->dir);
        goto out;
    }
    save_licence(it, it->parts[0].how == UDATA_FILE ? NULL : scratch[0]);
    rc = 0;
out:
    if (rc != 0) remove_tree(part);
    for (int i = 0; scratch && i < it->part_count; i++) {
        if (scratch[i] && it->parts[i].how != UDATA_FILE) unlink(scratch[i]);
        free(scratch[i]);
    }
    free(scratch);
    free(part);
    free(dest);
    return rc;
}
