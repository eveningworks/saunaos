// ISO 9660 and Joliet: a CD image, read-only, mounted from a FILE.
//
// Linux's isofs and Windows' cdfs.sys read the same two formats. Neither
// is mounted from a file directly -- Linux puts a loop device under it,
// Windows a virtual DVD drive -- and SaunaOS has neither: the image is a
// path, read with fs_read_range() (docs/roadmap-details.md, "ISO 9660,
// and an .iso mounted as a virtual CD drive"). Rock Ridge is not read.
//
// THE IMAGE IS UNTRUSTED INPUT. Every record, extent and length is
// checked against the image's size and its own container before it is
// used; a bad one ends that listing or that read, never the kernel.
//
// THE LOCK ORDER IS THE TRAP. The CD at /media/X holds its own mount's
// lock while a backend call runs, and its image usually lives on `/` --
// its PARENT -- whose lock a read of the image takes: child before
// parent, the inversion kernel/fs/CLAUDE.md forbids. So every read of
// the image DROPS this mount's lock (mount_drop_begin(), Linux's loop
// worker thread in this kernel's terms; not a mount_io gap, which
// exclusion would wait for while holding `/`), and nothing of the
// per-mount state is used across one: the image path is copied, and the
// directory cache is looked up again after every read. A mount over the folder holding a mounted image's
// file is refused (mount.c), so an image can never be read through
// itself.
//
// Names: Joliet's when the disc has them (as both OSes prefer), else the
// plain names as written -- upper case, ";1" and a bare trailing dot
// dropped. Lookups ignore ASCII case on both, as Windows' do.
#include "iso9660.h"
#include "fs_ops.h"
#include "fs.h"
#include "mount.h"
#include "heap.h"
#include "string.h"
#include "klog.h"
#include "caltime.h"
#include "kfmt.h"
#include "kutf8.h"
#include "kpath_buf.h"
#include "scheduler.h"

#define ISO_BLOCK 2048u
#define ISO_VD_FIRST 16u
// Linux's isofs gives up after the same hundred: the set must end in a
// terminator, and a hostile image need not have one.
#define ISO_VD_LAST 100u
#define ISO_REC_MIN 34u
// Directories at most this big are kept between calls; a bigger one is
// read for the call that wants it. Tuning, not a cap on the data.
#define ISO_DCACHE_SLOTS 8
#define ISO_DCACHE_MAX_DIR (256u * 1024u)

struct iso_ext { uint32_t lba, len; };

// One entry, as a lookup finds it.
struct iso_node {
    uint64_t pos;            // the image offset of its first record (its ino; a dir's is its extent)
    uint64_t size;           // the sum of its extents
    uint64_t mtime;          // epoch seconds, UTC
    int is_dir;
    int interleaved;         // file unit size set: not read
    int n_ext;
    struct iso_ext *ext;     // &one, or kmalloc'd for a multi-extent file
    struct iso_ext one;
};

struct iso_dcache { uint32_t lba, len, last; uint8_t *buf; };

struct iso_vol {
    char *image;             // the image's absolute path, kmalloc'd
    uint64_t image_size;
    int joliet;
    struct iso_node root;
    uint64_t vol_bytes;
    uint32_t tick;
    struct iso_dcache dc[ISO_DCACHE_SLOTS];
    int warned_interleave;
};

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void node_free(struct iso_node *n) {
    if (n->ext && n->ext != &n->one) kfree(n->ext);
    n->ext = 0;
    n->n_ext = 0;
}

// ---- reading the image -----------------------------------------------

// `len` bytes at `off` of the image: 1, 0 when they are not there, -1
// when the mount went while its lock was dropped (TOUCH NOTHING of `v`).
static int read_image(struct iso_vol *v, uint64_t off, void *buf, uint32_t len) {
    if (off > v->image_size || len > v->image_size - off) return 0;
    // COPIED: an unmount may free `v` while the lock is dropped.
    char *image = kpath_get();
    if (!image) return 0;
    k_strlcpy(image, v->image, FS_PATH_MAX);
    struct mount_drop g;
    int dropped = mount_drop_begin(v, &g);
    uint32_t got = 0;
    if (dropped >= 0 || (fs_lock_held_at(image) && fs_lock_owner_at(image) == scheduler_current_pid()))
        got = fs_read_range(image, off, buf, len);
    kpath_put(image);
    if (dropped > 0 && !mount_drop_end(&g)) return -1;
    return got == len;
}

// A directory's bytes. Cached ones are borrowed (*owned 0) and stay
// valid only until the next read_image(); a big one is the caller's to
// free. NULL with *err -1 when the mount went.
static const uint8_t *dir_get(struct iso_vol *v, uint32_t lba, uint32_t len, int *owned, int *err) {
    *owned = 0;
    *err = 0;
    for (int i = 0; i < ISO_DCACHE_SLOTS; i++)
        if (v->dc[i].buf && v->dc[i].lba == lba && v->dc[i].len == len) {
            v->dc[i].last = ++v->tick;
            return v->dc[i].buf;
        }
    if (len == 0 || (uint64_t)lba * ISO_BLOCK > v->image_size ||
        len > v->image_size - (uint64_t)lba * ISO_BLOCK) return 0;   // checked BEFORE the allocation it sizes
    uint8_t *buf = kmalloc(len);
    if (!buf) return 0;
    int r = read_image(v, (uint64_t)lba * ISO_BLOCK, buf, len);
    if (r <= 0) { kfree(buf); *err = r; return 0; }
    if (len > ISO_DCACHE_MAX_DIR) { *owned = 1; return buf; }
    // The lock was dropped: another call may have cached it meanwhile.
    int slot = 0;
    for (int i = 0; i < ISO_DCACHE_SLOTS; i++) {
        if (v->dc[i].buf && v->dc[i].lba == lba && v->dc[i].len == len) {
            kfree(buf);
            v->dc[i].last = ++v->tick;
            return v->dc[i].buf;
        }
        if (!v->dc[i].buf || v->dc[i].last < v->dc[slot].last) slot = i;
        if (!v->dc[slot].buf) break;
    }
    kfree(v->dc[slot].buf);
    v->dc[slot] = (struct iso_dcache){ lba, len, ++v->tick, buf };
    return buf;
}

// ---- directory records ------------------------------------------------

static uint64_t rec_time(const uint8_t *t) {
    int year = 1900 + t[0], mon = t[1], day = t[2];
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || t[3] > 23 || t[4] > 59 || t[5] > 59) return 0;
    int64_t s = cal_days_from_civil(year, mon, day) * 86400 + t[3] * 3600 + t[4] * 60 + t[5];
    int8_t gmt = (int8_t)t[6];                     // 15-minute units east of UTC
    if (gmt >= -48 && gmt <= 52) s -= (int64_t)gmt * 15 * 60;
    return s > 0 ? (uint64_t)s : 0;
}

// A record's name as SaunaOS shows it, into `out`. 0 when it cannot be
// shown (too long once in UTF-8, or empty).
static int rec_name(const uint8_t *id, int n, int joliet, char *out, int cap) {
    int len;
    if (joliet) {
        uint8_t le[256];
        int units = n / 2;
        for (int i = 0; i < units; i++) {
            le[2 * i] = id[2 * i + 1];
            le[2 * i + 1] = id[2 * i];
            if (!le[2 * i] && !le[2 * i + 1]) le[2 * i] = '_';   // a NUL would end the name early
        }
        len = kutf8_from_utf16le(le, units, out, cap);
        if (len < 0) return 0;
    } else {
        if (n >= cap) return 0;
        for (int i = 0; i < n; i++) out[i] = id[i] ? (char)id[i] : '_';
        len = n;
        out[len] = 0;
    }
    // ";1" (a version number), then a dot left with nothing after it.
    for (int i = len - 1; i > 0; i--) {
        if (out[i] == ';') { len = i; break; }
        if (out[i] < '0' || out[i] > '9') break;
    }
    if (len > 1 && out[len - 1] == '.') len--;
    out[len] = 0;
    // A NAME IS ONE COMPONENT: a disc that names an entry "." or ".." or
    // puts a '/' in one would hand a walker a path outside the entry.
    for (int i = 0; i < len; i++) if (out[i] == '/') out[i] = '_';
    if (!k_strcmp(out, ".") || !k_strcmp(out, "..")) return 0;
    return len > 0;
}

// Walks the records of one directory's bytes.
struct iso_iter {
    const uint8_t *buf;
    uint32_t len, pos;
    uint64_t base;           // the image offset of buf[0]
    int joliet;
};

// The next entry, its extents gathered: 1 with *n and `name` filled (free
// *n with node_free), 0 at the end -- a damaged record ends it too.
static int iter_next(struct iso_iter *it, struct iso_node *n, char *name, int cap) {
    while (it->pos < it->len) {
        uint32_t pos = it->pos, in_sec = pos % ISO_BLOCK;
        uint32_t rl = it->buf[pos];
        if (rl == 0) {                              // the rest of this sector is padding
            it->pos = pos - in_sec + ISO_BLOCK;
            continue;
        }
        const uint8_t *r = it->buf + pos;
        if (rl < ISO_REC_MIN || in_sec + rl > ISO_BLOCK || pos + rl > it->len) return 0;
        uint32_t nl = r[32];
        if (ISO_REC_MIN - 1 + nl > rl) return 0;
        it->pos = pos + rl;
        if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;   // "." and ".."
        if (r[25] & 0x04) continue;                            // an associated file
        if (!rec_name(r + 33, (int)nl, it->joliet, name, cap)) continue;
        *n = (struct iso_node){ 0 };
        n->pos = it->base + pos;
        n->is_dir = (r[25] & 0x02) != 0;
        n->interleaved = r[26] != 0;
        n->mtime = rec_time(r + 18);
        n->one = (struct iso_ext){ le32(r + 2), le32(r + 10) };
        n->ext = &n->one;
        n->n_ext = 1;
        n->size = n->one.len;
        // MULTI-EXTENT: the following records with the same name carry
        // the rest, the last without the flag.
        uint8_t flags = r[25];
        int cap_ext = 1;
        while ((flags & 0x80) && !n->is_dir) {
            uint32_t p = it->pos;
            while (p < it->len && it->buf[p] == 0) p = p - p % ISO_BLOCK + ISO_BLOCK;
            if (p >= it->len) break;
            const uint8_t *q = it->buf + p;
            uint32_t ql = q[0];
            if (ql < ISO_REC_MIN || p % ISO_BLOCK + ql > ISO_BLOCK || p + ql > it->len) break;
            if (n->n_ext == cap_ext) {
                struct iso_ext *e = kmalloc(sizeof *e * (size_t)cap_ext * 2);
                if (!e) { node_free(n); return 0; }
                k_memcpy(e, n->ext, sizeof *e * (size_t)n->n_ext);
                if (n->ext != &n->one) kfree(n->ext);
                n->ext = e;
                cap_ext *= 2;
            }
            n->ext[n->n_ext++] = (struct iso_ext){ le32(q + 2), le32(q + 10) };
            n->size += le32(q + 10);
            flags = q[25];
            it->pos = p + ql;
        }
        if (n->is_dir) {
            n->pos = (uint64_t)n->one.lba * ISO_BLOCK;
            n->size = n->one.len;
        }
        return 1;
    }
    return 0;
}

static int name_eq(const char *a, int alen, const char *b) {
    for (int i = 0; i < alen; i++, b++)
        if (!*b || k_tolower((unsigned char)a[i]) != k_tolower((unsigned char)*b)) return 0;
    return *b == 0;
}

// `path` (root-relative, "/" or "" for the root) to its node: 1, 0 when
// it is not there, -1 when the mount went. Free *out with node_free.
static int lookup(struct iso_vol *v, const char *path, struct iso_node *out) {
    struct iso_node cur = v->root;
    cur.ext = &cur.one;
    char name[FS_NAME_MAX + 1];
    const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) { *out = cur; if (cur.ext == &cur.one) out->ext = &out->one; return 1; }
        const char *end = p;
        while (*end && *end != '/') end++;
        if (!cur.is_dir) { node_free(&cur); return 0; }
        int owned, err;
        const uint8_t *d = dir_get(v, cur.one.lba, cur.one.len, &owned, &err);
        node_free(&cur);
        if (!d) return err < 0 ? -1 : 0;
        struct iso_iter it = { d, cur.one.len, 0, (uint64_t)cur.one.lba * ISO_BLOCK, v->joliet };
        struct iso_node n;
        int found = 0;
        while (iter_next(&it, &n, name, sizeof name)) {
            if (name_eq(p, (int)(end - p), name)) { found = 1; break; }
            node_free(&n);
        }
        if (owned) kfree((void *)d);
        if (!found) return 0;
        cur = n;
        if (n.ext == &n.one) cur.ext = &cur.one;
        p = end;
    }
}

// ---- fs_ops ------------------------------------------------------------

static int iso_size_of(void *st, const char *path, struct iso_node *n) {
    int r = lookup(st, path, n);
    return r > 0;
}

static uint64_t iso_size(void *st, const char *path) {
    struct iso_node n;
    if (!iso_size_of(st, path, &n)) return 0;
    uint64_t s = n.is_dir ? 0 : n.size;
    node_free(&n);
    return s;
}

static uint32_t iso_read_range(void *st, const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct iso_vol *v = st;
    struct iso_node n;
    if (lookup(v, path, &n) <= 0) return 0;
    uint32_t done = 0;
    if (n.is_dir || offset >= n.size) goto out;
    if (n.interleaved) {
        if (!v->warned_interleave) {
            v->warned_interleave = 1;
            klog_printf(KLOG_WARN "iso9660: %s is interleaved, which is not read\n", path);
        }
        goto out;
    }
    if (len > n.size - offset) len = (uint32_t)(n.size - offset);
    uint64_t at = 0;            // the file offset where extent i starts
    for (int i = 0; i < n.n_ext && done < len; i++) {
        uint64_t elen = n.ext[i].len;
        uint64_t want = offset + done;
        if (want >= at + elen) { at += elen; continue; }
        uint64_t in = want - at;
        uint32_t chunk = (uint32_t)(elen - in < len - done ? elen - in : len - done);
        int r = read_image(v, (uint64_t)n.ext[i].lba * ISO_BLOCK + in, (uint8_t *)buf + done, chunk);
        if (r <= 0) break;      // past the image, or the mount went: what came is the answer
        done += chunk;
        at += elen;
    }
out:
    node_free(&n);
    return done;
}

struct iso_step { uint32_t total; };

static void *iso_read_range_begin(void *st, const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct iso_step *h = kmalloc(sizeof *h);
    if (h) h->total = iso_read_range(st, path, offset, buf, len);
    return h;
}

static int iso_read_range_step(void *st, void *handle, uint32_t *out_total) {
    (void)st;
    struct iso_step *h = handle;
    if (!h) return FS_STEP_FAILED;
    if (out_total) *out_total = h->total;
    kfree(h);
    return FS_STEP_DONE;
}

static int iso_is_dir(void *st, const char *path) {
    struct iso_node n;
    if (lookup(st, path, &n) <= 0) return 0;
    int d = n.is_dir;
    node_free(&n);
    return d;
}

static int iso_exists(void *st, const char *path) {
    struct iso_node n;
    if (lookup(st, path, &n) <= 0) return 0;
    node_free(&n);
    return 1;
}

// THE CALLBACK RUNS UNDER THIS MOUNT'S LOCK and nothing here drops it
// between records, so a cached directory stays put for the whole walk.
static void iso_list(void *st, const char *dir, fs_list_cb cb, void *ctx) {
    struct iso_vol *v = st;
    struct iso_node d;
    if (lookup(v, dir, &d) <= 0) return;
    if (!d.is_dir) { node_free(&d); return; }
    int owned, err;
    const uint8_t *buf = dir_get(v, d.one.lba, d.one.len, &owned, &err);
    if (!buf) { node_free(&d); return; }
    char *name = kmalloc(FS_NAME_MAX + 1);
    if (name) {
        struct iso_iter it = { buf, d.one.len, 0, (uint64_t)d.one.lba * ISO_BLOCK, v->joliet };
        struct iso_node n;
        while (iter_next(&it, &n, name, FS_NAME_MAX + 1)) {
            uint64_t s = n.is_dir ? 0 : n.size;
            cb(ctx, name, s > 0xffffffffull ? 0xffffffffu : (uint32_t)s, n.is_dir);
            node_free(&n);
        }
        kfree(name);
    }
    if (owned) kfree((void *)buf);
    node_free(&d);
}

static int iso_stat(void *st, const char *path, struct fs_stat_info *out) {
    struct iso_node n;
    if (lookup(st, path, &n) <= 0) return 0;
    k_memset(out, 0, sizeof *out);
    out->ino = n.pos;
    out->created = out->modified = n.mtime;
    out->mode = n.is_dir ? 0555 : 0444;
    out->nlink = 1;
    out->is_dir = (uint8_t)n.is_dir;
    out->size = n.is_dir ? 0 : n.size;
    node_free(&n);
    return 1;
}

static int iso_disk_usage(void *st, uint64_t *used, uint64_t *total) {
    struct iso_vol *v = st;
    if (used) *used = v->vol_bytes;
    if (total) *total = v->vol_bytes;
    return 1;
}

static void iso_state_free(void *st) {
    struct iso_vol *v = st;
    if (!v) return;
    for (int i = 0; i < ISO_DCACHE_SLOTS; i++) kfree(v->dc[i].buf);
    kfree(v->image);
    kfree(v);
}

// Mounted only through iso9660_mount_image(), read-only (MNT_RDONLY):
// nothing probes a device for it, and no write reaches it.
static int iso_refuse_dev(void *st, const struct block_device *dev) { (void)st; (void)dev; return -1; }
static int iso_init(void *st, const struct block_device *dev, uint64_t size) { (void)st; (void)dev; (void)size; return -1; }
static int iso_refuse_path(void *st, const char *p) { (void)st; (void)p; return 0; }
static int iso_write(void *st, const char *p, const char *d, int a) { (void)st; (void)p; (void)d; (void)a; return 0; }
static int iso_write_range(void *st, const char *p, uint64_t o, const void *b, uint32_t l) { (void)st; (void)p; (void)o; (void)b; (void)l; return 0; }
static void *iso_write_range_begin(void *st, const char *p, uint64_t o, const void *b, uint32_t l) { (void)st; (void)p; (void)o; (void)b; (void)l; return 0; }
static int iso_write_range_step(void *st, void *h) { (void)st; (void)h; return FS_STEP_FAILED; }
static int iso_rename(void *st, const char *a, const char *b) { (void)st; (void)a; (void)b; return 0; }
static int iso_truncate(void *st, const char *p, uint64_t s) { (void)st; (void)p; (void)s; return 0; }

const struct fs_ops iso9660_ops = {
    .name = "iso9660",
    .caps = 0,
    .volume_relative = 0,
    .max_mounts = FS_MOUNTS_ANY,
    .state_free = iso_state_free,
    .probe = iso_refuse_dev,
    .wipe = iso_refuse_dev,
    .format = iso_refuse_dev,
    .init = iso_init,
    .touch = iso_refuse_path,
    .write = iso_write,
    .mkdir = iso_refuse_path,
    .del = iso_refuse_path,
    .size = iso_size,
    .read_range = iso_read_range,
    .write_range = iso_write_range,
    .write_range_begin = iso_write_range_begin,
    .write_range_step = iso_write_range_step,
    .read_range_begin = iso_read_range_begin,
    .read_range_step = iso_read_range_step,
    .rename = iso_rename,
    .truncate = iso_truncate,
    .is_dir = iso_is_dir,
    .exists = iso_exists,
    .list = iso_list,
    .stat = iso_stat,
    .disk_usage = iso_disk_usage,
};

// ---- opening an image ----------------------------------------------------

// The root record of a descriptor, checked against the image.
static int root_of(const uint8_t *vd, uint64_t image_size, struct iso_node *root) {
    const uint8_t *r = vd + 156;
    if (r[0] < ISO_REC_MIN) return 0;
    uint32_t lba = le32(r + 2), len = le32(r + 10);
    if (!len || !(r[25] & 0x02)) return 0;
    if ((uint64_t)lba * ISO_BLOCK > image_size || len > image_size - (uint64_t)lba * ISO_BLOCK) return 0;
    *root = (struct iso_node){ 0 };
    root->pos = (uint64_t)lba * ISO_BLOCK;
    root->is_dir = 1;
    root->mtime = rec_time(r + 18);
    root->one = (struct iso_ext){ lba, len };
    root->ext = &root->one;
    root->n_ext = 1;
    root->size = len;
    return 1;
}

int iso9660_open(struct iso_vol **out, const char *image, char *label, int label_cap, const char **why) {
    *out = 0;
    if (label && label_cap > 0) label[0] = 0;
    uint64_t size = fs_size(image);
    if (fs_is_dir(image) || size < (uint64_t)(ISO_VD_FIRST + 1) * ISO_BLOCK) {
        *why = "not a disc image (too small)";
        return 0;
    }
    struct iso_vol *v = kzalloc(sizeof *v);
    uint8_t *vd = kmalloc(ISO_BLOCK);
    size_t il = k_strlen(image) + 1;
    char *path = kmalloc(il);
    if (!v || !vd || !path) { kfree(v); kfree(vd); kfree(path); *why = "out of memory"; return 0; }
    k_memcpy(path, image, il);
    v->image = path;
    v->image_size = size;
    int have_pvd = 0;
    char plain[33] = "";
    for (uint32_t b = ISO_VD_FIRST; b < ISO_VD_LAST; b++) {
        if (read_image(v, (uint64_t)b * ISO_BLOCK, vd, ISO_BLOCK) <= 0) break;
        if (k_memcmp(vd + 1, "CD001", 5) != 0) break;
        if (vd[0] == 255) break;
        if (vd[0] != 1 && vd[0] != 2) continue;
        if (le16(vd + 128) != ISO_BLOCK) {
            if (vd[0] == 1) { *why = "a logical block size other than 2048 is not read"; break; }
            continue;
        }
        struct iso_node root;
        if (!root_of(vd, size, &root)) continue;
        if (vd[0] == 1 && !have_pvd) {
            have_pvd = 1;
            if (!v->joliet) v->root = root;
            v->vol_bytes = (uint64_t)le32(vd + 80) * ISO_BLOCK;
            int n = 32;
            while (n > 0 && (vd[40 + n - 1] == ' ' || vd[40 + n - 1] == 0)) n--;
            for (int i = 0; i < n; i++) plain[i] = (char)vd[40 + i];
            plain[n] = 0;
        } else if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' &&
                   (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            v->joliet = 1;
            v->root = root;
            if (label && label_cap > 0) {
                int units = 16;
                while (units > 0 && vd[40 + 2 * units - 2] == 0 &&
                       (vd[40 + 2 * units - 1] == ' ' || vd[40 + 2 * units - 1] == 0)) units--;
                uint8_t le[32];
                for (int i = 0; i < units; i++) { le[2 * i] = vd[41 + 2 * i]; le[2 * i + 1] = vd[40 + 2 * i]; }
                if (kutf8_from_utf16le(le, units, label, label_cap) < 0) label[0] = 0;
            }
        }
    }
    kfree(vd);
    if (!have_pvd) {
        if (!**why) *why = "no ISO 9660 volume descriptor";
        iso9660_close(v);
        return 0;
    }
    if (v->vol_bytes > size || !v->vol_bytes) v->vol_bytes = size;
    if (label && label_cap > 0 && !label[0]) k_strlcpy(label, plain, (size_t)label_cap);
    *out = v;
    return 1;
}

void iso9660_close(struct iso_vol *v) { iso_state_free(v); }

int iso9660_mount_image(const char *image, const char *point, const char **why) {
    static const char *dummy;
    if (!why) why = &dummy;
    *why = "";
    struct iso_vol *v;
    if (!iso9660_open(&v, image, 0, 0, why)) return 0;
    if (!mount_add_nodev(&iso9660_ops, v, point, MNT_RDONLY, image, why)) {
        iso9660_close(v);
        return 0;
    }
    klog_printf("iso9660: %s mounted at %s%s\n", image, point, v->joliet ? " (Joliet names)" : "");
    return 1;
}
