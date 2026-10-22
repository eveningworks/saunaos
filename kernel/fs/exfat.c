// exFAT: mounting, lookup and reading. See exfat_internal.h for the split.
//
// exFAT is what a stick bigger than 32 GiB ships with, and what Windows
// and macOS both write; FAT32 cannot hold a file of 4 GiB or a volume
// past 2 TiB. The specification has been public since 2019.
//
// THE THREE THINGS THAT ARE NOT FAT32:
//  1. Free space is an ALLOCATION BITMAP, a file in the root, not the
//     FAT. The FAT only chains clusters -- and a file whose clusters are
//     contiguous says so (NoFatChain) and has NO FAT entries at all, so
//     a reader that follows the FAT for it reads garbage.
//  2. A file is an ENTRY SET -- File, Stream Extension, one Name entry
//     per 15 UTF-16 code units -- with a checksum over the whole set.
//     Writing any one entry without the checksum makes Windows discard
//     the file.
//  3. Names compare through the volume's own UP-CASE TABLE, and the
//     Stream entry carries a hash of the up-cased name. Both have to be
//     right or another OS cannot find what this one wrote.
//
// Names are UTF-16 on disk and UTF-8 here, converted at the edge; a name
// that is not valid UTF-16 (an unpaired surrogate) reads as U+FFFD.
// ValidDataLength is honoured: bytes past it read as zeros, which is how
// a large file gets preallocated without writing every byte.
#include "exfat_internal.h"
#include "fs.h"
#include "heap.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"
#include "caltime.h"
#include "ktime.h"
#include "errno.h"

// ---- little-endian fields -------------------------------------------------

uint32_t exf_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint64_t exf_le64(const uint8_t *p) { return (uint64_t)exf_le32(p) | (uint64_t)exf_le32(p + 4) << 32; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
void exf_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void exf_put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void exf_put64(uint8_t *p, uint64_t v) { exf_put32(p, (uint32_t)v); exf_put32(p + 4, (uint32_t)(v >> 32)); }

// ---- sectors, clusters, the FAT -------------------------------------------

int exf_read_sector(struct exfat_state *x, uint64_t sector, void *buf) {
    return blkdev_read_sectors(x->dev, sector * x->dev_per_sec, (int)x->dev_per_sec, buf);
}

int exf_write_sector(struct exfat_state *x, uint64_t sector, const void *buf) {
    if (x->sec_valid && x->sec_lba == sector && buf != x->sec) k_memcpy(x->sec, buf, x->bps);
    if (x->fsec_valid && x->fsec_lba == sector && buf != x->fsec) k_memcpy(x->fsec, buf, x->bps);
    return blkdev_write_sectors(x->dev, sector * x->dev_per_sec, (int)x->dev_per_sec, buf);
}

static uint8_t *cached(struct exfat_state *x, uint64_t sector) {
    if (x->sec_valid && x->sec_lba == sector) return x->sec;
    x->sec_valid = 0;
    if (!exf_read_sector(x, sector, x->sec)) return 0;
    x->sec_lba = sector;
    x->sec_valid = 1;
    return x->sec;
}

int exf_cluster_ok(struct exfat_state *x, uint32_t clus) {
    return clus >= 2 && clus - 2 < x->cluster_count;
}

uint64_t exf_cluster_sector(struct exfat_state *x, uint32_t clus) {
    return x->heap_offset + ((uint64_t)(clus - 2) << x->spc_shift);
}

int exf_fat_get(struct exfat_state *x, uint32_t clus, uint32_t *out) {
    uint64_t byte = (uint64_t)clus * 4;
    uint64_t sector = x->fat_offset + (byte >> x->bps_shift);
    if (!(x->fsec_valid && x->fsec_lba == sector)) {
        x->fsec_valid = 0;
        if (!exf_read_sector(x, sector, x->fsec)) return 0;
        x->fsec_lba = sector;
        x->fsec_valid = 1;
    }
    *out = exf_le32(x->fsec + (byte & (x->bps - 1)));
    return 1;
}

// Written through: a FAT entry is on the disk when this returns.
int exf_fat_set(struct exfat_state *x, uint32_t clus, uint32_t val) {
    uint32_t old;
    if (!exf_fat_get(x, clus, &old)) return 0;
    exf_put32(x->fsec + (((uint64_t)clus * 4) & (x->bps - 1)), val);
    return exf_write_sector(x, x->fsec_lba, x->fsec);
}

int exf_fat_flush(struct exfat_state *x) { (void)x; return 1; }

uint32_t exf_stream_clusters(struct exfat_state *x, const struct exf_stream *s) {
    return (uint32_t)((s->length + x->cluster_bytes - 1) / x->cluster_bytes);
}

uint32_t exf_stream_cluster(struct exfat_state *x, struct exf_stream *s, uint64_t off) {
    if (!s->first) return 0;
    uint32_t idx = (uint32_t)(off / x->cluster_bytes);
    if (s->nofat) {
        if (s->length && idx >= exf_stream_clusters(x, s)) return 0;
        uint32_t c = s->first + idx;
        return exf_cluster_ok(x, c) ? c : 0;
    }
    if (!s->cur_clus || s->cur_idx > idx) { s->cur_idx = 0; s->cur_clus = s->first; }
    while (s->cur_idx < idx) {
        uint32_t next;
        if (!exf_cluster_ok(x, s->cur_clus) || !exf_fat_get(x, s->cur_clus, &next)) return 0;
        if (!exf_cluster_ok(x, next)) return 0;   // end of chain, or a bad pointer
        s->cur_clus = next;
        s->cur_idx++;
    }
    return exf_cluster_ok(x, s->cur_clus) ? s->cur_clus : 0;
}

// ---- directory entries ------------------------------------------------------

static int entry_sector(struct exfat_state *x, struct exf_stream *dir, uint32_t index,
                        uint64_t *sector, uint32_t *off) {
    uint64_t byte = (uint64_t)index * 32;
    if (dir->length && byte >= dir->length) return 0;
    uint32_t c = exf_stream_cluster(x, dir, byte);
    if (!c) return 0;
    uint64_t in = byte % x->cluster_bytes;
    *sector = exf_cluster_sector(x, c) + (in >> x->bps_shift);
    *off = (uint32_t)(in & (x->bps - 1));
    return 1;
}

int exf_dir_entry(struct exfat_state *x, struct exf_stream *dir, uint32_t index, uint8_t *e32) {
    uint64_t sector;
    uint32_t off;
    if (!entry_sector(x, dir, index, &sector, &off)) return 0;
    uint8_t *s = cached(x, sector);
    if (!s) return 0;
    k_memcpy(e32, s + off, 32);
    return 1;
}

int exf_dir_put(struct exfat_state *x, struct exf_stream *dir, uint32_t index, const uint8_t *e32) {
    uint64_t sector;
    uint32_t off;
    if (!entry_sector(x, dir, index, &sector, &off)) return 0;
    uint8_t *s = cached(x, sector);
    if (!s) return 0;
    k_memcpy(s + off, e32, 32);
    return exf_write_sector(x, sector, s);
}

// 6.3.3 and 7.4.4: the same rotate-and-add, 16 bits wide over an entry
// set (skipping the checksum's own two bytes), 32 bits wide over the boot
// region and the up-case table.
uint16_t exf_set_checksum(const uint8_t *set, int count) {
    uint16_t sum = 0;
    for (int i = 0; i < count * 32; i++) {
        if (i == 2 || i == 3) continue;
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + set[i]);
    }
    return sum;
}

uint32_t exf_boot_checksum(const uint8_t *region, uint32_t bps) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 11 * bps; i++) {
        if (i == 106 || i == 107 || i == 112) continue;   // VolumeFlags, PercentInUse
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + region[i];
    }
    return sum;
}

uint32_t exf_table_checksum(const uint8_t *data, uint64_t len) {
    uint32_t sum = 0;
    for (uint64_t i = 0; i < len; i++) sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + data[i];
    return sum;
}

static uint16_t up(struct exfat_state *x, uint16_t c) { return x->upcase ? x->upcase[c] : c; }

// 7.6.4: over the up-cased name, each code unit low byte first.
uint16_t exf_name_hash(struct exfat_state *x, const uint16_t *name, int len) {
    uint16_t h = 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = up(x, name[i]);
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
    }
    return h;
}

// ---- names: UTF-8 here, UTF-16 on the disk ---------------------------------

// A component of `in` (up to '/' or NUL) into UTF-16. -1 for invalid
// UTF-8 or a name too long; else the code-unit count.
int exf_utf8_to16(const char *in, uint16_t *out, int cap) {
    int n = 0;
    const uint8_t *p = (const uint8_t *)in;
    while (*p && *p != '/') {
        uint32_t cp;
        int more;
        if (*p < 0x80)          { cp = *p; more = 0; }
        else if ((*p >> 5) == 6) { cp = *p & 0x1F; more = 1; }
        else if ((*p >> 4) == 14) { cp = *p & 0x0F; more = 2; }
        else if ((*p >> 3) == 30) { cp = *p & 0x07; more = 3; }
        else return -1;
        p++;
        for (int i = 0; i < more; i++, p++) {
            if ((*p & 0xC0) != 0x80) return -1;
            cp = cp << 6 | (*p & 0x3F);
        }
        if (cp >= 0x10000) {
            if (n + 2 > cap) return -1;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            if (n + 1 > cap) return -1;
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

static int put_utf8(char *out, int at, int cap, uint32_t cp) {
    int need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (at + need >= cap) return -1;
    if (need == 1) out[at] = (char)cp;
    else if (need == 2) { out[at] = (char)(0xC0 | cp >> 6); out[at + 1] = (char)(0x80 | (cp & 0x3F)); }
    else if (need == 3) {
        out[at] = (char)(0xE0 | cp >> 12);
        out[at + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[at + 2] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[at] = (char)(0xF0 | cp >> 18);
        out[at + 1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[at + 2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[at + 3] = (char)(0x80 | (cp & 0x3F));
    }
    return at + need;
}

static void utf16_to8(const uint16_t *in, int len, char *out, int cap) {
    int at = 0;
    for (int i = 0; i < len; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;   // an unpaired surrogate
        }
        int next = put_utf8(out, at, cap, cp);
        if (next < 0) break;
        at = next;
    }
    out[at] = 0;
}

// ---- entry sets ---------------------------------------------------------

void exf_node_from_set(struct exf_node *n, const uint8_t *set) {
    const uint8_t *f = set, *s = set + 32;
    k_memset(n, 0, sizeof *n);
    n->attr = le16(f + 4);
    n->is_dir = (n->attr & ATTR_DIR) != 0;
    n->crt = exf_le32(f + 8);
    n->mod = exf_le32(f + 12);
    n->crt_10ms = f[20];
    n->mod_10ms = f[21];
    n->crt_utc = f[22];
    n->mod_utc = f[23];
    n->s.nofat = (s[1] & SF_NOFATCHAIN) != 0;
    n->s.valid = exf_le64(s + 8);
    n->s.first = exf_le32(s + 20);
    n->s.length = exf_le64(s + 24);
    if (n->s.valid > n->s.length) n->s.valid = n->s.length;
}

// Reads the set whose File entry is at `index` into x->set. Returns its
// entry count, 0 when it is not a well-formed in-use set.
static int read_set(struct exfat_state *x, struct exf_stream *dir, uint32_t index) {
    uint8_t *set = x->set;
    if (!exf_dir_entry(x, dir, index, set) || set[0] != ET_FILE) return 0;
    int count = set[1] + 1;
    if (count < 3 || count > SET_MAX) return 0;
    for (int i = 1; i < count; i++)
        if (!exf_dir_entry(x, dir, index + (uint32_t)i, set + i * 32)) return 0;
    if (set[32] != ET_STREAM) return 0;
    for (int i = 2; i < count; i++) if (set[i * 32] != ET_NAME) return 0;
    if (exf_set_checksum(set, count) != le16(set + 2)) return 0;
    return count;
}

// The set in x->set's name, as UTF-16 into x->name16. Its length.
static int set_name(struct exfat_state *x, int count) {
    int len = x->set[32 + 3];
    if (len > NAME_MAX16 || len > (count - 2) * NAME_PER_ENTRY) return -1;
    for (int i = 0; i < len; i++)
        x->name16[i] = le16(x->set + (2 + i / NAME_PER_ENTRY) * 32 + 2 + (i % NAME_PER_ENTRY) * 2);
    return len;
}

// Walks a directory's in-use file sets. `*index` is the cursor (start at
// 0); returns the set's entry count with x->set filled, or 0 at the end.
static int next_set(struct exfat_state *x, struct exf_stream *dir, uint32_t *index) {
    uint8_t e[32];
    while (exf_dir_entry(x, dir, *index, e)) {
        if (e[0] == ET_END) return 0;
        uint32_t at = *index;
        if (e[0] != ET_FILE) { (*index)++; continue; }
        int count = read_set(x, dir, at);
        *index = at + (count ? (uint32_t)count : 1u);
        if (count) return count;
    }
    return 0;
}

static int names_equal(struct exfat_state *x, const uint16_t *a, const uint16_t *b, int len) {
    for (int i = 0; i < len; i++) if (up(x, a[i]) != up(x, b[i])) return 0;
    return 1;
}

int exf_find(struct exfat_state *x, struct exf_stream *dir, const char *name, struct exf_node *out) {
    uint16_t *want = x->want16;
    int wlen = exf_utf8_to16(name, want, NAME_MAX16);
    if (wlen <= 0) return 0;
    uint16_t hash = exf_name_hash(x, want, wlen);
    uint32_t index = 0;
    int count;
    while ((count = next_set(x, dir, &index))) {
        const uint8_t *s = x->set + 32;
        if (s[3] != wlen || le16(s + 4) != hash) continue;
        if (set_name(x, count) != wlen || !names_equal(x, x->name16, want, wlen)) continue;
        exf_node_from_set(out, x->set);
        out->loc.dir = *dir;
        out->loc.dir.cur_clus = out->loc.dir.cur_idx = 0;
        out->loc.index = index - (uint32_t)count;
        out->loc.count = count;
        return 1;
    }
    return 0;
}

static void root_node(struct exfat_state *x, struct exf_node *n) {
    k_memset(n, 0, sizeof *n);
    n->is_dir = 1;
    n->attr = ATTR_DIR;
    n->s.first = x->root;
    n->s.length = 0;   // the chain's: exf_stream_cluster stops at its end
}

int exf_lookup(struct exfat_state *x, const char *path, struct exf_node *out) {
    root_node(x, out);
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        if (!out->is_dir) return 0;
        struct exf_stream dir = out->s;
        dir.cur_clus = dir.cur_idx = 0;
        if (!exf_find(x, &dir, p, out)) return 0;
        while (*p && *p != '/') p++;
        while (*p == '/') p++;
    }
    return 1;
}

int exf_parent(struct exfat_state *x, const char *path, struct exf_node *dir, const char **leaf) {
    const char *last = path, *p = path;
    for (; *p; p++) if (*p == '/' && p[1]) last = p + 1;
    if (last == path && *path == '/') return 0;   // "/" has no parent
    *leaf = last;
    // The parent is the path up to the last component.
    char *parent = x->path;
    uint32_t n = (uint32_t)(last - path);
    if (n > FS_PATH_MAX) return 0;
    k_memcpy(parent, path, n);
    parent[n] = 0;
    return exf_lookup(x, parent, dir) && dir->is_dir;
}

// ---- time ---------------------------------------------------------------

// 7.4.8: DOS-style local fields, 2 s resolution plus a 10 ms increment,
// and an offset from UTC. Written as UTC with the offset marked valid, so
// another system shows the right local time.
uint32_t exf_now(uint8_t *ten_ms, uint8_t *utc) {
    struct rtc_time t;
    ktime_read(&t);
    uint32_t year = t.year < 1980 ? 1980 : t.year > 2107 ? 2107 : t.year;
    if (ten_ms) *ten_ms = (uint8_t)((t.second & 1) * 100);
    if (utc) *utc = 0x80;   // valid, +0
    return (year - 1980) << 25 | (uint32_t)t.month << 21 | (uint32_t)t.day << 16 |
           (uint32_t)t.hour << 11 | (uint32_t)t.minute << 5 | (t.second / 2);
}

static uint64_t to_epoch(uint32_t ts, uint8_t ten_ms, uint8_t utc) {
    if (!ts) return 0;
    struct rtc_time t;
    t.year = (uint16_t)(1980 + (ts >> 25));
    t.month = (uint8_t)((ts >> 21) & 0xF);
    t.day = (uint8_t)((ts >> 16) & 0x1F);
    t.hour = (uint8_t)((ts >> 11) & 0x1F);
    t.minute = (uint8_t)((ts >> 5) & 0x3F);
    t.second = (uint8_t)((ts & 0x1F) * 2 + ten_ms / 100);
    if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) return 0;
    int64_t e = (int64_t)cal_rtc_to_epoch(&t);
    if (utc & 0x80) {
        int off = (int8_t)(utc << 1) >> 1;   // seven-bit signed, quarter hours
        e -= (int64_t)off * 15 * 60;
    }
    return e < 0 ? 0 : (uint64_t)e;
}

// ---- mounting -----------------------------------------------------------

int exf_set_dirty(struct exfat_state *x, int dirty) {
    uint8_t *s = cached(x, 0);
    if (!s) return 0;
    uint16_t flags = le16(s + 106);
    flags = (uint16_t)(dirty ? (flags | 2) : (flags & ~2));
    exf_put16(s + 106, flags);   // outside the boot checksum by design (3.1.13)
    x->vol_flags = flags;
    return exf_write_sector(x, 0, s);
}

// The boot sector's fields, validated (3.1). 1 when it is exFAT.
static int parse_boot(struct exfat_state *x, const uint8_t *b) {
    if (k_memcmp(b + 3, "EXFAT   ", 8) != 0) return 0;
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    for (int i = 11; i < 64; i++) if (b[i]) return 0;   // MustBeZero: not a FAT BPB
    uint8_t bs = b[108], cs = b[109];
    if (bs < 9 || bs > 12 || cs > 25 - bs) return 0;
    x->bps_shift = bs;
    x->bps = 1u << bs;
    x->spc_shift = cs;
    x->cluster_bytes = x->bps << cs;
    x->vol_sectors = exf_le64(b + 72);
    x->fat_offset = exf_le32(b + 80);
    x->fat_length = exf_le32(b + 84);
    x->heap_offset = exf_le32(b + 88);
    x->cluster_count = exf_le32(b + 92);
    x->root = exf_le32(b + 96);
    x->serial = exf_le32(b + 100);
    x->vol_flags = le16(b + 106);
    if (b[110] != 1 && b[110] != 2) return 0;
    if (!x->cluster_count || x->heap_offset < x->fat_offset + x->fat_length) return 0;
    if (x->heap_offset + ((uint64_t)x->cluster_count << cs) > x->vol_sectors) return 0;
    if ((uint64_t)x->fat_length << bs < ((uint64_t)x->cluster_count + 2) * 4) return 0;
    if (!exf_cluster_ok(x, x->root)) return 0;
    return 1;
}

// The boot region's checksum (sector 11 repeats it). 1 when it holds.
static int boot_region_ok(struct exfat_state *x, uint64_t first) {
    uint8_t *region = kmalloc(12 * x->bps);
    if (!region) return 0;
    int ok = 1;
    for (uint32_t i = 0; i < 12 && ok; i++) ok = exf_read_sector(x, first + i, region + i * x->bps);
    if (ok) {
        uint32_t sum = exf_boot_checksum(region, x->bps);
        for (uint32_t i = 0; i < x->bps / 4 && ok; i++) ok = exf_le32(region + 11 * x->bps + i * 4) == sum;
    }
    kfree(region);
    return ok;
}

static int read_boot(struct exfat_state *x, const struct block_device *dev) {
    uint8_t *b = kmalloc(512);
    if (!b) return -1;
    int r = blkdev_read_partial(dev, 0, 1, b) ? 0 : -1;
    if (r == 0) {
        x->dev = dev;
        if (parse_boot(x, b)) r = 1;
    }
    kfree(b);
    if (r != 1) return r;
    if (x->bps < blkdev_block_size(dev)) {
        klog_printf("exfat: %u-byte sectors on a %u-byte-block device -- not mounted\n",
                    x->bps, blkdev_block_size(dev));
        return 0;
    }
    x->dev_per_sec = x->bps / 512;
    if (x->heap_offset + ((uint64_t)x->cluster_count << x->spc_shift) >
        blkdev_sector_count(dev) / x->dev_per_sec)
        return 0;
    return 1;
}

// Reads a whole stream into a fresh buffer (the bitmap, the up-case table).
static uint8_t *read_stream(struct exfat_state *x, struct exf_stream *s, uint64_t len) {
    uint8_t *out = kmalloc(len ? len : 1);
    if (!out) return 0;
    for (uint64_t off = 0; off < len; off += x->bps) {
        uint32_t c = exf_stream_cluster(x, s, off);
        if (!c) { kfree(out); return 0; }
        uint64_t sector = exf_cluster_sector(x, c) + ((off % x->cluster_bytes) >> x->bps_shift);
        uint8_t *sec = cached(x, sector);
        if (!sec) { kfree(out); return 0; }
        uint64_t n = len - off < x->bps ? len - off : x->bps;
        k_memcpy(out + off, sec, (uint32_t)n);
    }
    return out;
}

// 7.2.5: a run of identity mappings is stored as 0xFFFF and its length.
static int load_upcase(struct exfat_state *x, struct exf_stream *s, uint32_t checksum) {
    uint8_t *raw = read_stream(x, s, s->length);
    if (!raw) return 0;
    if (exf_table_checksum(raw, s->length) != checksum) {
        klog_write("exfat: the up-case table's checksum is wrong\n");
        kfree(raw);
        return 0;
    }
    x->upcase = kmalloc(65536 * sizeof(uint16_t));
    if (!x->upcase) { kfree(raw); return 0; }
    for (uint32_t i = 0; i < 65536; i++) x->upcase[i] = (uint16_t)i;
    uint32_t c = 0;
    for (uint64_t i = 0; i + 1 < s->length && c < 65536; i += 2) {
        uint16_t v = le16(raw + i);
        if (v == 0xFFFF && i + 3 < s->length) {
            c += le16(raw + i + 2);
            i += 2;
            continue;
        }
        x->upcase[c++] = v;
    }
    kfree(raw);
    return 1;
}

static int count_free(struct exfat_state *x) {
    uint32_t used = 0;
    for (uint32_t c = 0; c < x->cluster_count; c++)
        if (x->bitmap[c / 8] & (1u << (c % 8))) used++;
    x->free_clusters = x->cluster_count - used;
    return 1;
}

static void unload(struct exfat_state *x) {
    kfree(x->bitmap); x->bitmap = 0;
    kfree(x->upcase); x->upcase = 0;
    kfree(x->sec); x->sec = 0;
    kfree(x->fsec); x->fsec = 0;
    x->sec_valid = x->fsec_valid = 0;
    x->mounted = 0;
}

static int exfat_probe(void *st, const struct block_device *dev) {
    struct exfat_state *x = st;
    int r = read_boot(x, dev);
    if (r != 1) return r;
    x->sec = kmalloc(x->bps);
    if (!x->sec) return -1;
    int ok = boot_region_ok(x, 0) || boot_region_ok(x, 12);
    kfree(x->sec);
    x->sec = 0;
    return ok;
}

static int exfat_init(void *st, const struct block_device *dev, uint64_t size_bytes) {
    (void)size_bytes;
    struct exfat_state *x = st;
    if (read_boot(x, dev) != 1) return -1;
    x->sec = kmalloc(x->bps);
    x->fsec = kmalloc(x->bps);
    if (!x->sec || !x->fsec) { unload(x); return -1; }
    if (!boot_region_ok(x, 0)) {
        if (!boot_region_ok(x, 12)) {
            klog_write("exfat: neither boot region's checksum holds -- not mounted\n");
            unload(x);
            return -1;
        }
        klog_write("exfat: the main boot region is damaged; the backup's holds\n");
    }

    // The root's two system entries: the allocation bitmap and the
    // up-case table. Both must be there (7.1, 7.2).
    struct exf_node root;
    root_node(x, &root);
    int have_bitmap = 0, have_upcase = 0;
    uint32_t up_sum = 0;
    struct exf_stream up_s = { 0 };
    uint8_t e[32];
    for (uint32_t i = 0; exf_dir_entry(x, &root.s, i, e) && e[0] != ET_END; i++) {
        if (e[0] == ET_BITMAP && !(e[1] & 1)) {   // the first FAT's bitmap
            x->bitmap_s.first = exf_le32(e + 20);
            x->bitmap_s.length = exf_le64(e + 24);
            have_bitmap = 1;
        } else if (e[0] == ET_UPCASE) {
            up_s.first = exf_le32(e + 20);
            up_s.length = exf_le64(e + 24);
            up_sum = exf_le32(e + 4);
            have_upcase = 1;
        }
    }
    x->bitmap_bytes = (x->cluster_count + 7) / 8;
    if (!have_bitmap || !have_upcase || x->bitmap_s.length < x->bitmap_bytes ||
        !up_s.length || up_s.length > 0x20000) {
        klog_write("exfat: no usable allocation bitmap or up-case table -- not mounted\n");
        unload(x);
        return -1;
    }
    x->bitmap = read_stream(x, &x->bitmap_s, x->bitmap_bytes);
    if (!x->bitmap || !load_upcase(x, &up_s, up_sum)) { unload(x); return -1; }
    count_free(x);
    x->mounted = 1;
    x->writable = 1;
    if (x->vol_flags & 2)
        klog_printf("exfat: %s was not unmounted cleanly -- it may want a check on another system\n",
                    blk_device_name(dev));
    exf_set_dirty(x, 1);   // cleared by a clean unmount, as Windows does
    klog_printf("exfat: mounted, %u clusters of %u bytes, %u free\n", x->cluster_count,
                x->cluster_bytes, x->free_clusters);
    return 1;
}

static void exfat_umount(void *st, const struct block_device *dev) {
    (void)dev;
    struct exfat_state *x = st;
    if (x->mounted) exf_set_dirty(x, 0);
    unload(x);
}

// ---- reading --------------------------------------------------------------

static int exfat_exists(void *st, const char *path) {
    struct exf_node n;
    return ((struct exfat_state *)st)->mounted && exf_lookup(st, path, &n);
}

static int exfat_is_dir(void *st, const char *path) {
    struct exf_node n;
    return ((struct exfat_state *)st)->mounted && exf_lookup(st, path, &n) && n.is_dir;
}

static uint64_t exfat_size(void *st, const char *path) {
    struct exf_node n;
    if (!((struct exfat_state *)st)->mounted || !exf_lookup(st, path, &n) || n.is_dir) return 0;
    return n.s.length;
}

static uint32_t exfat_read_range(void *st, const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct exfat_state *x = st;
    struct exf_node n;
    if (!x->mounted || !exf_lookup(x, path, &n) || n.is_dir) return 0;
    if (offset >= n.s.length) return 0;
    if (len > n.s.length - offset) len = (uint32_t)(n.s.length - offset);
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t at = offset + done;
        uint32_t in_sec = (uint32_t)(at & (x->bps - 1));
        uint32_t n_here = x->bps - in_sec;
        if (n_here > len - done) n_here = len - done;
        if (at >= n.s.valid) {   // past ValidDataLength: zeros, whatever is on the disk
            k_memset(out + done, 0, n_here);
            done += n_here;
            continue;
        }
        if (at + n_here > n.s.valid) n_here = (uint32_t)(n.s.valid - at);
        uint32_t c = exf_stream_cluster(x, &n.s, at);
        if (!c) break;
        uint64_t sector = exf_cluster_sector(x, c) + ((at % x->cluster_bytes) >> x->bps_shift);
        if (in_sec == 0 && n_here == x->bps) {
            // Whole sectors straight into the caller's buffer, as many as
            // stay inside this cluster and the valid data.
            uint32_t left_in_clus = (uint32_t)((x->cluster_bytes - at % x->cluster_bytes) >> x->bps_shift);
            uint32_t want = (len - done) >> x->bps_shift;
            uint64_t valid_secs = (n.s.valid - at) >> x->bps_shift;
            uint32_t k = want < left_in_clus ? want : left_in_clus;
            if (k > valid_secs) k = (uint32_t)valid_secs;
            uint32_t xfer = (uint32_t)blkdev_max_sectors_per_xfer(x->dev) / x->dev_per_sec;
            if (k > xfer) k = xfer;
            if (k == 0) k = 1;
            if (!blkdev_read_sectors(x->dev, sector * x->dev_per_sec, (int)(k * x->dev_per_sec),
                                     out + done))
                break;
            done += k << x->bps_shift;
            continue;
        }
        uint8_t *s = cached(x, sector);
        if (!s) break;
        k_memcpy(out + done, s + in_sec, n_here);
        done += n_here;
    }
    return done;
}

static void exfat_list(void *st, const char *dir_path, fs_list_cb cb, void *ctx) {
    struct exfat_state *x = st;
    struct exf_node d;
    if (!x->mounted || !cb || !exf_lookup(x, dir_path, &d) || !d.is_dir) return;
    uint32_t index = 0;
    int count;
    while ((count = next_set(x, &d.s, &index))) {
        int len = set_name(x, count);
        if (len <= 0) continue;
        struct exf_node n;
        exf_node_from_set(&n, x->set);
        utf16_to8(x->name16, len, x->name8, NAME8_MAX);
        // The listing's size is 32 bits (fs_list_cb): a file of 4 GiB or
        // more lists as the most it can say; stat() has the real size.
        uint64_t size = n.is_dir ? 0 : n.s.length;
        cb(ctx, x->name8, size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size, n.is_dir);
    }
}

static int exfat_stat(void *st, const char *path, struct fs_stat_info *out) {
    struct exfat_state *x = st;
    struct exf_node n;
    if (!x->mounted || !exf_lookup(x, path, &n)) return 0;
    if (!out) return 1;
    // No inodes, as FAT32: the first cluster stands in, and is 0 for every
    // empty file -- which is why FS_CAP_INODES is not claimed.
    out->ino = n.s.first;
    out->created = to_epoch(n.crt, n.crt_10ms, n.crt_utc);
    out->modified = to_epoch(n.mod, n.mod_10ms, n.mod_utc);
    int ro = (n.attr & ATTR_RO) != 0;
    out->mode = n.is_dir ? (ro ? 0555 : 0755) : (ro ? 0444 : 0644);
    out->nlink = 1;
    out->is_dir = n.is_dir;
    out->size = n.is_dir ? 0 : n.s.length;
    return 1;
}

static int exfat_disk_usage(void *st, uint64_t *out_used, uint64_t *out_total) {
    struct exfat_state *x = st;
    if (!x->mounted) return 0;
    uint64_t total = (uint64_t)x->cluster_count * x->cluster_bytes;
    if (out_total) *out_total = total;
    if (out_used) *out_used = total - (uint64_t)x->free_clusters * x->cluster_bytes;
    return 1;
}

// ---- the steppable pairs: whole calls, as FAT32's ---------------------------

struct exf_step { int ok; uint32_t total; };

static void *exfat_write_range_begin(void *st, const char *path, uint64_t offset,
                                     const void *buf, uint32_t len) {
    struct exf_step *h = kmalloc(sizeof *h);
    if (!h) return 0;
    h->ok = exfat_write_range(st, path, offset, buf, len);
    h->total = h->ok ? len : 0;
    return h;
}

static int exfat_write_range_step(void *st, void *handle) {
    (void)st;
    struct exf_step *h = handle;
    if (!h) return FS_STEP_FAILED;
    int ok = h->ok;
    kfree(h);
    return ok ? FS_STEP_DONE : FS_STEP_FAILED;
}

static void *exfat_read_range_begin(void *st, const char *path, uint64_t offset, void *buf,
                                    uint32_t len) {
    struct exf_step *h = kmalloc(sizeof *h);
    if (!h) return 0;
    h->total = exfat_read_range(st, path, offset, buf, len);
    h->ok = 1;
    return h;
}

static int exfat_read_range_step(void *st, void *handle, uint32_t *out_total) {
    (void)st;
    struct exf_step *h = handle;
    if (!h) return FS_STEP_FAILED;
    if (out_total) *out_total = h->total;
    kfree(h);
    return FS_STEP_DONE;
}

// ---- checking -------------------------------------------------------------
//
// REPORT-ONLY, as FAT32's: every directory reached from the root, every
// entry set's checksum, and every cluster a stream claims -- inside the
// volume, marked in the bitmap, and claimed once. The structural damage
// a crash or a bad driver leaves; repair is fsck.exfat's job.

static const struct fsck_stage_desc EXF_STAGES[] = { { "Walk the directories", "directories" } };

static int claim(struct exfat_state *x, uint8_t *seen, struct exf_stream *s,
                 struct fs_check_result *r) {
    uint32_t n = exf_stream_clusters(x, s);
    if (!s->first) return 0;
    if (!s->length) n = 0xFFFFFFFFu;   // the root: to the end of its chain
    int bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = exf_stream_cluster(x, s, (uint64_t)i * x->cluster_bytes);
        if (!c) { if (s->length) { r->out_of_range++; bad++; } break; }
        uint32_t b = c - 2;
        if (!(x->bitmap[b / 8] & (1u << (b % 8)))) { r->referenced_but_free++; bad++; }
        if (seen[b / 8] & (1u << (b % 8))) { r->double_allocated++; bad++; }
        seen[b / 8] |= (uint8_t)(1u << (b % 8));
        r->blocks_referenced++;
    }
    return bad;
}

static int exfat_check(void *st, int repair, struct fs_check_result *out, struct fs_check_live *live) {
    struct exfat_state *x = st;
    if (!x->mounted) return 0;
    if (repair) klog_write("exfat: repair is not implemented -- reporting only\n");
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    uint8_t *seen = kzalloc(x->bitmap_bytes);
    int cap = 64, n = 0;
    struct exf_stream *queue = kmalloc((size_t)cap * sizeof *queue);
    if (!seen || !queue) { kfree(seen); kfree(queue); return 0; }
    int problems = 0, stopped = 0;
    fsck_begin(live, repair, EXF_STAGES, 1);
    fsck_stage(live, 0, 0);
    // The system streams count as claimed.
    problems += claim(x, seen, &x->bitmap_s, &r);
    struct exf_node root;
    root_node(x, &root);
    problems += claim(x, seen, &root.s, &r);
    queue[n++] = root.s;
    for (int q = 0; q < n; q++) {
        if (fsck_step(live, (uint32_t)q, 0)) { stopped = 1; break; }
        struct exf_stream dir = queue[q];
        uint8_t e[32];
        for (uint32_t i = 0; exf_dir_entry(x, &dir, i, e) && e[0] != ET_END; i++) {
            if (e[0] == ET_UPCASE) {
                struct exf_stream u = { exf_le32(e + 20), 0, exf_le64(e + 24), 0, 0, 0 };
                problems += claim(x, seen, &u, &r);
                continue;
            }
            if (e[0] != ET_FILE) continue;
            int count = read_set(x, &dir, i);
            if (!count) { problems++; r.dir_errors++; continue; }
            struct exf_node f;
            exf_node_from_set(&f, x->set);
            r.records_used++;
            problems += claim(x, seen, &f.s, &r);
            if (f.is_dir && f.s.first) {
                if (n == cap) {
                    struct exf_stream *g = krealloc(queue, (size_t)cap * 2 * sizeof *queue);
                    if (!g) { problems++; break; }
                    queue = g;
                    cap *= 2;
                }
                queue[n++] = f.s;
            }
            i += (uint32_t)count - 1;
        }
    }
    // A cluster the bitmap marks but nothing claims is leaked space.
    if (!stopped)
        for (uint32_t c = 0; c < x->cluster_count; c++)
            if ((x->bitmap[c / 8] & (1u << (c % 8))) && !(seen[c / 8] & (1u << (c % 8)))) r.leaked++;
    kfree(seen);
    kfree(queue);
    fsck_end(live);
    if (stopped) return -ECANCELED;
    if (out) *out = r;
    return problems == 0 && r.leaked == 0;
}

// ---- per-mount state --------------------------------------------------------

static void *exfat_state_alloc(void) { return kzalloc(sizeof(struct exfat_state)); }

static void exfat_state_free(void *st) {
    unload(st);
    kfree(st);
}

const struct fs_ops exfat_ops = {
    .name = "exfat",
    .caps = 0,                // no inodes, links, modes or epoch times -- FAT32's answer
    .volume_relative = 1,
    .max_mounts = FS_MOUNTS_ANY,
    .state_alloc = exfat_state_alloc,
    .state_free = exfat_state_free,
    .probe = exfat_probe,
    .wipe = exfat_wipe,
    .format = exfat_format,
    .init = exfat_init,
    .umount = exfat_umount,
    .touch = exfat_touch,
    .create_excl = exfat_create_excl,
    .write = exfat_write,
    .mkdir = exfat_mkdir,
    .del = exfat_del,
    .size = exfat_size,
    .read_range = exfat_read_range,
    .write_range = exfat_write_range,
    .write_range_begin = exfat_write_range_begin,
    .write_range_step = exfat_write_range_step,
    .read_range_begin = exfat_read_range_begin,
    .read_range_step = exfat_read_range_step,
    .rename = exfat_rename,
    .truncate = exfat_truncate,
    .is_dir = exfat_is_dir,
    .exists = exfat_exists,
    .list = exfat_list,
    .stat = exfat_stat,
    .disk_usage = exfat_disk_usage,
    .check = exfat_check,
};
