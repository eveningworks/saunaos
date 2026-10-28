// Which filesystem a volume holds, by signature (kernel/fs_ident.h).
//
// One table, tried IN ORDER, and the order is part of the answer: two
// signatures can be on a volume at once, because a format overwrites only
// what its own layout covers. tfs3 never writes its first 32 KiB, so an
// NTFS boot sector survives a tfs3 format underneath it -- the checksummed
// tfs3 superblock is asked first, and FAT's heuristic BPB check last.
// libblkid settles the same ambiguity with probe priorities.
#include "fs_ident.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "kutf8.h"
#include <stddef.h>

#define IDENT_BUF 4096u

struct ident {
    fs_ident_read_fn rd;
    void *ctx;
    uint64_t bytes;
    uint8_t *buf;          // IDENT_BUF bytes, the last read
    struct fs_ident *out;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static int fetch(struct ident *id, uint64_t off, uint32_t len) {
    if (len > IDENT_BUF || off > id->bytes || len > id->bytes - off) return 0;
    return id->rd(id->ctx, off, len, id->buf);
}

static int found(struct ident *id, const char *type) {
    k_strlcpy(id->out->type, type, sizeof id->out->type);
    return 1;
}

// An 8-bit label: up to `n` bytes, stopping at a NUL, trailing blanks
// trimmed (FAT and ISO pad with spaces).
static void label8(struct ident *id, const uint8_t *p, size_t n) {
    char *l = id->out->label;
    size_t k = 0;
    for (size_t i = 0; i < n && p[i] && k + 1 < sizeof id->out->label; i++)
        l[k++] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '?';
    while (k && l[k - 1] == ' ') k--;
    l[k] = 0;
}

// A UTF-16LE label of `units` code units (up to a NUL), as UTF-8 through
// api/kutf8.h's converter -- a label too long stops at a whole character.
static void label16(struct ident *id, const uint8_t *p, size_t units) {
    char *l = id->out->label;
    size_t n = 0;
    while (n < units && le16(p + 2 * n)) n++;
    kutf8_from_utf16le(p, (int)n, l, (int)sizeof id->out->label);
    size_t k = k_strlen(l);
    while (k && l[k - 1] == ' ') k--;
    l[k] = 0;
}

static int pow2(uint32_t v) { return v && !(v & (v - 1)); }

// tfs3: "TFS3" in block 8, and the superblock's own checksum.
static int p_tfs3(struct ident *id) {
    if (!fetch(id, 32768, 512)) return 0;
    const uint8_t *b = id->buf;
    if (k_memcmp(b, "TFS3", 4) || le32(b + 44) != k_fnv1a(b, 44)) return 0;
    return found(id, "tfs3");
}

// The volume's name is $Volume's VOLUME_NAME attribute: MFT record 3.
static void ntfs_label(struct ident *id, uint32_t bps, uint64_t cluster) {
    const uint8_t *b = id->buf;
    int8_t cpr = (int8_t)b[0x40];
    uint64_t rec = cpr > 0 ? (uint64_t)cpr * cluster : (cpr > -32 ? 1ull << -cpr : 0);
    uint64_t mft = le64(b + 0x30) * cluster;
    if (rec < 512 || rec > IDENT_BUF || !fetch(id, mft + 3 * rec, (uint32_t)rec)) return;
    uint8_t *r = id->buf;
    if (k_memcmp(r, "FILE", 4)) return;
    // The update sequence: the last two bytes of each 512-byte stride were
    // swapped out for a check value when the record was written.
    uint16_t usa = le16(r + 4), n = le16(r + 6);
    if (usa + 2u * n > rec || n == 0 || (n - 1) * 512u > rec) return;
    for (uint16_t i = 1; i < n; i++) {
        uint8_t *end = r + i * 512 - 2;
        if (le16(end) != le16(r + usa)) return;   // torn: no label rather than a wrong one
        end[0] = r[usa + 2 * i];
        end[1] = r[usa + 2 * i + 1];
    }
    (void)bps;
    for (uint32_t a = le16(r + 0x14); a + 24 <= rec;) {
        uint32_t type = le32(r + a), len = le32(r + a + 4);
        if (type == 0xffffffffu || len < 24 || a + len > rec) return;
        if (type == 0x60 && r[a + 8] == 0) {   // VOLUME_NAME, resident
            uint32_t vlen = le32(r + a + 0x10);
            uint16_t voff = le16(r + a + 0x14);
            if (voff + vlen <= len) label16(id, r + a + voff, vlen / 2);
            return;
        }
        a += len;
    }
}

static int p_ntfs(struct ident *id) {
    const uint8_t *b = id->buf;
    if (k_memcmp(b + 3, "NTFS    ", 8)) return 0;
    uint32_t bps = le16(b + 11), spc = b[13];
    // Above 128, sectors per cluster is a negative shift (64 KiB+ clusters).
    if (spc > 128) spc = spc < 0xe0 ? 0 : 1u << (256 - spc);
    if (!pow2(bps) || bps < 256 || bps > 4096 || !pow2(spc)) return 0;
    found(id, "ntfs");
    ntfs_label(id, bps, (uint64_t)bps * spc);
    return 1;
}

// The label is a directory entry (0x83) in the root, not in the boot sector.
static int p_exfat(struct ident *id) {
    const uint8_t *b = id->buf;
    if (k_memcmp(b + 3, "EXFAT   ", 8)) return 0;
    uint32_t bpss = b[108], spcs = b[109], heap = le32(b + 88), root = le32(b + 96);
    if (bpss < 9 || bpss > 12 || spcs > 25 - bpss || root < 2) return 0;
    found(id, "exfat");
    uint64_t off = ((uint64_t)heap + ((uint64_t)(root - 2) << spcs)) << bpss;
    if (!fetch(id, off, 512)) return 1;
    for (int i = 0; i < 16; i++) {
        const uint8_t *e = id->buf + 32 * i;
        if (e[0] == 0x00) break;
        if (e[0] == 0x83) { label16(id, e + 2, e[1] <= 11 ? e[1] : 11); break; }
    }
    return 1;
}

static int p_sector0(struct ident *id) {
    if (!fetch(id, 0, 4096 <= id->bytes ? 4096 : 512)) return 0;
    const uint8_t *b = id->buf;
    if (!k_memcmp(b + 3, "-FVE-FS-", 8)) return found(id, "BitLocker");
    if (!k_memcmp(b + 3, "ReFS\0\0\0\0", 8)) return found(id, "refs");
    if (!k_memcmp(b, "XFSB", 4)) { found(id, "xfs"); label8(id, b + 108, 12); return 1; }
    if (!k_memcmp(b, "LUKS\xba\xbe", 6)) {
        found(id, "crypto_LUKS");
        if (b[6] == 0 && b[7] == 2) label8(id, b + 24, 48);
        return 1;
    }
    if (!k_memcmp(b + 32, "NXSB", 4)) return found(id, "apfs");
    if (p_ntfs(id) || p_exfat(id)) return 1;
    if (id->bytes < 4096) return 0;
    // 1 KiB in: ext2/3/4 and HFS+.
    const uint8_t *s = b + 1024;
    if (le16(s + 56) == 0xef53) {
        uint32_t compat = le32(s + 92), incompat = le32(s + 96), ro = le32(s + 100);
        // blkid's rule: any ext4-only feature makes it ext4, else a journal
        // makes it ext3.
        int four = (incompat & (0x40 | 0x80 | 0x200)) || (ro & (0x8 | 0x10 | 0x20 | 0x40));
        found(id, four ? "ext4" : (compat & 0x4) ? "ext3" : "ext2");
        label8(id, s + 120, 16);
        return 1;
    }
    if ((s[0] == 'H' && s[1] == '+' && s[3] == 4) || (s[0] == 'H' && s[1] == 'X' && s[3] == 5))
        return found(id, "hfsplus");
    // The end of the first 4 KiB page: Linux swap.
    if (!k_memcmp(b + 4086, "SWAPSPACE2", 10) || !k_memcmp(b + 4086, "SWAP-SPACE", 10)) {
        found(id, "swap");
        if (le32(s) == 1) label8(id, s + 28, 16);
        return 1;
    }
    return 0;
}

static int p_iso(struct ident *id) {
    if (!fetch(id, 32768, 512)) return 0;
    const uint8_t *b = id->buf;
    if (b[0] != 1 || k_memcmp(b + 1, "CD001", 5)) return 0;
    found(id, "iso9660");
    label8(id, b + 40, 32);
    return 1;
}

static int p_btrfs(struct ident *id) {
    if (!fetch(id, 65536, 1024)) return 0;
    if (k_memcmp(id->buf + 0x40, "_BHRfS_M", 8)) return 0;
    found(id, "btrfs");
    label8(id, id->buf + 0x12b, 256 < sizeof id->out->label ? 256 : sizeof id->out->label);
    return 1;
}

// LAST: a FAT boot sector is recognised by a BPB that merely looks sane.
static int p_fat(struct ident *id) {
    if (!fetch(id, 0, 512)) return 0;
    const uint8_t *b = id->buf;
    uint32_t bps = le16(b + 11), spc = b[13], reserved = le16(b + 14), fats = b[16];
    if (b[510] != 0x55 || b[511] != 0xaa) return 0;
    if (!pow2(bps) || bps < 512 || bps > 4096 || !pow2(spc) || !reserved || fats < 1 || fats > 2)
        return 0;
    const uint8_t *lab;
    if (le16(b + 22) == 0 && !k_memcmp(b + 82, "FAT32   ", 8)) {
        found(id, "fat32");
        lab = b + 71;
    } else if (!k_memcmp(b + 54, "FAT16   ", 8) || !k_memcmp(b + 54, "FAT12   ", 8)) {
        found(id, b[58] == '6' ? "fat16" : "fat12");
        lab = b + 43;
    } else {
        return 0;
    }
    if (k_memcmp(lab, "NO NAME    ", 11)) label8(id, lab, 11);
    return 1;
}

int fs_ident_read(fs_ident_read_fn rd, void *ctx, uint64_t bytes, struct fs_ident *out) {
    k_memset(out, 0, sizeof *out);
    uint8_t *buf = kmalloc(IDENT_BUF);
    if (!buf) return 0;
    struct ident id = { rd, ctx, bytes, buf, out };
    int ok = p_tfs3(&id) || p_sector0(&id) || p_iso(&id) || p_btrfs(&id) || p_fat(&id);
    kfree(buf);
    if (!ok) k_memset(out, 0, sizeof *out);
    return ok;
}

// Whole 512-byte sectors around the bytes asked for; the partial read
// handles a 4K-sector disk.
static int dev_read(void *ctx, uint64_t off, uint32_t len, void *buf) {
    const struct block_device *dev = ctx;
    uint64_t lba = off / 512, end = (off + len + 511) / 512;
    uint32_t skip = (uint32_t)(off % 512);
    if (skip == 0 && len % 512 == 0) return blkdev_read_partial(dev, lba, (int)(len / 512), buf);
    uint8_t *tmp = kmalloc((size_t)(end - lba) * 512);
    if (!tmp) return 0;
    int ok = blkdev_read_partial(dev, lba, (int)(end - lba), tmp);
    if (ok) k_memcpy(buf, tmp + skip, len);
    kfree(tmp);
    return ok;
}

int fs_ident(const struct block_device *dev, struct fs_ident *out) {
    if (!dev) { k_memset(out, 0, sizeof *out); return 0; }
    return fs_ident_read(dev_read, (void *)dev, blkdev_sector_count(dev) * 512, out);
}
