// See lib/utar.h.
#include "lib/utar.h"
#include "lib/ubytes.h"
#include "rt/sys.h"
#include <caltime.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK 512
#define CHUNK (64u * 1024u)
// A .tar.gz is inflated from memory; one bigger than this is refused.
#define TGZ_MAX (512ul * 1024 * 1024)

static int failf(char *err, int cap, int rc, const char *fmt, ...) {
    if (err && cap > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, (size_t)cap, fmt, ap);
        va_end(ap);
    }
    return rc;
}

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

enum utar_kind utar_kind_of(const char *path) {
    if (!path) return UTAR_NONE;
    if (ends_with(path, ".tar")) return UTAR_TAR;
    if (ends_with(path, ".tar.gz") || ends_with(path, ".tgz")) return UTAR_TGZ;
    if (ends_with(path, ".gz")) return UTAR_GZ;
    return UTAR_NONE;
}

// --- the stream parser ------------------------------------------------
//
// Fed the tar stream a piece at a time, from whichever source has it. A
// header is gathered whole, then the member's data passes through to
// `on_data` (when `on_entry` asked for it) or is skipped, then the
// padding to the next 512.

enum { P_HEADER, P_DATA, P_PAD, P_META, P_DONE };

struct parser {
    int state;
    unsigned char hdr[BLOCK];
    unsigned hfill;
    unsigned long long left;
    unsigned pad;
    // A 'L' (GNU long name) or 'x' (pax) member's data, gathered: it
    // names the member that follows.
    char meta[4096];
    unsigned mfill;
    int meta_type;
    char next_name[UTAR_NAME_MAX];
    int next_too_long;      // the name it gave does not fit: skip that member
    // What the current member is, and whether its data is wanted.
    char name[UTAR_NAME_MAX];
    unsigned long long size;
    int capture;
    // The consumer. on_entry: 0 skip the data, 1 hand it to on_data, -1 stop.
    int (*on_entry)(void *ctx, const char *name, unsigned long long size, int64_t mtime, int is_dir);
    int (*on_data)(void *ctx, const unsigned char *data, unsigned long n);
    // A captured member's last byte has gone to on_data. Non-zero stops.
    int (*on_end)(void *ctx);
    void *ctx;
    int stop;
    char *err;
    int errcap;
    int rc;
};

static unsigned long long octal(const unsigned char *p, int n) {
    // Base-256, GNU's form for a size past 8 GiB: the top bit set.
    if (p[0] & 0x80) {
        unsigned long long v = p[0] & 0x7F;
        for (int i = 1; i < n; i++) v = (v << 8) | p[i];
        return v;
    }
    unsigned long long v = 0;
    int i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (unsigned)(p[i] - '0');
    return v;
}

static int all_zero(const unsigned char *b) {
    for (int i = 0; i < BLOCK; i++) if (b[i]) return 0;
    return 1;
}

// A pax extended header: "LEN key=value\n" records. Only `path` is read.
static void pax_path(struct parser *p) {
    unsigned i = 0;
    while (i < p->mfill) {
        unsigned len = 0, j = i;
        while (j < p->mfill && p->meta[j] >= '0' && p->meta[j] <= '9') len = len * 10 + (unsigned)(p->meta[j++] - '0');
        if (!len || i + len > p->mfill || j >= p->mfill || p->meta[j] != ' ') return;
        const char *kv = p->meta + j + 1;
        unsigned kvlen = i + len - (j + 1);
        if (kvlen > 5 && !strncmp(kv, "path=", 5)) {
            unsigned vl = kvlen - 5;
            if (vl && kv[5 + vl - 1] == '\n') vl--;
            if (vl < sizeof p->next_name) { memcpy(p->next_name, kv + 5, vl); p->next_name[vl] = 0; }
            else p->next_too_long = 1;
        }
        i += len;
    }
}

static void header(struct parser *p) {
    unsigned char *h = p->hdr;
    if (all_zero(h)) { p->state = P_DONE; return; }
    unsigned long sum = 0;
    for (int i = 0; i < BLOCK; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (octal(h + 148, 8) != sum) {
        p->rc = failf(p->err, p->errcap, -EINVAL, "a header's checksum does not match -- not a tar, or damaged");
        p->state = P_DONE;
        return;
    }
    int type = h[156];
    unsigned long long size = octal(h + 124, 12);
    int64_t mtime = (int64_t)octal(h + 136, 12);

    if (type == 'L' || type == 'x') {      // names the next member
        p->meta_type = type;
        p->mfill = 0;
        p->left = size;
        p->pad = (unsigned)((BLOCK - size % BLOCK) % BLOCK);
        p->state = P_META;
        return;
    }
    if (type == 'g') {                      // pax global: nothing used
        p->capture = 0;
        p->left = size;
        p->pad = (unsigned)((BLOCK - size % BLOCK) % BLOCK);
        p->state = size ? P_DATA : P_PAD;
        return;
    }

    // A NAME THAT DOES NOT FIT IS SKIPPED, never truncated: a cut name
    // is a different file's, and extracting under it would be a guess.
    int too_long = p->next_too_long;
    p->next_too_long = 0;
    if (p->next_name[0]) {
        snprintf(p->name, sizeof p->name, "%s", p->next_name);
        p->next_name[0] = 0;
    } else {
        char nm[101], pre[156];
        memcpy(nm, h, 100); nm[100] = 0;
        memcpy(pre, h + 345, 155); pre[155] = 0;
        int n = (!memcmp(h + 257, "ustar", 5) && pre[0])
            ? snprintf(p->name, sizeof p->name, "%s/%s", pre, nm)
            : snprintf(p->name, sizeof p->name, "%s", nm);
        if (n >= (int)sizeof p->name) too_long = 1;
    }
    if (too_long) p->name[0] = 0;   // neither listed nor extracted
    // "./a/b" is "a/b": the leading ./ GNU tar writes for `tar c .`.
    while (p->name[0] == '.' && p->name[1] == '/') memmove(p->name, p->name + 2, strlen(p->name + 2) + 1);

    int is_dir = type == '5';
    int is_file = type == '0' || type == 0 || type == '7';
    if (is_dir) {
        size_t l = strlen(p->name);
        if (l && p->name[l - 1] != '/' && l + 1 < sizeof p->name) { p->name[l] = '/'; p->name[l + 1] = 0; }
        size = 0;
    }
    p->size = size;
    p->capture = 0;
    if ((is_dir || is_file) && p->name[0]) {
        int r = p->on_entry ? p->on_entry(p->ctx, p->name, size, mtime, is_dir) : 0;
        if (r < 0) { p->stop = 1; p->state = P_DONE; return; }
        p->capture = r > 0;
    }
    p->left = is_dir ? 0 : size;
    p->pad = (unsigned)((BLOCK - p->left % BLOCK) % BLOCK);
    p->state = p->left ? P_DATA : (p->pad ? P_PAD : P_HEADER);
    if (p->capture && !p->left) {
        if (p->on_data) p->on_data(p->ctx, 0, 0);
        if (p->on_end && p->on_end(p->ctx)) { p->stop = 1; p->state = P_DONE; }
    }
}

// Returns 0 to go on, 1 when the walk is over (finished, stopped, failed).
static int feed(struct parser *p, const unsigned char *d, unsigned long n) {
    while (n && p->state != P_DONE) {
        if (p->state == P_HEADER) {
            unsigned take = BLOCK - p->hfill;
            if (take > n) take = (unsigned)n;
            memcpy(p->hdr + p->hfill, d, take);
            p->hfill += take; d += take; n -= take;
            if (p->hfill == BLOCK) { p->hfill = 0; header(p); }
        } else if (p->state == P_DATA || p->state == P_META) {
            unsigned long take = p->left < n ? (unsigned long)p->left : n;
            if (p->state == P_META) {
                unsigned room = (unsigned)sizeof p->meta - 1 - p->mfill;
                unsigned keep = take < room ? (unsigned)take : room;
                memcpy(p->meta + p->mfill, d, keep);
                p->mfill += keep;
            } else if (p->capture && p->on_data) {
                if (p->on_data(p->ctx, d, take)) { p->stop = 1; p->state = P_DONE; return 1; }
            }
            p->left -= take; d += take; n -= take;
            if (!p->left && p->state == P_DATA && p->capture && p->on_end && p->on_end(p->ctx)) {
                p->stop = 1;
                p->state = P_DONE;
                return 1;
            }
            if (!p->left) {
                if (p->state == P_META) {
                    p->meta[p->mfill] = 0;
                    if (p->meta_type == 'L') {
                        if (strlen(p->meta) >= sizeof p->next_name || p->mfill >= sizeof p->meta - 1)
                            p->next_too_long = 1;
                        else memcpy(p->next_name, p->meta, strlen(p->meta) + 1);
                    } else {
                        pax_path(p);
                    }
                }
                p->state = p->pad ? P_PAD : P_HEADER;
            }
        } else if (p->state == P_PAD) {
            unsigned take = p->pad < n ? p->pad : (unsigned)n;
            p->pad -= take; d += take; n -= take;
            if (!p->pad) p->state = P_HEADER;
        }
    }
    return p->state == P_DONE;
}

// --- the sources ------------------------------------------------------

static int gz_out(void *ctx, const uint8_t *data, size_t n) {
    struct parser *p = ctx;
    return feed(p, data, (unsigned long)n) ? 1 : 0;   // non-zero stops the inflate
}

// The whole compressed file, malloc'd. NULL with `err` set.
static unsigned char *slurp(const char *path, unsigned long *len, char *err, int errcap) {
    struct sys_stat st;
    if (sys_stat(path, &st) != 0) { failf(err, errcap, -ENOENT, "cannot open %s", path); return 0; }
    if (st.size > TGZ_MAX) { failf(err, errcap, -E2BIG, "%s is too big to open here", path); return 0; }
    unsigned long n = (unsigned long)st.size;
    unsigned char *buf = malloc(n ? n : 1);
    if (!buf) { failf(err, errcap, -ENOMEM, "out of memory for %s", path); return 0; }
    int fd = open(path, O_RDONLY);
    if (fd < 0 || ub_read_at(fd, 0, buf, n) < 0) {
        if (fd >= 0) close(fd);
        free(buf);
        failf(err, errcap, -EIO, "could not read %s", path);
        return 0;
    }
    close(fd);
    *len = n;
    return buf;
}

// Runs the parser over the archive at `path`, whichever kind it is.
static int walk(const char *path, struct parser *p) {
    enum utar_kind k = utar_kind_of(path);
    if (k == UTAR_TAR) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) return failf(p->err, p->errcap, -ENOENT, "cannot open %s", path);
        unsigned char *buf = malloc(CHUNK);
        if (!buf) { close(fd); return failf(p->err, p->errcap, -ENOMEM, "out of memory"); }
        for (;;) {
            long r = read(fd, buf, CHUNK);
            if (r < 0) { p->rc = failf(p->err, p->errcap, -EIO, "could not read %s", path); break; }
            if (r == 0 || feed(p, buf, (unsigned long)r)) break;
        }
        free(buf);
        close(fd);
    } else if (k == UTAR_TGZ) {
        unsigned long n = 0;
        unsigned char *src = slurp(path, &n, p->err, p->errcap);
        if (!src) return -EIO;
        struct uinflate_scratch *sc = malloc(sizeof *sc);
        size_t outlen = 0;
        int rc = sc ? uinflate(src, n, UINFLATE_GZIP, sc, gz_out, p, &outlen) : -ENOMEM;
        free(sc);
        free(src);
        // -EIO is the parser stopping the inflate, which is not a failure.
        if (rc < 0 && rc != -EIO && !p->rc)
            p->rc = failf(p->err, p->errcap, -EINVAL, "%s: %s", path, uinflate_error());
    } else {
        return failf(p->err, p->errcap, -EINVAL, "%s is not a tar archive", path);
    }
    if (!p->rc && !p->stop && p->state != P_DONE && (p->state != P_HEADER || p->hfill))
        p->rc = failf(p->err, p->errcap, -EINVAL, "%s ends in the middle of a member", path);
    return p->rc;
}

// A lone .gz: the member's name is the file's without the .gz, its size
// the trailer's ISIZE (the length mod 2^32, which is what gzip -l shows).
static void gz_member(const char *path, char *name, int cap) {
    const char *b = strrchr(path, '/');
    snprintf(name, (size_t)cap, "%s", b ? b + 1 : path);
    size_t l = strlen(name);
    if (l > 3) name[l - 3] = 0;
}

// The listing callback's "non-zero stops" as the parser's -1; a listing
// never wants a member's data.
struct lister { utar_entry_fn fn; void *ctx; };
static int list_entry(void *ctx, const char *name, unsigned long long size, int64_t mtime, int is_dir) {
    struct lister *l = ctx;
    return l->fn(l->ctx, name, size, mtime, is_dir) ? -1 : 0;
}

int utar_list(const char *path, utar_entry_fn fn, void *ctx, char *err, int errcap) {
    if (err && errcap > 0) err[0] = 0;
    if (utar_kind_of(path) == UTAR_GZ) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) return failf(err, errcap, -ENOENT, "cannot open %s", path);
        struct sys_stat st;
        unsigned char magic[2], isz[4];
        int ok = sys_stat(path, &st) == 0 && st.size >= 18 &&
                 ub_read_at(fd, 0, magic, 2) == 0 && magic[0] == 0x1F && magic[1] == 0x8B &&
                 ub_read_at(fd, (unsigned long)st.size - 4, isz, 4) == 0;
        close(fd);
        if (!ok) return failf(err, errcap, -EINVAL, "%s is not a gzip file", path);
        char name[UTAR_NAME_MAX];
        gz_member(path, name, sizeof name);
        fn(ctx, name, ub_le32(isz), (int64_t)cal_rtc_to_epoch(&st.modified), 0);
        return 0;
    }
    struct parser *p = calloc(1, sizeof *p);
    if (!p) return failf(err, errcap, -ENOMEM, "out of memory");
    struct lister l = { fn, ctx };
    p->on_entry = list_entry;
    p->ctx = &l;
    p->err = err;
    p->errcap = errcap;
    int rc = walk(path, p);
    free(p);
    return rc;
}

// --- extracting -------------------------------------------------------

struct xout {
    const char *want;
    int fd, found, done_member, write_failed, stopped;
    unsigned long long got, total;
    utar_progress progress;
    void *ctx;
};

static int x_entry(void *ctx, const char *name, unsigned long long size, int64_t mtime, int is_dir) {
    (void)mtime;
    struct xout *x = ctx;
    if (x->found) return -1;                       // past it: stop the walk
    if (is_dir || strcmp(name, x->want)) return 0;
    x->found = 1;
    x->total = size;
    return 1;
}

static int x_data(void *ctx, const unsigned char *d, unsigned long n) {
    struct xout *x = ctx;
    if (!n) return 0;
    unsigned long put = 0;
    while (put < n) {
        long w = write(x->fd, d + put, n - put);
        if (w <= 0) { x->write_failed = 1; return 1; }
        put += (unsigned long)w;
    }
    x->got += n;
    if (x->progress && x->progress(x->ctx, (unsigned long)x->got, (unsigned long)x->total)) {
        x->stopped = 1;
        return 1;
    }
    return 0;
}

static int gz_file_out(void *ctx, const uint8_t *data, size_t n) {
    return x_data(ctx, data, (unsigned long)n);
}

int utar_extract(const char *path, const char *name, const char *out_path,
                 utar_progress progress, void *ctx, char *err, int errcap) {
    if (err && errcap > 0) err[0] = 0;
    char part[512];
    if (snprintf(part, sizeof part, "%s.part", out_path) >= (int)sizeof part)
        return failf(err, errcap, -ENAMETOOLONG, "%s: name too long", out_path);
    struct xout x = { .want = name, .progress = progress, .ctx = ctx };
    x.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (x.fd < 0) return failf(err, errcap, -EACCES, "cannot write %s", part);

    int rc = 0;
    if (utar_kind_of(path) == UTAR_GZ) {
        char member[UTAR_NAME_MAX];
        gz_member(path, member, sizeof member);
        if (strcmp(member, name)) {
            rc = failf(err, errcap, -ENOENT, "%s is not in %s", name, path);
        } else {
            unsigned long n = 0;
            unsigned char *src = slurp(path, &n, err, errcap);
            if (!src) rc = -EIO;
            else {
                x.found = 1;
                x.total = n >= 4 ? ub_le32(src + n - 4) : 0;
                struct uinflate_scratch *sc = malloc(sizeof *sc);
                size_t outlen = 0;
                int r = sc ? uinflate(src, n, UINFLATE_GZIP, sc, gz_file_out, &x, &outlen) : -ENOMEM;
                free(sc);
                free(src);
                if (r < 0 && !x.stopped && !x.write_failed)
                    rc = failf(err, errcap, -EINVAL, "%s: %s", path, uinflate_error());
                x.total = x.got;   // ISIZE is mod 2^32; the stream's own end is the truth
            }
        }
    } else {
        struct parser *p = calloc(1, sizeof *p);
        if (!p) rc = failf(err, errcap, -ENOMEM, "out of memory");
        else {
            p->on_entry = x_entry;
            p->on_data = x_data;
            p->ctx = &x;
            p->err = err;
            p->errcap = errcap;
            rc = walk(path, p);
            free(p);
        }
        if (!rc && !x.found) rc = failf(err, errcap, -ENOENT, "%s is not in %s", name, path);
    }
    if (fsync(x.fd) != 0) x.write_failed = 1;
    close(x.fd);
    if (!rc && x.stopped) rc = failf(err, errcap, -EINTR, "stopped");
    if (!rc && x.write_failed) rc = failf(err, errcap, -EIO, "writing %s failed", part);
    if (!rc && x.got != x.total)
        rc = failf(err, errcap, -EINVAL, "%s came out at %llu bytes of %llu", name, x.got, x.total);
    if (rc < 0) { unlink(part); return rc; }
    if (rename(part, out_path) != 0) {
        unlink(part);
        return failf(err, errcap, -EIO, "cannot rename %s into place", part);
    }
    return 0;
}

// --- many in one pass --------------------------------------------------

struct many {
    utar_route route;
    void *ctx;
    utar_progress progress;
    void *pctx;
    int fd;
    char out[512], part[512];
    unsigned long long got, size, done_all;
    char *err;
    int errcap, rc;
};

static int m_entry(void *ctx, const char *name, unsigned long long size, int64_t mtime, int is_dir) {
    (void)mtime;
    struct many *m = ctx;
    if (!m->route(m->ctx, name, size, is_dir, m->out, sizeof m->out) || is_dir) return 0;
    if (snprintf(m->part, sizeof m->part, "%s.part", m->out) >= (int)sizeof m->part) {
        m->rc = failf(m->err, m->errcap, -ENAMETOOLONG, "%s: name too long", m->out);
        return -1;
    }
    m->fd = open(m->part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (m->fd < 0) { m->rc = failf(m->err, m->errcap, -EACCES, "cannot write %s", m->part); return -1; }
    m->got = 0;
    m->size = size;
    return 1;
}

static int m_data(void *ctx, const unsigned char *d, unsigned long n) {
    struct many *m = ctx;
    unsigned long put = 0;
    while (put < n) {
        long w = write(m->fd, d + put, n - put);
        if (w <= 0) { m->rc = failf(m->err, m->errcap, -EIO, "writing %s failed", m->part); return 1; }
        put += (unsigned long)w;
    }
    m->got += n;
    m->done_all += n;
    if (n && m->progress && m->progress(m->pctx, (unsigned long)m->done_all, 0)) {
        m->rc = failf(m->err, m->errcap, -EINTR, "stopped");
        return 1;
    }
    return 0;
}

static int m_end(void *ctx) {
    struct many *m = ctx;
    int bad = fsync(m->fd) != 0;
    close(m->fd);
    m->fd = -1;
    if (bad || m->got != m->size) {
        unlink(m->part);
        m->rc = failf(m->err, m->errcap, -EIO, "%s came out short", m->out);
        return 1;
    }
    if (rename(m->part, m->out) != 0) {
        unlink(m->part);
        m->rc = failf(m->err, m->errcap, -EIO, "cannot rename %s into place", m->part);
        return 1;
    }
    return 0;
}

int utar_extract_many(const char *path, utar_route route, void *ctx,
                      utar_progress progress, void *pctx, char *err, int errcap) {
    if (err && errcap > 0) err[0] = 0;
    if (utar_kind_of(path) == UTAR_GZ) {           // one member: route it, then extract
        char member[UTAR_NAME_MAX], out[512];
        gz_member(path, member, sizeof member);
        if (!route(ctx, member, 0, 0, out, sizeof out)) return 0;
        return utar_extract(path, member, out, progress, pctx, err, errcap);
    }
    struct many m = { route, ctx, progress, pctx, -1, "", "", 0, 0, 0, err, errcap, 0 };
    struct parser *p = calloc(1, sizeof *p);
    if (!p) return failf(err, errcap, -ENOMEM, "out of memory");
    p->on_entry = m_entry;
    p->on_data = m_data;
    p->on_end = m_end;
    p->ctx = &m;
    p->err = err;
    p->errcap = errcap;
    int rc = walk(path, p);
    free(p);
    if (m.fd >= 0) { close(m.fd); unlink(m.part); }   // a walk that ended mid-member
    return m.rc ? m.rc : rc;
}

// --- writing ----------------------------------------------------------

static int raw_write(int fd, const void *b, unsigned long n) {
    const unsigned char *p = b;
    while (n) {
        long w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (unsigned long)w;
    }
    return 0;
}

// Sends the buffered tar bytes on: gzipped as one piece, or as they are.
static int emit(struct utar_writer *w, int last) {
    if (!w->gz) {
        int r = w->fill ? raw_write(w->fd, w->buf, w->fill) : 0;
        w->fill = 0;
        return r;
    }
    size_t zl = 0;
    if (udeflate_gz_piece(&w->gzs, w->buf, w->fill, last, w->zbuf, w->zcap, &zl) != 0) return -1;
    w->fill = 0;
    return raw_write(w->fd, w->zbuf, zl);
}

static int put(struct utar_writer *w, const void *d, unsigned long n) {
    const unsigned char *p = d;
    while (n) {
        unsigned long room = CHUNK - w->fill, take = n < room ? n : room;
        memcpy(w->buf + w->fill, p, take);
        w->fill += take; p += take; n -= take;
        if (w->fill == CHUNK && emit(w, 0)) return -1;
    }
    return 0;
}

int utar_create_begin(struct utar_writer *w, const char *out_path, int gzip, int level,
                      char *err, int errcap) {
    memset(w, 0, sizeof *w);
    w->fd = -1;
    if (snprintf(w->out, sizeof w->out, "%s", out_path) >= (int)sizeof w->out ||
        snprintf(w->part, sizeof w->part, "%s.part", out_path) >= (int)sizeof w->part)
        return failf(err, errcap, -ENAMETOOLONG, "%s: name too long", out_path);
    w->gz = gzip;
    w->gzs.level = level ? (enum udeflate_level)level : UDEFLATE_NORMAL;
    w->buf = malloc(CHUNK);
    w->zcap = (unsigned long)udeflate_bound(CHUNK) + 64;
    w->zbuf = gzip ? malloc(w->zcap) : 0;
    if (!w->buf || (gzip && !w->zbuf)) { utar_create_abort(w); return failf(err, errcap, -ENOMEM, "out of memory"); }
    w->fd = open(w->part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (w->fd < 0) { utar_create_abort(w); return failf(err, errcap, -EACCES, "cannot write %s", w->part); }
    return 0;
}

static void oct(unsigned char *f, int width, unsigned long long v) {
    // width-1 octal digits and a NUL, the form every tar reads.
    f[width - 1] = 0;
    for (int i = width - 2; i >= 0; i--) { f[i] = (unsigned char)('0' + (v & 7)); v >>= 3; }
}

// One ustar header. A name ustar cannot hold (100 bytes, or a 155-byte
// prefix and 100 at a '/') goes first as a GNU long-name member.
static int header_out(struct utar_writer *w, const char *name, int type,
                      unsigned long long size, int64_t mtime, unsigned mode) {
    unsigned char h[BLOCK];
    size_t len = strlen(name);
    const char *base = name, *prefix = "";
    size_t plen = 0;
    if (len > 100) {
        const char *slash = 0;
        for (const char *s = name; *s; s++)
            if (*s == '/' && (size_t)(s - name) <= 155 && strlen(s + 1) <= 100 && s + 1 != name + len) slash = s;
        if (slash) { prefix = name; plen = (size_t)(slash - name); base = slash + 1; }
        else {
            memset(h, 0, sizeof h);
            memcpy(h, "././@LongLink", 13);
            oct(h + 100, 8, 0644); oct(h + 108, 8, 0); oct(h + 116, 8, 0);
            oct(h + 124, 12, len + 1); oct(h + 136, 12, 0);
            h[156] = 'L';
            memcpy(h + 257, "ustar  ", 8);
            memset(h + 148, ' ', 8);
            unsigned long sum = 0;
            for (int i = 0; i < BLOCK; i++) sum += h[i];
            oct(h + 148, 7, sum); h[155] = ' ';
            unsigned char pad[BLOCK];
            memset(pad, 0, sizeof pad);
            if (put(w, h, BLOCK) || put(w, name, len + 1) ||
                put(w, pad, (BLOCK - (len + 1) % BLOCK) % BLOCK)) return -1;
            base = name;   // truncated below; the long name is what readers use
        }
    }
    memset(h, 0, sizeof h);
    memcpy(h, base, strlen(base) < 100 ? strlen(base) : 100);
    oct(h + 100, 8, mode);
    oct(h + 108, 8, 0);
    oct(h + 116, 8, 0);
    oct(h + 124, 12, size);
    oct(h + 136, 12, mtime > 0 ? (unsigned long long)mtime : 0);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memcpy(h + 265, "root", 4);
    memcpy(h + 297, "root", 4);
    if (plen) memcpy(h + 345, prefix, plen);
    memset(h + 148, ' ', 8);
    unsigned long sum = 0;
    for (int i = 0; i < BLOCK; i++) sum += h[i];
    oct(h + 148, 7, sum);
    h[155] = ' ';
    return put(w, h, BLOCK);
}

static int64_t mtime_of(const char *path) {
    struct sys_stat st;
    return sys_stat(path, &st) == 0 ? (int64_t)cal_rtc_to_epoch(&st.modified) : 0;
}

int utar_add_dir(struct utar_writer *w, const char *name, char *err, int errcap) {
    char n[UTAR_NAME_MAX + 2];
    size_t l = strlen(name);
    if (snprintf(n, sizeof n, "%s%s", name, l && name[l - 1] == '/' ? "" : "/") >= (int)sizeof n)
        return failf(err, errcap, -ENAMETOOLONG, "%s: name too long", name);
    if (header_out(w, n, '5', 0, 0, 0755)) return failf(err, errcap, -EIO, "writing %s failed", w->part);
    return 0;
}

int utar_add_file(struct utar_writer *w, const char *name, const char *src_path,
                  utar_progress progress, void *ctx, char *err, int errcap) {
    struct sys_stat st;
    if (sys_stat(src_path, &st) != 0) return failf(err, errcap, -ENOENT, "cannot read %s", src_path);
    if (strlen(name) >= UTAR_NAME_MAX) return failf(err, errcap, -ENAMETOOLONG, "%s: name too long", name);
    unsigned long long size = st.size;
    if (size >= (1ull << 33)) return failf(err, errcap, -E2BIG, "%s is too big for a tar here", src_path);
    int fd = open(src_path, O_RDONLY);
    if (fd < 0) return failf(err, errcap, -EACCES, "cannot open %s", src_path);
    if (header_out(w, name, '0', size, mtime_of(src_path), 0644)) {
        close(fd);
        return failf(err, errcap, -EIO, "writing %s failed", w->part);
    }
    unsigned char *buf = malloc(CHUNK);
    if (!buf) { close(fd); return failf(err, errcap, -ENOMEM, "out of memory"); }
    unsigned long long done = 0;
    int rc = 0;
    while (done < size) {
        long r = read(fd, buf, CHUNK);
        if (r <= 0) break;
        if ((unsigned long long)r > size - done) r = (long)(size - done);
        if (put(w, buf, (unsigned long)r)) { rc = failf(err, errcap, -EIO, "writing %s failed", w->part); break; }
        done += (unsigned long long)r;
        if (progress && progress(ctx, (unsigned long)done, (unsigned long)size)) {
            rc = failf(err, errcap, -EINTR, "stopped");
            break;
        }
    }
    free(buf);
    close(fd);
    if (!rc && done != size) rc = failf(err, errcap, -EIO, "%s changed while it was read", src_path);
    if (!rc) {
        unsigned char pad[BLOCK];
        memset(pad, 0, sizeof pad);
        if (put(w, pad, (unsigned long)((BLOCK - size % BLOCK) % BLOCK)))
            rc = failf(err, errcap, -EIO, "writing %s failed", w->part);
    }
    return rc;
}

int utar_create_end(struct utar_writer *w, char *err, int errcap) {
    unsigned char zero[BLOCK * 2];
    memset(zero, 0, sizeof zero);
    int bad = put(w, zero, sizeof zero) || emit(w, 1) || fsync(w->fd) != 0;
    close(w->fd);
    w->fd = -1;
    free(w->buf); free(w->zbuf);
    w->buf = w->zbuf = 0;
    if (bad) { unlink(w->part); return failf(err, errcap, -EIO, "finishing %s failed", w->part); }
    if (rename(w->part, w->out) != 0) {
        unlink(w->part);
        return failf(err, errcap, -EIO, "cannot rename %s into place", w->part);
    }
    return 0;
}

void utar_create_abort(struct utar_writer *w) {
    if (w->fd >= 0) { close(w->fd); w->fd = -1; unlink(w->part); }
    free(w->buf); free(w->zbuf);
    w->buf = w->zbuf = 0;
}
