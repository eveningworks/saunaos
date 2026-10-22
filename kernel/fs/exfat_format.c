// exFAT: formatting a volume and wiping one. See exfat_internal.h.
//
// THE LAYOUT, Microsoft's defaults (Default Cluster Size for exFAT): 4 KiB
// clusters up to 256 MiB, 32 KiB up to 32 GiB, 128 KiB past it; the FAT at
// 1 MiB so the cluster heap starts aligned on flash; the bitmap, the
// up-case table and the root directory in the first clusters.
//
// THE UP-CASE TABLE IS GENERATED, not copied: the case pairs of the
// scripts people name files in -- Latin, Greek, Cyrillic, Armenian,
// fullwidth -- compressed as 7.2.5 allows. Every name this volume hashes
// goes through the table on the disk, which is this one, so two systems
// agree on a name's hash whatever the table holds. A pair this table
// does not know is two different names, as on any volume whose table
// lacks it.
#include "exfat_internal.h"
#include "heap.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"
#include "krandom.h"

// The upper case of BMP code unit `c`, or `c`.
static uint16_t upper_of(uint32_t c) {
    if (c >= 0x61 && c <= 0x7A) return (uint16_t)(c - 0x20);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (uint16_t)(c - 0x20);
    if (c == 0xFF) return 0x178;
    // Latin Extended-A: pairs, upper first, except the two runs where the
    // pair starts on an odd code point.
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (uint16_t)((c & 1) ? c - 1 : c);
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (uint16_t)((c & 1) ? c : c - 1);
    // Greek
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return (uint16_t)(c - 0x25);
    if (c >= 0x3B1 && c <= 0x3C1) return (uint16_t)(c - 0x20);
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x3C3 && c <= 0x3CB) return (uint16_t)(c - 0x20);
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3CD || c == 0x3CE) return (uint16_t)(c - 0x3F);
    // Cyrillic
    if (c >= 0x430 && c <= 0x44F) return (uint16_t)(c - 0x20);
    if (c >= 0x450 && c <= 0x45F) return (uint16_t)(c - 0x50);
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
        return (uint16_t)((c & 1) ? c - 1 : c);
    if (c >= 0x4C1 && c <= 0x4CE) return (uint16_t)((c & 1) ? c : c - 1);
    if (c == 0x4CF) return 0x4C0;
    // Armenian
    if (c >= 0x561 && c <= 0x586) return (uint16_t)(c - 0x30);
    // Latin Extended Additional (Vietnamese and more)
    if ((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) return (uint16_t)((c & 1) ? c - 1 : c);
    // Fullwidth Latin
    if (c >= 0xFF41 && c <= 0xFF5A) return (uint16_t)(c - 0x20);
    return (uint16_t)c;
}

// The compressed table into `out` (room for 2 * 65536 bytes). Its length.
static uint32_t build_upcase(uint8_t *out) {
    uint32_t n = 0, c = 0;
    while (c < 0x10000) {
        uint32_t run = 0;
        while (c + run < 0x10000 && upper_of(c + run) == c + run && run < 0xFFFF) run++;
        if (run >= 3) {   // worth the two units a compressed run costs
            exf_put16(out + n, 0xFFFF); exf_put16(out + n + 2, (uint16_t)run);
            n += 4;
            c += run;
            continue;
        }
        exf_put16(out + n, upper_of(c));
        n += 2;
        c++;
    }
    return n;
}

static int write_zeros(const struct block_device *dev, uint64_t lba, uint64_t count) {
    uint32_t chunk = (uint32_t)blkdev_max_sectors_per_xfer(dev);
    uint8_t *z = kzalloc((size_t)chunk * 512);
    if (!z) return 0;
    int ok = 1;
    for (uint64_t at = 0; at < count && ok;) {
        uint32_t c = count - at < chunk ? (uint32_t)(count - at) : chunk;
        ok = blkdev_write_sectors(dev, lba + at, (int)c, z);
        at += c;
    }
    kfree(z);
    return ok;
}

int exfat_wipe(void *st, const struct block_device *dev) {
    (void)st;
    if (!dev || blkdev_sector_count(dev) < 24) return 1;
    // The main and backup boot sectors: the signature a probe reads.
    uint32_t spb = blkdev_block_sectors(dev);
    uint32_t backup = 12 * (spb > 1 ? spb : 1);
    return write_zeros(dev, 0, spb) && write_zeros(dev, backup, spb);
}

int exfat_format(void *st, const struct block_device *dev) {
    struct exfat_state *x = st;
    k_memset(x, 0, sizeof *x);
    x->dev = dev;
    uint32_t block = blkdev_block_size(dev);
    x->bps = block < 512 ? 512 : block;
    x->bps_shift = x->bps == 4096 ? 12 : 9;
    x->dev_per_sec = x->bps / 512;
    uint64_t bytes = blkdev_sector_count(dev) * 512ull;
    x->vol_sectors = bytes >> x->bps_shift;
    uint32_t cbytes = bytes <= (256ull << 20) ? 4096 : bytes <= (32ull << 30) ? 32768 : 131072;
    if (cbytes < x->bps) cbytes = x->bps;
    x->cluster_bytes = cbytes;
    for (x->spc_shift = 0; (x->bps << x->spc_shift) < cbytes; x->spc_shift++) {}
    uint32_t spc = 1u << x->spc_shift;

    x->fat_offset = bytes >= (64ull << 20) ? (1u << 20) >> x->bps_shift : 32;
    uint64_t cc = 0;
    for (int pass = 0; pass < 4; pass++) {
        x->fat_length = (uint32_t)(((cc + 2) * 4 + x->bps - 1) / x->bps);
        uint64_t heap = ((uint64_t)x->fat_offset + x->fat_length + spc - 1) & ~(uint64_t)(spc - 1);
        x->heap_offset = (uint32_t)heap;
        cc = heap < x->vol_sectors ? (x->vol_sectors - heap) >> x->spc_shift : 0;
        if (cc > 0xFFFFFFF5u) cc = 0xFFFFFFF5u;
    }
    if (cc < 16) {
        klog_write("exfat: the volume is too small to format\n");
        return 0;
    }
    x->cluster_count = (uint32_t)cc;

    // The system clusters: bitmap, up-case table, root.
    uint32_t bitmap_bytes = (x->cluster_count + 7) / 8;
    uint32_t nb = (bitmap_bytes + cbytes - 1) / cbytes;
    uint8_t *table = kmalloc(2 * 65536);
    uint8_t *sec = kzalloc(x->bps);
    uint8_t *region = kzalloc(12 * x->bps);
    if (!table || !sec || !region) { kfree(table); kfree(sec); kfree(region); return 0; }
    uint32_t tlen = build_upcase(table);
    uint32_t tsum = exf_table_checksum(table, tlen);
    uint32_t nu = (tlen + cbytes - 1) / cbytes;
    uint32_t c_bitmap = 2, c_upcase = 2 + nb, c_root = 2 + nb + nu;
    uint32_t used = nb + nu + 1;
    int ok = 1;

    // The FAT: zeroed, then the media entries and the three chains.
    ok = write_zeros(dev, (uint64_t)x->fat_offset * x->dev_per_sec,
                     (uint64_t)x->fat_length * x->dev_per_sec);
    uint32_t last_entry = c_root;
    for (uint32_t s = 0; ok && s <= (last_entry * 4) / x->bps; s++) {
        k_memset(sec, 0, x->bps);
        for (uint32_t i = 0; i < x->bps / 4; i++) {
            uint32_t c = s * (x->bps / 4) + i, v = 0;
            if (c == 0) v = 0xFFFFFFF8u;
            else if (c == 1) v = 0xFFFFFFFFu;
            else if (c >= c_bitmap && c < c_upcase) v = c + 1 < c_upcase ? c + 1 : EXFAT_EOC;
            else if (c >= c_upcase && c < c_root) v = c + 1 < c_root ? c + 1 : EXFAT_EOC;
            else if (c == c_root) v = EXFAT_EOC;
            exf_put32(sec + i * 4, v);
        }
        ok = blkdev_write_sectors(dev, ((uint64_t)x->fat_offset + s) * x->dev_per_sec,
                                  (int)x->dev_per_sec, sec);
    }

    // The bitmap's clusters, zeroed, with the system clusters' bits set.
    uint64_t heap_lba = (uint64_t)x->heap_offset * x->dev_per_sec;
    uint64_t clus_lba = (uint64_t)spc * x->dev_per_sec;
    if (ok) ok = write_zeros(dev, heap_lba, (uint64_t)(nb + nu + 1) * clus_lba);
    for (uint32_t s = 0; ok && s * x->bps * 8 < used; s++) {
        k_memset(sec, 0, x->bps);
        for (uint32_t b = s * x->bps * 8; b < used && b < (s + 1) * x->bps * 8; b++)
            sec[(b / 8) % x->bps] |= (uint8_t)(1u << (b % 8));
        ok = blkdev_write_sectors(dev, heap_lba + (uint64_t)s * x->dev_per_sec, (int)x->dev_per_sec, sec);
    }
    // The up-case table.
    uint64_t up_lba = heap_lba + (uint64_t)(c_upcase - 2) * clus_lba;
    for (uint32_t off = 0; ok && off < tlen; off += x->bps) {
        k_memset(sec, 0, x->bps);
        k_memcpy(sec, table + off, tlen - off < x->bps ? tlen - off : x->bps);
        ok = blkdev_write_sectors(dev, up_lba + (uint64_t)(off / x->bps) * x->dev_per_sec,
                                  (int)x->dev_per_sec, sec);
    }
    // The root: an empty label, the bitmap and the up-case table.
    if (ok) {
        k_memset(sec, 0, x->bps);
        uint8_t *e = sec;
        e[0] = ET_LABEL;                                   // CharacterCount 0: no label
        e += 32;
        e[0] = ET_BITMAP;
        exf_put32(e + 20, c_bitmap);
        exf_put64(e + 24, bitmap_bytes);
        e += 32;
        e[0] = ET_UPCASE;
        exf_put32(e + 4, tsum);
        exf_put32(e + 20, c_upcase);
        exf_put64(e + 24, tlen);
        ok = blkdev_write_sectors(dev, heap_lba + (uint64_t)(c_root - 2) * clus_lba,
                                  (int)x->dev_per_sec, sec);
    }

    // The boot region (3.1-3.4), main then backup.
    if (ok) {
        uint8_t *b = region;
        b[0] = 0xEB; b[1] = 0x76; b[2] = 0x90;
        k_memcpy(b + 3, "EXFAT   ", 8);
        exf_put64(b + 72, x->vol_sectors);
        exf_put32(b + 80, x->fat_offset);
        exf_put32(b + 84, x->fat_length);
        exf_put32(b + 88, x->heap_offset);
        exf_put32(b + 92, x->cluster_count);
        exf_put32(b + 96, c_root);
        exf_put32(b + 100, (uint32_t)krandom_u64());
        exf_put16(b + 104, 0x0100);        // revision 1.00
        b[108] = (uint8_t)x->bps_shift;
        b[109] = (uint8_t)x->spc_shift;
        b[110] = 1;                        // one FAT
        b[111] = 0x80;
        b[112] = (uint8_t)((uint64_t)used * 100 / x->cluster_count);
        k_memset(b + 120, 0xF4, 390);      // boot code: hlt
        b[510] = 0x55; b[511] = 0xAA;
        for (int i = 1; i <= 8; i++) {     // extended boot sectors' signatures
            uint8_t *s = region + i * x->bps;
            s[x->bps - 2] = 0x55;
            s[x->bps - 1] = 0xAA;
        }
        uint32_t sum = exf_boot_checksum(region, x->bps);
        for (uint32_t i = 0; i < x->bps / 4; i++) exf_put32(region + 11 * x->bps + i * 4, sum);
        for (int copy = 0; copy < 2 && ok; copy++)
            ok = blkdev_write_sectors(dev, (uint64_t)copy * 12 * x->dev_per_sec,
                                      (int)(12 * x->dev_per_sec), region);
    }
    kfree(table);
    kfree(sec);
    kfree(region);
    if (ok) blkdev_flush(dev);
    k_memset(x, 0, sizeof *x);
    return ok;
}
