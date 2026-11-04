// What starts a disk or a volume: an MBR's partition table, a GPT (its
// protective MBR, the header at LBA 1 and the entries, names from their
// UTF-16), and a FAT boot sector's BIOS parameter block with the regions
// it lays out. Little-endian throughout; a 512-byte sector unless the
// GPT header is found at 4096.
#include "base/ubinmap_internal.h"

#include <stdio.h>
#include <string.h>

#define GPT_SCAN_MAX 65536   // entries examined, used or not
#define GPT_LIST_MAX 4096    // used entries listed

static int has_signature(struct ubinmap_ctx *c) {
    uint8_t s[2];
    return !bm_read(c, 510, s, 2) && s[0] == 0x55 && s[1] == 0xAA;
}

static const char *mbr_type(unsigned t) {
    switch (t) {
    case 0x00: return "empty";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16, under 32 MiB";
    case 0x05: return "extended";
    case 0x06: return "FAT16";
    case 0x07: return "NTFS or exFAT";
    case 0x0B: return "FAT32";
    case 0x0C: return "FAT32 (LBA)";
    case 0x0E: return "FAT16 (LBA)";
    case 0x0F: return "extended (LBA)";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0x8E: return "Linux LVM";
    case 0xA5: return "FreeBSD";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System";
    default: return "unknown";
    }
}

// The four entries of a partition table, as nodes. Returns how many are
// in use; `*protective` set when one is a GPT's 0xEE.
static int partition_table(struct ubinmap_ctx *c, int *protective) {
    int used = 0;
    if (protective) *protective = 0;
    bm_add(c, 0, 446, 64, 0, "Partition table", "4 entries");
    for (int i = 0; i < 4; i++) {
        uint8_t e[16];
        uint64_t at = 446 + (uint64_t)i * 16;
        if (bm_read(c, at, e, 16)) break;
        unsigned boot = e[0], type = e[4];
        uint32_t start = bm_get32(c, e + 8), count = bm_get32(c, e + 12);
        char label[24], size[32];
        snprintf(label, sizeof label, "Partition %d", i + 1);
        bm_human((uint64_t)count * 512, size, sizeof size);
        if (type) used++;
        if (type == 0xEE && protective) *protective = 1;
        bm_add(c, 1, at, 16, type ? 2 + (i & 1) : 0, label, "0x%02X %s", type, mbr_type(type));
        bm_add(c, 2, at, 1, 0, "Boot flag", "%s", boot == 0x80 ? "active" : boot ? "invalid" : "no");
        bm_add(c, 2, at + 4, 1, 0, "Type", "0x%02X %s", type, mbr_type(type));
        bm_add(c, 2, at + 8, 4, 0, "First sector", "%u", (unsigned)start);
        bm_add(c, 2, at + 12, 4, 0, "Sectors", "%u (%s)", (unsigned)count, size);
    }
    return used;
}

int ubinmap_mbr(struct ubinmap_ctx *c) {
    if (c->src->size < 512 || !has_signature(c)) return 0;
    for (int i = 0; i < 4; i++) {   // a boot flag is 0 or 0x80 -- anything else is not a table
        uint8_t f;
        if (bm_u8(c, 446 + (uint64_t)i * 16, &f) || (f && f != 0x80)) return 0;
    }
    c->be = 0;
    uint32_t sig = 0;
    bm_u32(c, 440, &sig);
    bm_add(c, 0, 0, 440, 1, "Boot code", "440 bytes");
    bm_add(c, 0, 440, 4, 0, "Disk signature", "0x%08X", (unsigned)sig);
    int used = partition_table(c, 0);
    bm_add(c, 0, 510, 2, 4, "Signature", "55 AA");
    bm_format(c, "MBR disk, %d partition%s", used, used == 1 ? "" : "s");
    return 1;
}

// --- GPT ------------------------------------------------------------------

static const struct { const char *guid, *name; } GPT_TYPES[] = {
    { "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI System" },
    { "21686148-6449-6E6F-744E-656564454649", "BIOS boot" },
    { "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Basic data" },
    { "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved" },
    { "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows recovery" },
    { "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem" },
    { "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux root (x86-64)" },
    { "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap" },
    { "E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM" },
    { "933AC7E1-2EB4-4F13-B844-0E14E2AEF915", "Linux home" },
    { "48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS+" },
    { "7C3457EF-0000-11AA-AA11-00306543ECAC", "Apple APFS" },
    { "516E7CB6-6ECF-11D6-8FF8-00022D09712B", "FreeBSD" },
};

static const char *gpt_type(const char *guid) {
    for (unsigned i = 0; i < sizeof GPT_TYPES / sizeof GPT_TYPES[0]; i++)
        if (!strcmp(GPT_TYPES[i].guid, guid)) return GPT_TYPES[i].name;
    return "unknown type";
}

// A GPT name: UTF-16LE, 36 code units, to UTF-8 -- a lone surrogate as U+FFFD.
static void utf16_name(const uint8_t *p, char *out, size_t cap) {
    size_t k = 0;
    for (int i = 0; i < 36; i++) {
        uint32_t u = (uint32_t)(p[2 * i] | p[2 * i + 1] << 8);
        if (!u) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < 36) {
            uint32_t lo = (uint32_t)(p[2 * i + 2] | p[2 * i + 3] << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            } else {
                u = 0xFFFD;
            }
        } else if (u >= 0xD800 && u < 0xE000) {
            u = 0xFFFD;
        }
        if (u < 0x20) u = '?';
        char b[4];
        int n;
        if (u < 0x80) { b[0] = (char)u; n = 1; }
        else if (u < 0x800) { b[0] = (char)(0xC0 | u >> 6); b[1] = (char)(0x80 | (u & 63)); n = 2; }
        else if (u < 0x10000) {
            b[0] = (char)(0xE0 | u >> 12); b[1] = (char)(0x80 | (u >> 6 & 63)); b[2] = (char)(0x80 | (u & 63)); n = 3;
        } else {
            b[0] = (char)(0xF0 | u >> 18); b[1] = (char)(0x80 | (u >> 12 & 63));
            b[2] = (char)(0x80 | (u >> 6 & 63)); b[3] = (char)(0x80 | (u & 63)); n = 4;
        }
        if (k + (size_t)n + 1 > cap) break;
        memcpy(out + k, b, (size_t)n);
        k += (size_t)n;
    }
    if (cap) out[k] = '\0';
}

int ubinmap_gpt(struct ubinmap_ctx *c) {
    if (c->src->size < 1024 || !has_signature(c)) return 0;
    c->be = 0;
    uint64_t sector = 0;
    char sig[8];
    if (!bm_read(c, 512, sig, 8) && !memcmp(sig, "EFI PART", 8)) sector = 512;
    else if (!bm_read(c, 4096, sig, 8) && !memcmp(sig, "EFI PART", 8)) sector = 4096;
    if (!sector) return 0;
    uint8_t h[92];
    if (bm_read(c, sector, h, 92)) return 0;
    bm_add(c, 0, 0, 440, 1, "Protective MBR", "boot code");
    int prot;
    partition_table(c, &prot);
    bm_add(c, 0, 510, 2, 0, "Signature", "55 AA");
    if (!prot) bm_note(c, "the MBR has no 0xEE entry protecting the GPT");

    uint32_t hsize = bm_get32(c, h + 12), nent = bm_get32(c, h + 80), esize = bm_get32(c, h + 84);
    uint64_t first = bm_get64(c, h + 40), last = bm_get64(c, h + 48), elba = bm_get64(c, h + 72);
    char guid[40];
    bm_guid(h + 56, guid, sizeof guid);
    bm_add(c, 0, sector, hsize < 92 ? 92 : hsize, 4, "GPT header", "LBA 1");
    bm_add(c, 1, sector, 8, 0, "Signature", "EFI PART");
    bm_add(c, 1, sector + 8, 4, 0, "Revision", "%u.%u", bm_get16(c, h + 10), bm_get16(c, h + 8));
    bm_add(c, 1, sector + 12, 4, 0, "Header size", "%u", (unsigned)hsize);
    bm_add(c, 1, sector + 16, 4, 0, "Header CRC32", "0x%08X", (unsigned)bm_get32(c, h + 16));
    bm_add(c, 1, sector + 24, 8, 0, "This header", "LBA %llu", (unsigned long long)bm_get64(c, h + 24));
    bm_add(c, 1, sector + 32, 8, 0, "Backup header", "LBA %llu", (unsigned long long)bm_get64(c, h + 32));
    bm_add(c, 1, sector + 40, 8, 0, "First usable", "LBA %llu", (unsigned long long)first);
    bm_add(c, 1, sector + 48, 8, 0, "Last usable", "LBA %llu", (unsigned long long)last);
    bm_add(c, 1, sector + 56, 16, 0, "Disk GUID", "%s", guid);
    bm_add(c, 1, sector + 72, 8, 0, "Entries at", "LBA %llu", (unsigned long long)elba);
    bm_add(c, 1, sector + 80, 4, 0, "Entries", "%u", (unsigned)nent);
    bm_add(c, 1, sector + 84, 4, 0, "Entry size", "%u", (unsigned)esize);
    bm_add(c, 1, sector + 88, 4, 0, "Entries CRC32", "0x%08X", (unsigned)bm_get32(c, h + 88));

    if (esize < 128 || esize > 4096 || esize % 8) {
        bm_note(c, "an entry size of %u is not a GPT's", (unsigned)esize);
        bm_format(c, "GPT disk, damaged");
        return 1;
    }
    uint64_t at = elba <= UINT64_MAX / sector ? elba * sector : UINT64_MAX;
    uint64_t fit = at < c->src->size ? (c->src->size - at) / esize : 0;
    if (nent > fit) bm_note(c, "the header claims %u entries; the file holds %llu", (unsigned)nent,
                            (unsigned long long)fit);
    uint64_t n = nent < fit ? nent : fit;
    if (n > GPT_SCAN_MAX) n = GPT_SCAN_MAX;
    if (bm_add(c, 0, at, (uint64_t)nent * esize, 0, "Partition entries", "%u x %u", (unsigned)nent,
               (unsigned)esize) < 0)
        return 1;
    int used = 0;
    for (uint64_t i = 0; i < n && used < GPT_LIST_MAX && !bm_full(c); i++) {
        uint8_t e[128];
        uint64_t ea = at + i * esize;
        if (bm_read(c, ea, e, 128)) break;
        static const uint8_t zero[16];
        if (!memcmp(e, zero, 16)) continue;   // an unused entry
        char type[40], uniq[40], name[112], label[112], size[32];
        bm_guid(e, type, sizeof type);
        bm_guid(e + 16, uniq, sizeof uniq);
        utf16_name(e + 56, name, sizeof name);
        uint64_t lo = bm_get64(c, e + 32), hi = bm_get64(c, e + 40);
        bm_human(hi >= lo && hi - lo < UINT64_MAX / sector ? (hi - lo + 1) * sector : 0, size, sizeof size);
        if (name[0]) snprintf(label, sizeof label, "%s", name);
        else snprintf(label, sizeof label, "Partition %llu", (unsigned long long)i + 1);
        bm_add(c, 1, ea, esize, 2 + (used & 1), label, "%s", gpt_type(type));
        bm_add(c, 2, ea, 16, 0, "Type", "%s", type);
        bm_add(c, 2, ea + 16, 16, 0, "Unique GUID", "%s", uniq);
        bm_add(c, 2, ea + 32, 8, 0, "First LBA", "%llu", (unsigned long long)lo);
        bm_add(c, 2, ea + 40, 8, 0, "Last LBA", "%llu", (unsigned long long)hi);
        bm_add(c, 2, ea + 40, 0, 0, "Size", "%s", size);
        bm_add(c, 2, ea + 48, 8, 0, "Attributes", "0x%llX", (unsigned long long)bm_get64(c, e + 48));
        bm_add(c, 2, ea + 56, 72, 0, "Name", "%s", name);
        used++;
    }
    bm_format(c, "GPT disk, %d partition%s", used, used == 1 ? "" : "s");
    return 1;
}

// --- FAT ------------------------------------------------------------------

static int pow2(unsigned v) { return v && !(v & (v - 1)); }

// A BPB's text field, its space padding cut.
static void field(const uint8_t *p, size_t n, char *out, size_t cap) {
    bm_ascii(p, n, out, cap);
    for (size_t k = strlen(out); k > 0 && out[k - 1] == ' '; k--) out[k - 1] = '\0';
}

int ubinmap_fat(struct ubinmap_ctx *c) {
    uint8_t b[90];
    if (c->src->size < 512 || !has_signature(c) || bm_read(c, 0, b, 90)) return 0;
    if (!(b[0] == 0xEB && b[2] == 0x90) && b[0] != 0xE9) return 0;
    c->be = 0;
    unsigned bps = bm_get16(c, b + 11), spc = b[13], rsvd = bm_get16(c, b + 14), fats = b[16];
    unsigned roots = bm_get16(c, b + 17), tot16 = bm_get16(c, b + 19), spf16 = bm_get16(c, b + 22);
    uint32_t tot32 = bm_get32(c, b + 32), spf32 = bm_get32(c, b + 36);
    if (!pow2(bps) || bps < 512 || bps > 4096 || !pow2(spc) || !rsvd || !fats || fats > 4) return 0;
    uint64_t total = tot16 ? tot16 : tot32, spf = spf16 ? spf16 : spf32;
    uint64_t root_secs = ((uint64_t)roots * 32 + bps - 1) / bps;
    uint64_t meta = rsvd + (uint64_t)fats * spf + root_secs;
    uint64_t clusters = total > meta ? (total - meta) / spc : 0;
    int bits = spf16 == 0 ? 32 : clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    char oem[9], label[12], fstype[9];
    field(b + 3, 8, oem, sizeof oem);
    bm_format(c, "FAT%d volume", bits);

    bm_add(c, 0, 0, 3, 1, "Jump", "%02X %02X %02X", b[0], b[1], b[2]);
    bm_add(c, 0, 3, 8, 0, "OEM name", "%s", oem);
    bm_add(c, 0, 11, 25, 2, "BIOS parameter block", "%u-byte sectors", bps);
    bm_add(c, 1, 11, 2, 0, "Bytes per sector", "%u", bps);
    bm_add(c, 1, 13, 1, 0, "Sectors per cluster", "%u", spc);
    bm_add(c, 1, 14, 2, 0, "Reserved sectors", "%u", rsvd);
    bm_add(c, 1, 16, 1, 0, "FATs", "%u", fats);
    bm_add(c, 1, 17, 2, 0, "Root entries", "%u", roots);
    bm_add(c, 1, 19, 2, 0, "Sectors (16-bit)", "%u", tot16);
    bm_add(c, 1, 21, 1, 0, "Media", "0x%02X", b[21]);
    bm_add(c, 1, 22, 2, 0, "Sectors per FAT", "%u", spf16);
    bm_add(c, 1, 24, 2, 0, "Sectors per track", "%u", bm_get16(c, b + 24));
    bm_add(c, 1, 26, 2, 0, "Heads", "%u", bm_get16(c, b + 26));
    bm_add(c, 1, 28, 4, 0, "Hidden sectors", "%u", (unsigned)bm_get32(c, b + 28));
    bm_add(c, 1, 32, 4, 0, "Sectors (32-bit)", "%u", (unsigned)tot32);
    if (bits == 32) {
        field(b + 71, 11, label, sizeof label);
        field(b + 82, 8, fstype, sizeof fstype);
        bm_add(c, 0, 36, 54, 3, "FAT32 extension", "%s", label);
        bm_add(c, 1, 36, 4, 0, "Sectors per FAT", "%u", (unsigned)spf32);
        bm_add(c, 1, 44, 4, 0, "Root cluster", "%u", (unsigned)bm_get32(c, b + 44));
        bm_add(c, 1, 48, 2, 0, "FSInfo sector", "%u", bm_get16(c, b + 48));
        bm_add(c, 1, 50, 2, 0, "Backup boot sector", "%u", bm_get16(c, b + 50));
        bm_add(c, 1, 67, 4, 0, "Volume ID", "%08X", (unsigned)bm_get32(c, b + 67));
        bm_add(c, 1, 71, 11, 0, "Label", "%s", label);
        bm_add(c, 1, 82, 8, 0, "File system", "%s", fstype);
        bm_add(c, 0, 90, 420, 4, "Boot code", "420 bytes");
    } else {
        field(b + 43, 11, label, sizeof label);
        field(b + 54, 8, fstype, sizeof fstype);
        bm_add(c, 0, 36, 26, 3, "FAT12/16 extension", "%s", label);
        bm_add(c, 1, 36, 1, 0, "Drive", "0x%02X", b[36]);
        bm_add(c, 1, 39, 4, 0, "Volume ID", "%08X", (unsigned)bm_get32(c, b + 39));
        bm_add(c, 1, 43, 11, 0, "Label", "%s", label);
        bm_add(c, 1, 54, 8, 0, "File system", "%s", fstype);
        bm_add(c, 0, 62, 448, 4, "Boot code", "448 bytes");
    }
    bm_add(c, 0, 510, 2, 0, "Signature", "55 AA");
    // The regions the BPB lays out, as far as the file holds them.
    uint64_t at = (uint64_t)rsvd * bps;
    bm_add(c, 0, 0, at, 0, "Reserved sectors", "%u", rsvd);
    for (unsigned f = 0; f < fats && !bm_full(c); f++) {
        char nm[16];
        snprintf(nm, sizeof nm, "FAT %u", f + 1);
        bm_add(c, 0, at, spf * bps, 5 + (int)(f & 1), nm, "%llu sectors", (unsigned long long)spf);
        at += spf * bps;
    }
    if (root_secs) {
        bm_add(c, 0, at, root_secs * bps, 7, "Root directory", "%u entries", roots);
        at += root_secs * bps;
    }
    bm_add(c, 0, at, total > meta ? (total - meta) * bps : 0, 0, "Data area", "%llu clusters",
           (unsigned long long)clusters);
    return 1;
}
