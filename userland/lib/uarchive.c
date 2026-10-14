// See lib/uarchive.h.
#include "lib/uarchive.h"
#include "lib/uzip.h"
#include "lib/utar.h"
#include "lib/uwalk.h"
#include "rt/sys.h"
#include <caltime.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int failf(char *err, int cap, int rc, const char *fmt, ...) {
    if (err && cap > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, (size_t)cap, fmt, ap);
        va_end(ap);
    }
    return rc;
}

static int ends_with(const char *s, size_t n, const char *suf) {
    size_t b = strlen(suf);
    return n >= b && !strncasecmp(s + n - b, suf, b);
}

// The kind a name of length `n` ends in -- longest suffix first, so
// "x.tar.gz" is a tar and not a lone gzip.
static enum uarchive_kind kind_n(const char *s, size_t n) {
    if (ends_with(s, n, ".zip")) return UARC_ZIP;
    if (ends_with(s, n, ".tar.gz") || ends_with(s, n, ".tgz")) return UARC_TGZ;
    if (ends_with(s, n, ".tar")) return UARC_TAR;
    if (ends_with(s, n, ".gz")) return UARC_GZ;
    return UARC_NONE;
}

enum uarchive_kind uarchive_kind_of(const char *path) {
    return path ? kind_n(path, strlen(path)) : UARC_NONE;
}

const char *uarchive_suffix(enum uarchive_kind k) {
    switch (k) {   // dispatch-ok: one name per kind
    case UARC_ZIP: return ".zip";
    case UARC_TAR: return ".tar";
    case UARC_TGZ: return ".tar.gz";
    case UARC_GZ:  return ".gz";
    default:       return "";
    }
}

const char *uarchive_label(enum uarchive_kind k) {
    switch (k) {   // dispatch-ok: one label per kind
    case UARC_ZIP: return "ZIP";
    case UARC_TAR: return "tar";
    case UARC_TGZ: return "tar + gzip";
    case UARC_GZ:  return "gzip";
    default:       return "";
    }
}

int uarchive_split(const char *path, char *archive, int acap, const char **inner) {
    for (const char *s = path; *s; s++) {
        if (s[1] != '/' && s[1] != 0) continue;
        size_t n = (size_t)(s + 1 - path);
        if (kind_n(path, n) == UARC_NONE) continue;
        if ((int)n >= acap) return 0;
        memcpy(archive, path, n);
        archive[n] = 0;
        *inner = s[1] ? s + 2 : s + 1;
        return 1;
    }
    return 0;
}

int uarchive_stem(const char *path, char *out, int cap) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t n = strlen(b);
    enum uarchive_kind k = kind_n(b, n);
    size_t cut = k == UARC_NONE ? 0 : (k == UARC_TGZ && ends_with(b, n, ".tgz")) ? 4 : strlen(uarchive_suffix(k));
    if (n - cut + 1 > (size_t)cap || n == cut) return 0;
    memcpy(out, b, n - cut);
    out[n - cut] = 0;
    return 1;
}

// --- listing ----------------------------------------------------------

struct lst { uarchive_fn fn; void *ctx; };

static int from_zip(void *ctx, const char *name, unsigned long size, unsigned long packed,
                    unsigned d, unsigned t) {
    struct lst *l = ctx;
    struct uarchive_entry e = { name, size, packed, 1, 0, { 0 } };
    size_t n = strlen(name);
    e.is_dir = n && name[n - 1] == '/';
    e.modified.year = (uint16_t)(1980 + (d >> 9));
    e.modified.month = (uint8_t)((d >> 5) & 15);
    e.modified.day = (uint8_t)(d & 31);
    e.modified.hour = (uint8_t)(t >> 11);
    e.modified.minute = (uint8_t)((t >> 5) & 63);
    e.modified.second = (uint8_t)((t & 31) * 2);
    return l->fn(l->ctx, &e);
}

static int from_tar(void *ctx, const char *name, unsigned long long size, int64_t mtime, int is_dir) {
    struct lst *l = ctx;
    struct uarchive_entry e = { name, size, 0, 0, is_dir, { 0 } };
    cal_epoch_to_rtc(mtime > 0 ? (uint64_t)mtime : 0, &e.modified);
    return l->fn(l->ctx, &e);
}

int uarchive_list(const char *path, uarchive_fn fn, void *ctx, char *err, int errcap) {
    struct lst l = { fn, ctx };
    enum uarchive_kind k = uarchive_kind_of(path);
    if (k == UARC_ZIP) return uzip_list(path, from_zip, &l, err, errcap);
    if (k != UARC_NONE) return utar_list(path, from_tar, &l, err, errcap);
    return failf(err, errcap, -EINVAL, "%s is not an archive", path);
}

// --- extracting -------------------------------------------------------

struct prog { uarchive_progress fn; void *ctx; unsigned long long base, total; };

static int member_prog(void *ctx, unsigned long done, unsigned long total) {
    struct prog *p = ctx;
    (void)total;
    return p->fn ? p->fn(p->ctx, p->base + done, p->total) : 0;
}

int uarchive_extract(const char *path, const char *member, const char *out_path,
                     uarchive_progress progress, void *ctx, char *err, int errcap) {
    struct prog p = { progress, ctx, 0, 0 };
    enum uarchive_kind k = uarchive_kind_of(path);
    if (k == UARC_ZIP)
        return uzip_extract(path, member, out_path, progress ? member_prog : 0, &p, err, errcap);
    if (k != UARC_NONE)
        return utar_extract(path, member, out_path, progress ? member_prog : 0, &p, err, errcap);
    return failf(err, errcap, -EINVAL, "%s is not an archive", path);
}

struct many { uarchive_route route; void *ctx; const char *path; uarchive_progress progress; void *pctx;
               char *err; int errcap; int rc; };

static int many_zip(void *ctx, const char *name, unsigned long size, unsigned long packed,
                    unsigned d, unsigned t) {
    struct many *m = ctx;
    struct uarchive_entry e = { name, size, packed, 1, 0, { 0 } };
    size_t n = strlen(name);
    e.is_dir = n && name[n - 1] == '/';
    (void)d; (void)t;
    char out[512];
    if (!m->route(m->ctx, &e, out, sizeof out) || e.is_dir) return 0;
    struct prog p = { m->progress, m->pctx, 0, 0 };
    m->rc = uzip_extract(m->path, name, out, m->progress ? member_prog : 0, &p, m->err, m->errcap);
    return m->rc != 0;
}

struct troute { uarchive_route route; void *ctx; };
static int tar_route(void *ctx, const char *name, unsigned long long size, int is_dir,
                     char *out, int cap) {
    struct troute *t = ctx;
    struct uarchive_entry e = { name, size, 0, 0, is_dir, { 0 } };
    return t->route(t->ctx, &e, out, cap);
}

static int tar_prog(void *ctx, unsigned long done, unsigned long total) {
    struct prog *p = ctx;
    (void)total;
    return p->fn ? p->fn(p->ctx, done, 0) : 0;
}

int uarchive_extract_many(const char *path, uarchive_route route, void *ctx,
                          uarchive_progress progress, void *pctx, char *err, int errcap) {
    enum uarchive_kind k = uarchive_kind_of(path);
    if (k == UARC_ZIP) {
        struct many m = { route, ctx, path, progress, pctx, err, errcap, 0 };
        int rc = uzip_list(path, many_zip, &m, err, errcap);
        return m.rc ? m.rc : rc;
    }
    if (k == UARC_NONE) return failf(err, errcap, -EINVAL, "%s is not an archive", path);
    struct troute t = { route, ctx };
    struct prog p = { progress, pctx, 0, 0 };
    return utar_extract_many(path, tar_route, &t, progress ? tar_prog : 0, &p, err, errcap);
}

// --- creating ---------------------------------------------------------

struct make {
    enum uarchive_kind kind;
    int level;
    struct uzip_writer zw;
    struct utar_writer tw;
    size_t dlen;                 // strlen(dir) + 1: a path's member name starts there
    struct prog p;
    char *err;
    int errcap;
    int rc;
};

static int add_one(struct make *m, const char *path, const char *name, int is_dir,
                   unsigned long long size) {
    if (m->kind == UARC_ZIP)
        m->rc = is_dir ? uzip_add_dir(&m->zw, name, m->err, m->errcap)
                       : uzip_add_file(&m->zw, name, path, m->level, member_prog, &m->p, m->err, m->errcap);
    else
        m->rc = is_dir ? utar_add_dir(&m->tw, name, m->err, m->errcap)
                       : utar_add_file(&m->tw, name, path, member_prog, &m->p, m->err, m->errcap);
    if (!is_dir) m->p.base += size;
    return m->rc;
}

static int walk_add(void *ctx, const char *path, const struct sys_dirent *e) {
    struct make *m = ctx;
    if (strlen(path) <= m->dlen) return 1;
    if (add_one(m, path, path + m->dlen, e->is_dir, e->size)) return -1;
    return 1;
}

static int walk_measure(void *ctx, const char *path, const struct sys_dirent *e) {
    (void)path;
    unsigned long long *total = ctx;
    if (!e->is_dir) *total += e->size;
    return 1;
}

int uarchive_create(const char *out_path, enum uarchive_kind kind, int level,
                    const char *dir, const char *const *names, int count,
                    uarchive_progress progress, void *ctx, char *err, int errcap) {
    if (kind != UARC_ZIP && kind != UARC_TAR && kind != UARC_TGZ)
        return failf(err, errcap, -EINVAL, "that kind of archive cannot be made here");
    struct uwalk *w = malloc(sizeof *w);
    struct make *m = calloc(1, sizeof *m);
    if (!w || !m) { free(w); free(m); return failf(err, errcap, -ENOMEM, "out of memory"); }
    m->kind = kind;
    m->level = level;
    m->err = err;
    m->errcap = errcap;
    m->p.fn = progress;
    m->p.ctx = ctx;
    // "/" is the one dir whose children do not start after a '/' of their own.
    m->dlen = strlen(dir) + (strcmp(dir, "/") ? 1 : 0);

    char path[UWALK_PATH];
    for (int i = 0; i < count; i++) {                    // the total, first
        struct sys_stat st;
        if (snprintf(path, sizeof path, "%s/%s", strcmp(dir, "/") ? dir : "", names[i]) >= (int)sizeof path ||
            sys_stat(path, &st) != 0) continue;
        if (!st.is_dir) m->p.total += st.size;
        else if (uwalk_begin(w, path)) while (uwalk_step(w, walk_measure, &m->p.total)) {}
    }

    int rc = kind == UARC_ZIP ? uzip_create_begin(&m->zw, out_path, err, errcap)
                              : utar_create_begin(&m->tw, out_path, kind == UARC_TGZ, level, err, errcap);
    for (int i = 0; !rc && i < count; i++) {
        struct sys_stat st;
        if (snprintf(path, sizeof path, "%s/%s", strcmp(dir, "/") ? dir : "", names[i]) >= (int)sizeof path) {
            rc = failf(err, errcap, -ENAMETOOLONG, "%s: name too long", names[i]);
            break;
        }
        if (sys_stat(path, &st) != 0) { rc = failf(err, errcap, -ENOENT, "%s is gone", path); break; }
        if (add_one(m, path, names[i], st.is_dir, st.size)) { rc = m->rc; break; }
        if (st.is_dir && uwalk_begin(w, path)) {
            while (uwalk_step(w, walk_add, m)) {}
            if (m->rc) rc = m->rc;
            else if (w->too_deep || w->unreadable)
                rc = failf(err, errcap, -EIO, "%s: %u folder(s) could not be read", names[i],
                           w->too_deep + w->unreadable);
        }
    }
    if (!rc) rc = kind == UARC_ZIP ? uzip_create_end(&m->zw, err, errcap)
                                   : utar_create_end(&m->tw, err, errcap);
    else if (kind == UARC_ZIP) uzip_create_abort(&m->zw);
    else utar_create_abort(&m->tw);
    free(w);
    free(m);
    return rc;
}
