// exFAT: allocation, and every call that changes the volume. See
// exfat_internal.h for the split and exfat.c for the format's traps.
//
// THE ORDER A NEW FILE REACHES THE DISK: its clusters (bitmap, then FAT),
// its data, the set's secondary entries, and the File entry LAST -- the
// one with the InUse bit and the checksum. A pull or a crash before that
// leaves entries nothing points at and a bitmap with a few clusters set,
// which another system's check reclaims; it never leaves a file whose
// entry names clusters that were not yet written.
//
// A FILE'S CLUSTERS ARE CONTIGUOUS WHEN THEY CAN BE (NoFatChain, as
// Windows writes them) and grow in place; when the next cluster is
// taken, the run is converted to a FAT chain first -- its FAT entries
// written, the flag cleared -- and then the chain grows.
#include "exfat_internal.h"
#include "heap.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"
#include "errno.h"

// ---- the allocation bitmap ------------------------------------------------

// A cluster number from the disk is checked HERE, where it would index
// the bitmap: a corrupt stick's stream may point anywhere. Outside the
// heap reads as used, and a put there is refused.
static int bm_test(struct exfat_state *x, uint32_t c) {
    if (!exf_cluster_ok(x, c)) return 1;
    uint32_t b = c - 2;
    return (x->bitmap[b / 8] >> (b % 8)) & 1;
}

// One bit, in memory and on the disk (the sector holding it, written
// through).
static int bm_put(struct exfat_state *x, uint32_t c, int used) {
    if (!exf_cluster_ok(x, c)) return 0;
    uint32_t b = c - 2;
    if (used) x->bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
    else x->bitmap[b / 8] &= (uint8_t)~(1u << (b % 8));
    if (used) x->free_clusters--; else x->free_clusters++;
    uint64_t off = (uint64_t)(b / 8) & ~(uint64_t)(x->bps - 1);
    uint32_t clus = exf_stream_cluster(x, &x->bitmap_s, off);
    if (!clus) return 0;
    uint64_t sector = exf_cluster_sector(x, clus) + ((off % x->cluster_bytes) >> x->bps_shift);
    uint32_t n = x->bitmap_bytes - (uint32_t)off < x->bps ? x->bitmap_bytes - (uint32_t)off : x->bps;
    if (n < x->bps) {   // the bitmap's last sector: keep what follows it as it is
        if (!exf_read_sector(x, sector, x->sec)) return 0;
        x->sec_valid = 0;
        k_memcpy(x->sec, x->bitmap + off, n);
        return exf_write_sector(x, sector, x->sec);
    }
    return exf_write_sector(x, sector, x->bitmap + off);
}

// `count` free clusters in a row, the first at or after the hint; 0 if
// there is no such run.
static uint32_t find_run(struct exfat_state *x, uint32_t count) {
    uint32_t total = x->cluster_count;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? 0 : x->alloc_hint, run = 0;
        for (uint32_t b = start; b < total; b++) {
            if (x->bitmap[b / 8] & (1u << (b % 8))) { run = 0; continue; }
            if (++run == count) return b - count + 1 + 2;
        }
    }
    return 0;
}

static int zero_cluster(struct exfat_state *x, uint32_t c) {
    k_memset(x->sec, 0, x->bps);
    x->sec_valid = 0;
    uint64_t first = exf_cluster_sector(x, c);
    for (uint32_t i = 0; i < (1u << x->spc_shift); i++)
        if (!exf_write_sector(x, first + i, x->sec)) return 0;
    return 1;
}

// ---- growing and shrinking a stream -----------------------------------------

static uint32_t chain_last(struct exfat_state *x, struct exf_stream *s, uint32_t have) {
    return exf_stream_cluster(x, s, (uint64_t)(have - 1) * x->cluster_bytes);
}

// The FAT entries a contiguous run never had: each cluster to the next,
// the last to the end of chain. Then it is an ordinary chain.
static int to_chain(struct exfat_state *x, struct exf_stream *s, uint32_t have) {
    if (!exf_cluster_ok(x, s->first) || !exf_cluster_ok(x, s->first + have - 1)) return 0;
    for (uint32_t i = 0; i < have; i++)
        if (!exf_fat_set(x, s->first + i, i + 1 < have ? s->first + i + 1 : EXFAT_EOC)) return 0;
    s->nofat = 0;
    s->cur_clus = s->cur_idx = 0;
    return 1;
}

// At least `want` clusters, new ones zeroed when `zero`. The stream's
// fields change; the caller writes them to its entry set.
static int grow(struct exfat_state *x, struct exf_stream *s, uint32_t want, int zero) {
    uint32_t have = s->first ? exf_stream_clusters(x, s) : 0;
    if (want <= have) return 1;
    if (want - have > x->free_clusters) return 0;
    if (!have) {
        uint32_t run = find_run(x, want);
        if (run) {   // all of it in a row: no FAT entries at all
            for (uint32_t i = 0; i < want; i++) {
                if (!bm_put(x, run + i, 1)) return 0;
                if (zero && !zero_cluster(x, run + i)) return 0;
            }
            s->first = run;
            s->nofat = 1;
            x->alloc_hint = run + want - 2;
            s->cur_clus = s->cur_idx = 0;
            return 1;
        }
    } else if (s->nofat) {
        uint32_t next = s->first + have, need = want - have, i = 0;
        for (; i < need; i++)
            if (!exf_cluster_ok(x, next + i) || bm_test(x, next + i)) break;
        if (i == need) {   // the run continues in place
            for (i = 0; i < need; i++) {
                if (!bm_put(x, next + i, 1)) return 0;
                if (zero && !zero_cluster(x, next + i)) return 0;
            }
            return 1;
        }
        if (!to_chain(x, s, have)) return 0;
    }
    uint32_t last = have ? chain_last(x, s, have) : 0;
    for (uint32_t i = have; i < want; i++) {
        uint32_t c = find_run(x, 1);
        if (!c || !bm_put(x, c, 1) || !exf_fat_set(x, c, EXFAT_EOC)) return 0;
        if (zero && !zero_cluster(x, c)) return 0;
        if (last) { if (!exf_fat_set(x, last, c)) return 0; }
        else { s->first = c; s->nofat = 0; }
        last = c;
        x->alloc_hint = c - 2;
    }
    s->cur_clus = s->cur_idx = 0;
    return 1;
}

// Keeps the first `keep` clusters and frees the rest.
static int shrink(struct exfat_state *x, struct exf_stream *s, uint32_t keep) {
    if (!s->first) return 1;
    uint32_t have = exf_stream_clusters(x, s);
    // No length: a chain is walked to its end (never called on the root);
    // a contiguous run with no length holds nothing to free.
    if (!s->length) have = s->nofat ? 0 : 0xFFFFFFFFu;
    if (keep >= have) return 1;
    if (s->nofat) {
        // The whole run inside the heap before any of it is freed: a
        // length a corrupt entry inflated must not free half and fail.
        if (!exf_cluster_ok(x, s->first) || !exf_cluster_ok(x, s->first + have - 1)) return 0;
        for (uint32_t i = keep; i < have; i++) if (!bm_put(x, s->first + i, 0)) return 0;
    } else {
        uint32_t c = keep ? chain_last(x, s, keep) : s->first;
        uint32_t next = c;
        if (keep) {
            if (!exf_fat_get(x, c, &next) || !exf_fat_set(x, c, EXFAT_EOC)) return 0;
        }
        for (uint32_t n = 0; exf_cluster_ok(x, next) && n < x->cluster_count; n++) {
            uint32_t after;
            if (!exf_fat_get(x, next, &after)) return 0;
            if (!bm_put(x, next, 0) || !exf_fat_set(x, next, 0)) return 0;
            next = after;
        }
    }
    if (!keep) { s->first = 0; s->nofat = 0; }
    s->cur_clus = s->cur_idx = 0;
    return 1;
}

// ---- entry sets -------------------------------------------------------------

// A node's File and Stream fields written back into its set, the
// checksum recomputed. The root has no set.
static int store(struct exfat_state *x, struct exf_node *n) {
    if (!n->loc.count) return 1;
    uint8_t *set = x->uset;
    for (int i = 0; i < n->loc.count; i++)
        if (!exf_dir_entry(x, &n->loc.dir, n->loc.index + (uint32_t)i, set + i * 32)) return 0;
    if (set[0] != ET_FILE || set[32] != ET_STREAM) return 0;
    exf_put16(set + 4, n->attr);
    exf_put32(set + 12, n->mod);
    exf_put32(set + 16, n->mod);   // accessed: written as modified
    set[21] = n->mod_10ms;
    set[23] = n->mod_utc;
    set[24] = n->mod_utc;
    uint8_t *s = set + 32;
    s[1] = (uint8_t)(SF_ALLOC | (n->s.nofat ? SF_NOFATCHAIN : 0));
    exf_put64(s + 8, n->s.valid);
    exf_put32(s + 20, n->s.first);
    exf_put64(s + 24, n->s.length);
    exf_put16(set + 2, exf_set_checksum(set, n->loc.count));
    return exf_dir_put_set(x, &n->loc.dir, n->loc.index, set, n->loc.count);
}

static void touch_times(struct exf_node *n) { n->mod = exf_now(&n->mod_10ms, &n->mod_utc); }

// ---- directories -------------------------------------------------------------

// A run of `count` free entries in `dir`, growing it by a zeroed cluster
// when it has none. Returns the first index, or -1.
static int64_t find_slots(struct exfat_state *x, struct exf_node *dir, int count) {
    uint8_t e[32];
    uint32_t run = 0, i = 0;
    for (;; i++) {
        if (!exf_dir_entry(x, &dir->s, i, e)) break;   // past the end of its clusters
        if (e[0] & ET_INUSE) { run = 0; continue; }
        if (++run == (uint32_t)count) return (int64_t)(i - (uint32_t)count + 1);
    }
    // Grow: one more cluster, zeroed (all END entries).
    uint32_t have = dir->s.first ? (dir->s.length ? exf_stream_clusters(x, &dir->s) : i * 32 / x->cluster_bytes) : 0;
    struct exf_stream s = dir->s;
    if (!dir->loc.count) {   // the root: a chain with no length to keep
        s.length = (uint64_t)have * x->cluster_bytes;
        if (!grow(x, &s, have + 1, 1)) return -1;
        dir->s.first = s.first;
        dir->s.cur_clus = dir->s.cur_idx = 0;
    } else {
        if (!grow(x, &s, have + 1, 1)) return -1;
        s.length = s.valid = (uint64_t)(have + 1) * x->cluster_bytes;
        dir->s = s;
        touch_times(dir);
        if (!store(x, dir)) return -1;
    }
    return find_slots(x, dir, count);
}

// exFAT forbids these in a name (7.7.3), and an empty one.
static int name_ok(const uint16_t *n, int len) {
    if (len <= 0 || len > NAME_MAX16) return 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = n[i];
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' ||
            c == '?' || c == '\\' || c == '|')
            return 0;
    }
    return 1;
}

// Builds a set in x->nset: File, Stream, names. Its entry count.
static int build_set(struct exfat_state *x, const uint16_t *name, int len, uint16_t attr,
                     const struct exf_stream *s, uint32_t crt, uint8_t crt_10ms, uint8_t crt_utc) {
    int names = (len + NAME_PER_ENTRY - 1) / NAME_PER_ENTRY;
    int count = 2 + names;
    uint8_t *set = x->nset;
    k_memset(set, 0, (size_t)count * 32);
    set[0] = ET_FILE;
    set[1] = (uint8_t)(count - 1);
    exf_put16(set + 4, attr);
    uint8_t ms, utc;
    uint32_t now = exf_now(&ms, &utc);
    exf_put32(set + 8, crt ? crt : now);
    exf_put32(set + 12, now);
    exf_put32(set + 16, now);
    set[20] = crt ? crt_10ms : ms;
    set[21] = ms;
    set[22] = crt ? crt_utc : utc;
    set[23] = utc;
    set[24] = utc;
    uint8_t *st = set + 32;
    st[0] = ET_STREAM;
    st[1] = (uint8_t)(SF_ALLOC | (s->nofat ? SF_NOFATCHAIN : 0));
    st[3] = (uint8_t)len;
    exf_put16(st + 4, exf_name_hash(x, name, len));
    exf_put64(st + 8, s->valid);
    exf_put32(st + 20, s->first);
    exf_put64(st + 24, s->length);
    for (int i = 0; i < len; i++) {
        uint8_t *e = set + (2 + i / NAME_PER_ENTRY) * 32;
        e[0] = ET_NAME;
        exf_put16(e + 2 + (i % NAME_PER_ENTRY) * 2, name[i]);
    }
    for (int k = 0; k < names; k++) set[(2 + k) * 32] = ET_NAME;
    exf_put16(set + 2, exf_set_checksum(set, count));
    return count;
}

// Writes x->nset into `dir`, File entry last. The new set's location.
static int place_set(struct exfat_state *x, struct exf_node *dir, int count, struct exf_loc *loc) {
    int64_t at = find_slots(x, dir, count);
    if (at < 0) return 0;
    if (!exf_dir_put_set(x, &dir->s, (uint32_t)at, x->nset, count)) return 0;
    loc->dir = dir->s;
    loc->dir.cur_clus = loc->dir.cur_idx = 0;
    loc->index = (uint32_t)at;
    loc->count = count;
    return 1;
}

// Clears every InUse bit of a set: the entries are free, its clusters are
// the caller's to free first.
static int unlink_set(struct exfat_state *x, struct exf_loc *loc) {
    uint8_t e[32];
    for (int i = 0; i < loc->count; i++) {
        if (!exf_dir_entry(x, &loc->dir, loc->index + (uint32_t)i, e)) return 0;
        e[0] &= (uint8_t)~ET_INUSE;
        if (!exf_dir_put(x, &loc->dir, loc->index + (uint32_t)i, e)) return 0;
    }
    return 1;
}

// A new, empty file or directory at `path`. 1 made, 0 failed, -EEXIST.
static int create(struct exfat_state *x, const char *path, int dir, struct exf_node *out) {
    struct exf_node parent, existing;
    const char *leaf;
    if (!x->mounted || !exf_parent(x, path, &parent, &leaf)) return 0;
    if (exf_find(x, &parent.s, leaf, &existing)) return -EEXIST;
    uint16_t *name = x->name16;
    int len = exf_utf8_to16(leaf, name, NAME_MAX16);
    if (!name_ok(name, len)) return 0;
    struct exf_stream s = { 0 };
    if (dir) {   // a directory starts with one zeroed cluster: no "." or ".." in exFAT
        if (!grow(x, &s, 1, 1)) return 0;
        s.length = s.valid = x->cluster_bytes;
    }
    int count = build_set(x, name, len, dir ? ATTR_DIR : ATTR_ARCHIVE, &s, 0, 0, 0);
    struct exf_loc loc;
    if (!place_set(x, &parent, count, &loc)) {
        if (dir) shrink(x, &s, 0);
        return 0;
    }
    if (out) {
        exf_node_from_set(out, x->nset);
        out->loc = loc;
    }
    return 1;
}

// ---- the fs_ops -------------------------------------------------------------

int exfat_touch(void *st, const char *path) {
    struct exfat_state *x = st;
    struct exf_node n;
    if (!x->mounted) return 0;
    if (exf_lookup(x, path, &n)) {
        touch_times(&n);
        return store(x, &n);
    }
    return create(x, path, 0, 0) == 1;
}

int exfat_create_excl(void *st, const char *path) {
    int r = create(st, path, 0, 0);
    return r == -EEXIST ? -EEXIST : r == 1;
}

int exfat_mkdir(void *st, const char *path) { return create(st, path, 1, 0) == 1; }

// Zeros from the valid length to `to`: what a write past the end, or a
// write into preallocated space, must not leave as old disk contents.
static int zero_gap(struct exfat_state *x, struct exf_node *n, uint64_t to) {
    k_memset(x->sec, 0, x->bps);
    x->sec_valid = 0;
    for (uint64_t at = n->s.valid; at < to;) {
        uint32_t c = exf_stream_cluster(x, &n->s, at);
        if (!c) return 0;
        uint64_t sector = exf_cluster_sector(x, c) + ((at % x->cluster_bytes) >> x->bps_shift);
        uint32_t in = (uint32_t)(at & (x->bps - 1));
        if (in) {   // keep the sector's valid head
            uint8_t *keep = kmalloc(x->bps);
            if (!keep || !exf_read_sector(x, sector, keep)) { kfree(keep); return 0; }
            k_memset(keep + in, 0, x->bps - in);
            int ok = exf_write_sector(x, sector, keep);
            kfree(keep);
            if (!ok) return 0;
            k_memset(x->sec, 0, x->bps);
        } else if (!exf_write_sector(x, sector, x->sec)) {
            return 0;
        }
        at += x->bps - in;
    }
    n->s.valid = to;
    return 1;
}

int exfat_write_range(void *st, const char *path, uint64_t offset, const void *buf, uint32_t len) {
    struct exfat_state *x = st;
    struct exf_node n;
    if (!x->mounted) return 0;
    if (!exf_lookup(x, path, &n)) {
        if (create(x, path, 0, &n) != 1) return 0;
    }
    if (n.is_dir) return 0;
    uint64_t end = offset + len;
    if (end < offset) return 0;
    uint32_t need = (uint32_t)((end + x->cluster_bytes - 1) / x->cluster_bytes);
    if (!grow(x, &n.s, need, 0)) return 0;
    if (end > n.s.length) n.s.length = end;
    if (offset > n.s.valid && !zero_gap(x, &n, offset)) return 0;

    const uint8_t *src = buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t at = offset + done;
        uint32_t c = exf_stream_cluster(x, &n.s, at);
        if (!c) return 0;
        uint64_t sector = exf_cluster_sector(x, c) + ((at % x->cluster_bytes) >> x->bps_shift);
        uint32_t in = (uint32_t)(at & (x->bps - 1));
        uint32_t take = x->bps - in;
        if (take > len - done) take = len - done;
        if (in == 0 && take == x->bps) {
            // Whole sectors from the caller's buffer, to the cluster's end.
            uint32_t left = (uint32_t)((x->cluster_bytes - at % x->cluster_bytes) >> x->bps_shift);
            uint32_t k = (len - done) >> x->bps_shift;
            uint32_t xfer = (uint32_t)blkdev_max_sectors_per_xfer(x->dev) / x->dev_per_sec;
            if (k > left) k = left;
            if (k > xfer) k = xfer;
            if (!blkdev_write_sectors(x->dev, sector * x->dev_per_sec, (int)(k * x->dev_per_sec),
                                      src + done))
                return 0;
            if (x->sec_valid && x->sec_lba >= sector && x->sec_lba < sector + k) x->sec_valid = 0;
            done += k << x->bps_shift;
            continue;
        }
        // A partial sector: what is past the valid length is zeros, not
        // whatever the disk held.
        uint8_t *s = x->sec;
        x->sec_valid = 0;
        if ((at & ~(uint64_t)(x->bps - 1)) < n.s.valid) {
            if (!exf_read_sector(x, sector, s)) return 0;
        } else {
            k_memset(s, 0, x->bps);
        }
        k_memcpy(s + in, src + done, take);
        if (!exf_write_sector(x, sector, s)) return 0;
        done += take;
    }
    if (end > n.s.valid) n.s.valid = end;
    touch_times(&n);
    return store(x, &n);
}

int exfat_write(void *st, const char *path, const char *data, int append) {
    struct exfat_state *x = st;
    struct exf_node n;
    uint32_t len = (uint32_t)k_strlen(data);
    uint64_t at = 0;
    if (append && exf_lookup(x, path, &n) && !n.is_dir) at = n.s.length;
    if (!append && exf_lookup(x, path, &n) && !exfat_truncate(st, path, 0)) return 0;
    if (!len) return exfat_touch(st, path);
    return exfat_write_range(st, path, at, data, len);
}

int exfat_truncate(void *st, const char *path, uint64_t size) {
    struct exfat_state *x = st;
    struct exf_node n;
    if (!x->mounted || !exf_lookup(x, path, &n) || n.is_dir) return 0;
    uint32_t keep = (uint32_t)((size + x->cluster_bytes - 1) / x->cluster_bytes);
    if (size < n.s.length) {
        if (!shrink(x, &n.s, keep)) return 0;
        if (n.s.valid > size) n.s.valid = size;
    } else if (size > n.s.length) {
        // Allocated but not written: past the valid length it reads as
        // zeros without a byte of it touching the disk.
        if (!grow(x, &n.s, keep, 0)) return 0;
    }
    n.s.length = size;
    touch_times(&n);
    return store(x, &n);
}

static int dir_empty(struct exfat_state *x, struct exf_node *d) {
    uint8_t e[32];
    for (uint32_t i = 0; exf_dir_entry(x, &d->s, i, e) && e[0] != ET_END; i++)
        if (e[0] & ET_INUSE) return 0;
    return 1;
}

int exfat_del(void *st, const char *path) {
    struct exfat_state *x = st;
    struct exf_node n;
    if (!x->mounted || !exf_lookup(x, path, &n) || !n.loc.count) return 0;
    if (n.is_dir && !dir_empty(x, &n)) return 0;
    // The entries first: a pull between the two leaves clusters marked
    // used that nothing names (space, not a file pointing at freed data).
    if (!unlink_set(x, &n.loc)) return 0;
    return shrink(x, &n.s, 0);
}

// Whether `newpath` lies inside directory `d` -- which would cut it off
// from the root. By IDENTITY, each ancestor resolved and its entry's
// place compared: names match without regard to case, so "/Photos"
// moved to "/photos/old" is into itself though no byte prefix says so.
static int into_itself(struct exfat_state *x, const struct exf_node *d, const char *newpath) {
    size_t len = k_strlen(newpath);
    char *p = kmalloc(len + 1);
    if (!p) return 1;            // cannot tell: refuse
    int inside = 0;
    for (size_t i = len; i > 0 && !inside; i--) {
        if (newpath[i] != '/') continue;
        k_memcpy(p, newpath, i);
        p[i] = 0;
        struct exf_node a;
        inside = exf_lookup(x, p, &a) && a.loc.count && a.loc.index == d->loc.index &&
                 a.loc.dir.first == d->loc.dir.first;
    }
    kfree(p);
    return inside;
}

int exfat_rename(void *st, const char *oldpath, const char *newpath) {
    struct exfat_state *x = st;
    struct exf_node n, parent, existing;
    const char *leaf;
    if (!x->mounted || !exf_lookup(x, oldpath, &n) || !n.loc.count) return 0;
    if (n.is_dir && into_itself(x, &n, newpath)) return 0;
    if (!exf_parent(x, newpath, &parent, &leaf)) return 0;
    if (exf_find(x, &parent.s, leaf, &existing)) {
        // Only a change of case to the SAME entry is a rename onto itself.
        if (existing.loc.index != n.loc.index || existing.loc.dir.first != n.loc.dir.first) return 0;
    }
    uint16_t *name = x->name16;
    int len = exf_utf8_to16(leaf, name, NAME_MAX16);
    if (!name_ok(name, len)) return 0;
    int count = build_set(x, name, len, n.attr, &n.s, n.crt, n.crt_10ms, n.crt_utc);
    // Its modified time is the file's, not now: a rename is not a write.
    exf_put32(x->nset + 12, n.mod);
    x->nset[21] = n.mod_10ms;
    x->nset[23] = n.mod_utc;
    exf_put16(x->nset + 2, exf_set_checksum(x->nset, count));
    struct exf_loc old = n.loc, fresh;
    if (!place_set(x, &parent, count, &fresh)) return 0;
    return unlink_set(x, &old);
}
