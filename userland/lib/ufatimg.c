// See ufatimg.h. FAT12 and FAT16 disk images, read from memory.
#include "lib/ufatimg.h"
#include <errno.h>
#include <string.h>

static uint32_t rd16(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void trimmed(char *out, const uint8_t *src, size_t n) {
    memcpy(out, src, n);
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

int ufatimg_open(struct ufatimg *v, const uint8_t *img, size_t n) {
    memset(v, 0, sizeof *v);
    if (n < 512) return -EINVAL;
    const uint8_t *b = img;
    uint32_t bps = rd16(b + 11), spc = b[13], reserved = rd16(b + 14), nfats = b[16];
    uint32_t root_entries = rd16(b + 17), total = rd16(b + 19), fat_secs = rd16(b + 22);
    if (!total) total = rd32(b + 32);
    if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || !spc || (spc & (spc - 1)) ||
        !reserved || !nfats || nfats > 2 || !root_entries || !fat_secs || !total)
        return -EINVAL;
    uint64_t fat_at = (uint64_t)reserved * bps;
    uint64_t root_at = fat_at + (uint64_t)nfats * fat_secs * bps;
    uint64_t root_bytes = (uint64_t)root_entries * 32;
    uint64_t data_at = root_at + (root_bytes + bps - 1) / bps * bps;
    uint64_t vol = (uint64_t)total * bps;
    if (data_at >= vol || root_at + root_bytes > n) return -EINVAL;
    uint64_t clusters = (vol - data_at) / ((uint64_t)bps * spc);
    if (clusters < 1 || clusters >= 65525) return -EINVAL;   // FAT32 is the kernel's
    v->fat16 = clusters >= 4085;
    // The FAT must hold an entry for every cluster, and the image must hold the FAT.
    uint64_t need = v->fat16 ? (clusters + 2) * 2 : (clusters + 2) * 3 / 2 + 1;
    if ((uint64_t)fat_secs * bps < need || fat_at + (uint64_t)fat_secs * bps > n) return -EINVAL;
    v->img = img;
    v->n = n;
    v->bps = bps;
    v->spc = spc;
    v->fat_at = (uint32_t)fat_at;
    v->fat_bytes = fat_secs * bps;
    v->root_at = (uint32_t)root_at;
    v->root_entries = root_entries;
    v->data_at = (uint32_t)data_at;
    v->clusters = (uint32_t)clusters;
    if (b[38] == 0x29) trimmed(v->label, b + 43, 11);   // the extended boot record's label
    // The root's volume-label entry wins, as DOS's LABEL writes both.
    for (uint32_t i = 0; i < root_entries; i++) {
        const uint8_t *e = img + root_at + i * 32;
        if (!e[0]) break;
        if (e[0] != 0xE5 && (e[11] & 0x3F) == 0x08) {
            trimmed(v->label, e, 11);
            break;
        }
    }
    if (!strcmp(v->label, "NO NAME")) v->label[0] = 0;
    return 0;
}

// The FAT's entry for `c`: the next cluster, or 0xFFFF for the end of a chain.
static uint32_t next_of(const struct ufatimg *v, uint32_t c) {
    const uint8_t *fat = v->img + v->fat_at;
    uint32_t x;
    if (v->fat16) {
        x = rd16(fat + c * 2);
        return x >= 0xFFF8 ? 0xFFFF : x;
    }
    x = rd16(fat + c + c / 2);
    x = c & 1 ? x >> 4 : x & 0xFFF;
    return x >= 0xFF8 ? 0xFFFF : x;
}

static int valid(const struct ufatimg *v, uint32_t c) { return c >= 2 && c < v->clusters + 2; }

// Cluster `c`'s bytes, or NULL when the image is cut short there.
static const uint8_t *cluster(const struct ufatimg *v, uint32_t c, size_t *len) {
    uint64_t at = v->data_at + (uint64_t)(c - 2) * v->bps * v->spc;
    *len = (size_t)v->bps * v->spc;
    if (at + *len > v->n) return 0;
    return v->img + at;
}

// 1 to go on, 0 the directory's end (a zero first byte), <0 stop with that.
static int entry(const uint8_t *e, int (*cb)(void *, const struct ufatimg_ent *), void *ctx, int *count) {
    if (!e[0]) return 0;
    if (e[0] == 0xE5 || (e[11] & 0x0F) == 0x0F || (e[11] & 0x08) || e[0] == '.') return 1;
    struct ufatimg_ent out;
    char base[9], ext[4];
    trimmed(base, e, 8);
    trimmed(ext, e + 8, 3);
    if ((uint8_t)base[0] == 0x05) base[0] = (char)0xE5;   // DOS's escape for a name starting E5
    if (ext[0]) {
        size_t b = strlen(base);
        memcpy(out.name, base, b);
        out.name[b] = '.';
        memcpy(out.name + b + 1, ext, strlen(ext) + 1);
    } else {
        memcpy(out.name, base, strlen(base) + 1);
    }
    out.attr = e[11];
    out.size = rd32(e + 28);
    out.cluster = rd16(e + 26);
    (*count)++;
    return cb(ctx, &out) ? -1 : 1;
}

int ufatimg_list(const struct ufatimg *v, uint32_t dir,
                 int (*cb)(void *ctx, const struct ufatimg_ent *e), void *ctx) {
    int count = 0, r;
    if (!dir) {
        for (uint32_t i = 0; i < v->root_entries; i++)
            if ((r = entry(v->img + v->root_at + i * 32, cb, ctx, &count)) <= 0) return count;
        return count;
    }
    // A subdirectory is a chain like a file's; at most `clusters` links, so a loop ends.
    uint32_t c = dir;
    for (uint32_t steps = 0; c != 0xFFFF; steps++) {
        if (!valid(v, c) || steps > v->clusters) return -EINVAL;
        size_t len;
        const uint8_t *p = cluster(v, c, &len);
        if (!p) return -EINVAL;
        for (size_t at = 0; at + 32 <= len; at += 32)
            if ((r = entry(p + at, cb, ctx, &count)) <= 0) return count;
        c = next_of(v, c);
    }
    return count;
}

struct finder { const char *name; struct ufatimg_ent *out; int found; };

static int same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

static int find_cb(void *ctx, const struct ufatimg_ent *e) {
    struct finder *f = ctx;
    if (!same_name(e->name, f->name)) return 0;
    *f->out = *e;
    f->found = 1;
    return 1;
}

int ufatimg_find(const struct ufatimg *v, uint32_t dir, const char *name, struct ufatimg_ent *out) {
    struct finder f = { name, out, 0 };
    int r = ufatimg_list(v, dir, find_cb, &f);
    if (r < 0) return r;
    return f.found ? 0 : -ENOENT;
}

int ufatimg_read(const struct ufatimg *v, const struct ufatimg_ent *e,
                 int (*out)(void *ctx, const uint8_t *data, size_t n), void *ctx) {
    uint32_t left = e->size, c = e->cluster;
    if (!left) return 0;
    for (uint32_t steps = 0; left; steps++) {
        if (c == 0xFFFF || !valid(v, c) || steps > v->clusters) return -EINVAL;
        size_t len;
        const uint8_t *p = cluster(v, c, &len);
        if (!p) return -EINVAL;
        if (len > left) len = left;
        if (out(ctx, p, len)) return -EIO;
        left -= (uint32_t)len;
        c = next_of(v, c);
    }
    return 0;
}
