// See lib/uzip.h.
#include "lib/uzip.h"
#include "lib/uinflate.h"
#include "lib/ubytes.h"
#include "rt/sys.h"
#include <kcrc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EOCD_SIG  0x06054b50u
#define CDIR_SIG  0x02014b50u
#define LOCAL_SIG 0x04034b50u
#define EOCD_LEN  22
#define CDIR_LEN  46
#define LOCAL_LEN 30
// The end record sits behind a comment of at most 64 KiB.
#define EOCD_SEARCH (EOCD_LEN + 0xFFFF)
// A directory bigger than this is not an archive anything here asks for.
#define CDIR_MAX (4u * 1024u * 1024u)

static int failf(char *err, int cap, int rc, const char *fmt, ...) {
    if (err && cap > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, (size_t)cap, fmt, ap);
        va_end(ap);
    }
    return rc;
}

struct member {
    unsigned method, flags;
    uint32_t crc, csize, usize, local;
};

// Walks the central directory, calling `fn` with each record (its
// fixed part at `rec`, its name at `name`/`nlen`) until it returns
// non-zero, which is passed back. 0 when every record was walked, or a
// negative errno for an archive this cannot read.
typedef int (*cdir_fn)(void *ctx, const unsigned char *rec, const char *name, unsigned nlen);

static int walk_cdir(int fd, unsigned long fsize, cdir_fn fn, void *ctx, char *err, int cap) {
    unsigned long tail = fsize < EOCD_SEARCH ? fsize : EOCD_SEARCH;
    if (tail < EOCD_LEN) return failf(err, cap, -EINVAL, "not a zip archive (too short)");
    unsigned char *t = malloc(tail);
    if (!t) return failf(err, cap, -ENOMEM, "out of memory for the archive's end");
    if (ub_read_at(fd, fsize - tail, t, tail) < 0) {
        free(t);
        return failf(err, cap, -EIO, "could not read the archive's end");
    }
    // The LAST signature whose comment length lands exactly on the end:
    // a signature inside the comment would otherwise be believed.
    long at = -1;
    for (long i = (long)tail - EOCD_LEN; i >= 0; i--)
        if (ub_le32(t + i) == EOCD_SIG && (unsigned long)i + EOCD_LEN + ub_le16(t + i + 20) == tail) {
            at = i;
            break;
        }
    if (at < 0) { free(t); return failf(err, cap, -EINVAL, "not a zip archive (no end record)"); }
    uint32_t cd_size = ub_le32(t + at + 12), cd_off = ub_le32(t + at + 16);
    unsigned entries = ub_le16(t + at + 10);
    free(t);
    if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || entries == 0xFFFF)
        return failf(err, cap, -ENOTSUP, "a zip64 archive, which this does not read");
    if (cd_size > CDIR_MAX || (unsigned long)cd_off + cd_size > fsize)
        return failf(err, cap, -EINVAL, "the archive's directory does not fit the file");

    unsigned char *cd = malloc(cd_size ? cd_size : 1);
    if (!cd) return failf(err, cap, -ENOMEM, "out of memory for the archive's directory");
    if (ub_read_at(fd, cd_off, cd, cd_size) < 0) {
        free(cd);
        return failf(err, cap, -EIO, "could not read the archive's directory");
    }
    uint32_t p = 0;
    int rc = 0;
    for (unsigned e = 0; e < entries; e++) {
        if (p + CDIR_LEN > cd_size || ub_le32(cd + p) != CDIR_SIG) {
            rc = failf(err, cap, -EINVAL, "the archive's directory is damaged");
            break;
        }
        unsigned fl = ub_le16(cd + p + 28), xl = ub_le16(cd + p + 30), cl = ub_le16(cd + p + 32);
        if ((unsigned long)p + CDIR_LEN + fl + xl + cl > cd_size) {
            rc = failf(err, cap, -EINVAL, "the archive's directory is damaged");
            break;
        }
        rc = fn(ctx, cd + p, (const char *)cd + p + CDIR_LEN, fl);
        if (rc) break;
        p += CDIR_LEN + fl + xl + cl;
    }
    free(cd);
    return rc;
}

struct find { const char *name; size_t nlen; struct member *m; };

static int find_one(void *ctx, const unsigned char *rec, const char *name, unsigned nlen) {
    struct find *f = ctx;
    if (nlen != f->nlen || memcmp(name, f->name, nlen)) return 0;
    f->m->flags = ub_le16(rec + 8);
    f->m->method = ub_le16(rec + 10);
    f->m->crc = ub_le32(rec + 16);
    f->m->csize = ub_le32(rec + 20);
    f->m->usize = ub_le32(rec + 24);
    f->m->local = ub_le32(rec + 42);
    return 1;
}

// Finds `name` in the central directory. 0, or a negative errno.
static int find_member(int fd, unsigned long fsize, const char *name, struct member *m,
                       char *err, int cap) {
    struct find f = { name, strlen(name), m };
    int rc = walk_cdir(fd, fsize, find_one, &f, err, cap);
    if (rc == 1) return 0;
    if (rc == 0) return failf(err, cap, -ENOENT, "the archive has no %s", name);
    return rc;
}

struct list { uzip_entry_fn fn; void *ctx; };

static int list_one(void *ctx, const unsigned char *rec, const char *name, unsigned nlen) {
    struct list *l = ctx;
    char n[UZIP_NAME_MAX];
    if (nlen >= sizeof n) return 0;   // a name past the bound is skipped, not cut
    memcpy(n, name, nlen);
    n[nlen] = '\0';
    return l->fn(l->ctx, n, ub_le32(rec + 24), ub_le32(rec + 20), ub_le16(rec + 14), ub_le16(rec + 12));
}

int uzip_list(const char *zip_path, uzip_entry_fn fn, void *ctx, char *err, int errcap) {
    int fd = open(zip_path, O_RDONLY);
    if (fd < 0) return failf(err, errcap, -errno, "%s: cannot open", zip_path);
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return failf(err, errcap, -EIO, "%s: cannot stat", zip_path); }
    struct list l = { fn, ctx };
    int rc = walk_cdir(fd, (unsigned long)st.st_size, list_one, &l, err, errcap);
    close(fd);
    return rc < 0 ? rc : 0;
}

struct out {
    int fd;
    uint32_t crc;
    unsigned long done, total;
    uzip_progress progress;
    void *ctx;
    int stopped, write_failed;
};

static int out_write(void *vctx, const uint8_t *data, size_t n) {
    struct out *o = vctx;
    size_t put = 0;
    while (put < n) {
        long w = write(o->fd, data + put, n - put);
        if (w <= 0) { o->write_failed = 1; return -1; }
        put += (size_t)w;
    }
    o->crc = kcrc32_update(o->crc, data, n);
    o->done += n;
    if (o->progress && o->progress(o->ctx, o->done, o->total)) { o->stopped = 1; return -1; }
    return 0;
}

int uzip_extract(const char *zip_path, const char *name, const char *out_path,
                 uzip_progress progress, void *ctx, char *err, int errcap) {
    if (err && errcap > 0) err[0] = 0;
    int fd = open(zip_path, O_RDONLY);
    if (fd < 0) return failf(err, errcap, -ENOENT, "cannot open %s", zip_path);
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return failf(err, errcap, -EIO, "cannot stat %s", zip_path); }
    unsigned long fsize = (unsigned long)st.st_size;

    struct member m;
    int rc = find_member(fd, fsize, name, &m, err, errcap);
    if (rc < 0) { close(fd); return rc; }
    if (m.flags & 1) { close(fd); return failf(err, errcap, -ENOTSUP, "%s is encrypted", name); }
    if (m.method != 0 && m.method != 8) {
        close(fd);
        return failf(err, errcap, -ENOTSUP, "%s uses compression method %u", name, m.method);
    }
    if (m.method == 0 && m.csize != m.usize) {
        close(fd);
        return failf(err, errcap, -EINVAL, "%s is stored but its sizes differ", name);
    }

    unsigned char lh[LOCAL_LEN];
    if ((unsigned long)m.local + LOCAL_LEN > fsize || ub_read_at(fd, m.local, lh, sizeof lh) < 0 ||
        ub_le32(lh) != LOCAL_SIG) {
        close(fd);
        return failf(err, errcap, -EINVAL, "%s's local header is damaged", name);
    }
    unsigned long data = (unsigned long)m.local + LOCAL_LEN + ub_le16(lh + 26) + ub_le16(lh + 28);
    if (data + m.csize > fsize) {
        close(fd);
        return failf(err, errcap, -EINVAL, "%s runs past the end of the archive", name);
    }
    unsigned char *src = malloc(m.csize ? m.csize : 1);
    if (!src) { close(fd); return failf(err, errcap, -ENOMEM, "out of memory for %s", name); }
    if (ub_read_at(fd, data, src, m.csize) < 0) {
        free(src);
        close(fd);
        return failf(err, errcap, -EIO, "could not read %s", name);
    }
    close(fd);

    char part[512];
    if (snprintf(part, sizeof part, "%s.part", out_path) >= (int)sizeof part) {
        free(src);
        return failf(err, errcap, -ENAMETOOLONG, "%s: name too long", out_path);
    }
    struct out o = { .crc = KCRC32_INIT, .total = m.usize, .progress = progress, .ctx = ctx };
    o.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (o.fd < 0) { free(src); return failf(err, errcap, -EACCES, "cannot write %s", part); }

    if (m.method == 0) {
        rc = out_write(&o, src, m.csize) ? -1 : 0;
    } else {
        struct uinflate_scratch *sc = malloc(sizeof *sc);
        size_t outlen = 0;
        rc = sc ? uinflate(src, m.csize, UINFLATE_RAW, sc, out_write, &o, &outlen) : -ENOMEM;
        free(sc);
    }
    free(src);
    if (fsync(o.fd) != 0) o.write_failed = 1;
    close(o.fd);

    if (o.stopped) rc = failf(err, errcap, -EINTR, "stopped");
    else if (o.write_failed) rc = failf(err, errcap, -EIO, "writing %s failed", part);
    else if (rc < 0) rc = failf(err, errcap, -EINVAL, "%s: %s", name, uinflate_error());
    else if (o.done != m.usize || KCRC32_FINAL(o.crc) != m.crc)
        rc = failf(err, errcap, -EINVAL, "%s does not match its CRC-32 and size", name);
    if (rc < 0) { unlink(part); return rc; }
    if (rename(part, out_path) != 0) {
        unlink(part);
        return failf(err, errcap, -EIO, "cannot rename %s into place", part);
    }
    return 0;
}

// --- writing ----------------------------------------------------------

struct uzip_wentry {
    char *name;
    uint32_t crc, csize, usize, offset;
    uint16_t method, dos_date, dos_time;
    uint8_t is_dir;
};

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static int write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n) {
        long w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

// MS-DOS packs a date as years-since-1980 | month | day and a time in
// two-second steps; before 1980 is not representable, so it clamps.
static void dos_stamp(const char *path, uint16_t *date, uint16_t *tm) {
    struct sys_stat st;
    unsigned y = 1980, mo = 1, d = 1, h = 0, mi = 0, s = 0;
    if (sys_stat(path, &st) == 0 && st.modified.year >= 1980) {
        y = st.modified.year; mo = st.modified.month; d = st.modified.day;
        h = st.modified.hour; mi = st.modified.minute; s = st.modified.second;
    }
    *date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    *tm = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
}

int uzip_create_begin(struct uzip_writer *w, const char *out_path, char *err, int errcap) {
    memset(w, 0, sizeof *w);
    w->fd = -1;
    if (snprintf(w->out, sizeof w->out, "%s", out_path) >= (int)sizeof w->out ||
        snprintf(w->part, sizeof w->part, "%s.part", out_path) >= (int)sizeof w->part)
        return failf(err, errcap, -ENAMETOOLONG, "%s: name too long", out_path);
    w->fd = open(w->part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (w->fd < 0) return failf(err, errcap, -EACCES, "cannot write %s", w->part);
    return 0;
}

static struct uzip_wentry *new_entry(struct uzip_writer *w, const char *name, char *err, int errcap) {
    if (strlen(name) >= UZIP_NAME_MAX) { failf(err, errcap, -ENAMETOOLONG, "%s: name too long", name); return 0; }
    if (w->count == w->cap) {
        int cap = w->cap ? w->cap * 2 : 64;
        struct uzip_wentry *g = realloc(w->ent, sizeof *g * (size_t)cap);
        if (!g) { failf(err, errcap, -ENOMEM, "out of memory for the directory"); return 0; }
        w->ent = g;
        w->cap = cap;
    }
    struct uzip_wentry *e = &w->ent[w->count];
    memset(e, 0, sizeof *e);
    e->name = strdup(name);
    if (!e->name) { failf(err, errcap, -ENOMEM, "out of memory for the directory"); return 0; }
    w->count++;
    return e;
}

static int write_local(struct uzip_writer *w, struct uzip_wentry *e, const void *data) {
    unsigned char h[LOCAL_LEN];
    size_t nlen = strlen(e->name);
    put32(h, LOCAL_SIG);
    put16(h + 4, 20);                // version needed: 2.0, deflate
    put16(h + 6, 0x0800);            // bit 11: the name is UTF-8
    put16(h + 8, e->method);
    put16(h + 10, e->dos_time);
    put16(h + 12, e->dos_date);
    put32(h + 14, e->crc);
    put32(h + 18, e->csize);
    put32(h + 22, e->usize);
    put16(h + 26, (unsigned)nlen);
    put16(h + 28, 0);
    e->offset = (uint32_t)w->offset;
    if (write_all(w->fd, h, sizeof h) || write_all(w->fd, e->name, nlen) ||
        (e->csize && write_all(w->fd, data, e->csize)))
        return -1;
    w->offset += sizeof h + nlen + e->csize;
    return 0;
}

int uzip_add_dir(struct uzip_writer *w, const char *name, char *err, int errcap) {
    char n[UZIP_NAME_MAX + 1];
    size_t l = strlen(name);
    if (snprintf(n, sizeof n, "%s%s", name, l && name[l - 1] == '/' ? "" : "/") >= (int)sizeof n)
        return failf(err, errcap, -ENAMETOOLONG, "%s: name too long", name);
    struct uzip_wentry *e = new_entry(w, n, err, errcap);
    if (!e) return -ENOMEM;
    e->is_dir = 1;
    e->dos_date = (uint16_t)((0 << 9) | (1 << 5) | 1);
    if (write_local(w, e, 0)) return failf(err, errcap, -EIO, "writing %s failed", w->part);
    return 0;
}

// The largest member this reads whole -- and holds twice, raw and packed.
#define UZIP_MEMBER_MAX (512ul * 1024 * 1024)

int uzip_add_file(struct uzip_writer *w, const char *name, const char *src_path, int level,
                  uzip_progress progress, void *ctx, char *err, int errcap) {
    struct sys_stat st;
    if (sys_stat(src_path, &st) != 0) return failf(err, errcap, -ENOENT, "cannot read %s", src_path);
    if (st.size >= 0xFFFFFFFFull || st.size > UZIP_MEMBER_MAX)
        return failf(err, errcap, -E2BIG, "%s is too big to compress here", src_path);
    unsigned long n = (unsigned long)st.size;
    unsigned char *raw = malloc(n ? n : 1);
    if (!raw) return failf(err, errcap, -ENOMEM, "out of memory for %s", src_path);
    int fd = open(src_path, O_RDONLY);
    if (fd < 0) { free(raw); return failf(err, errcap, -EACCES, "cannot open %s", src_path); }
    unsigned long got = 0;
    while (got < n) {
        long r = read(fd, raw + got, n - got);
        if (r <= 0) break;
        got += (unsigned long)r;
        if (progress && progress(ctx, got / 2, n)) {   // reading is half the work
            close(fd); free(raw);
            return failf(err, errcap, -EINTR, "stopped");
        }
    }
    close(fd);
    if (got != n) { free(raw); return failf(err, errcap, -EIO, "%s changed while it was read", src_path); }

    struct uzip_wentry *e = new_entry(w, name, err, errcap);
    if (!e) { free(raw); return -ENOMEM; }
    e->usize = (uint32_t)n;
    e->crc = KCRC32_FINAL(kcrc32_update(KCRC32_INIT, raw, n));
    dos_stamp(src_path, &e->dos_date, &e->dos_time);

    // DEFLATED ONLY WHEN IT IS SMALLER: a JPEG or another archive comes
    // out bigger, and every archiver stores those.
    unsigned char *packed = 0;
    size_t plen = 0;
    if (level > 0 && n > 0) {
        size_t cap = udeflate_bound(n);
        packed = malloc(cap);
        if (packed && udeflate_level_into(raw, n, UINFLATE_RAW, (enum udeflate_level)level,
                                          packed, cap, &plen) != 0) {
            free(packed);
            packed = 0;
        }
    }
    int rc;
    if (packed && plen < n) {
        e->method = 8;
        e->csize = (uint32_t)plen;
        rc = write_local(w, e, packed);
    } else {
        e->method = 0;
        e->csize = (uint32_t)n;
        rc = write_local(w, e, raw);
    }
    free(packed);
    free(raw);
    if (rc) return failf(err, errcap, -EIO, "writing %s failed", w->part);
    if (progress) progress(ctx, n, n);
    return 0;
}

int uzip_create_end(struct uzip_writer *w, char *err, int errcap) {
    unsigned long cd_start = w->offset, cd_len = 0;
    for (int i = 0; i < w->count; i++) {
        struct uzip_wentry *e = &w->ent[i];
        unsigned char c[CDIR_LEN];
        size_t nlen = strlen(e->name);
        memset(c, 0, sizeof c);
        put32(c, CDIR_SIG);
        put16(c + 4, 0x031E);        // made by: Unix, spec 3.0
        put16(c + 6, 20);
        put16(c + 8, 0x0800);
        put16(c + 10, e->method);
        put16(c + 12, e->dos_time);
        put16(c + 14, e->dos_date);
        put32(c + 16, e->crc);
        put32(c + 20, e->csize);
        put32(c + 24, e->usize);
        put16(c + 28, (unsigned)nlen);
        // External attributes: Unix mode in the high half, and MS-DOS's
        // directory bit, so both kinds of unzip see a folder as one.
        put32(c + 38, e->is_dir ? ((0040755u << 16) | 0x10) : (0100644u << 16));
        put32(c + 42, e->offset);
        if (write_all(w->fd, c, sizeof c) || write_all(w->fd, e->name, nlen)) {
            uzip_create_abort(w);
            return failf(err, errcap, -EIO, "writing the directory failed");
        }
        cd_len += sizeof c + nlen;
    }
    unsigned char eocd[EOCD_LEN];
    memset(eocd, 0, sizeof eocd);
    put32(eocd, EOCD_SIG);
    put16(eocd + 8, (unsigned)w->count);
    put16(eocd + 10, (unsigned)w->count);
    put32(eocd + 12, (uint32_t)cd_len);
    put32(eocd + 16, (uint32_t)cd_start);
    int bad = w->count > 0xFFFF || write_all(w->fd, eocd, sizeof eocd) || fsync(w->fd) != 0;
    close(w->fd);
    w->fd = -1;
    for (int i = 0; i < w->count; i++) free(w->ent[i].name);
    free(w->ent);
    w->ent = 0;
    if (bad) { unlink(w->part); return failf(err, errcap, -EIO, "finishing %s failed", w->part); }
    if (rename(w->part, w->out) != 0) {
        unlink(w->part);
        return failf(err, errcap, -EIO, "cannot rename %s into place", w->part);
    }
    return 0;
}

void uzip_create_abort(struct uzip_writer *w) {
    if (w->fd >= 0) { close(w->fd); w->fd = -1; unlink(w->part); }
    for (int i = 0; i < w->count; i++) free(w->ent[i].name);
    free(w->ent);
    w->ent = 0;
    w->count = 0;
}
