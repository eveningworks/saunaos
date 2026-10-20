// Tests for the filesystem recogniser (kernel/fs/fs_ident.c), over
// hand-built images in memory: each carries only the bytes the real
// filesystem's own tools put where its signature lives, so a test fails
// on an offset or a check that is wrong, not on a mkfs it cannot run.
#include "ktest.h"
#include "fs_ident.h"
#include "heap.h"
#include "string.h"

#define IMG_BYTES (128u * 1024u)

static uint8_t *g_img;

static int mem_read(void *ctx, uint64_t off, uint32_t len, void *buf) {
    (void)ctx;
    if (off + len > IMG_BYTES) return 0;
    k_memcpy(buf, g_img + off, len);
    return 1;
}

static int fresh(void) {
    if (!g_img) g_img = kmalloc(IMG_BYTES);
    if (g_img) k_memset(g_img, 0, IMG_BYTES);
    return g_img != 0;
}

static void w16(uint32_t off, uint16_t v) { g_img[off] = (uint8_t)v; g_img[off + 1] = (uint8_t)(v >> 8); }
static void w32(uint32_t off, uint32_t v) { w16(off, (uint16_t)v); w16(off + 2, (uint16_t)(v >> 16)); }
static void put(uint32_t off, const char *s) { k_memcpy(g_img + off, s, k_strlen(s)); }
static void put16(uint32_t off, const char *s) { for (; *s; s++, off += 2) w16(off, (uint8_t)*s); }

static int ident(struct fs_ident *id) { return fs_ident_read(mem_read, 0, IMG_BYTES, id); }

// An NTFS boot sector (512-byte sectors, 4 KiB clusters, 1 KiB records,
// the MFT at cluster 4) and $Volume's record carrying `label`.
static void ntfs(const char *label) {
    put(3, "NTFS    ");
    w16(11, 512);
    g_img[13] = 8;
    w32(0x30, 4);                  // MFT at cluster 4 = byte 16384
    g_img[0x40] = (uint8_t)-10;    // 2^10-byte records
    uint32_t r = 16384 + 3 * 1024;
    put(r, "FILE");
    w16(r + 4, 0x30);              // update sequence at 0x30, two strides + the check
    w16(r + 6, 3);
    w16(r + 0x30, 0xbeef);
    w16(r + 0x32, 0x1111);         // the real bytes at the ends of the strides
    w16(r + 0x34, 0x2222);
    w16(r + 510, 0xbeef);
    w16(r + 1022, 0xbeef);
    w16(r + 0x14, 0x38);           // first attribute
    uint32_t a = r + 0x38, n = (uint32_t)k_strlen(label);
    w32(a, 0x60);                  // VOLUME_NAME
    w32(a + 4, 24 + 2 * n + 8);
    g_img[a + 8] = 0;              // resident
    w32(a + 0x10, 2 * n);
    w16(a + 0x14, 24);
    put16(a + 24, label);
    w32(a + 24 + 2 * n + 8, 0xffffffffu);
}

KTEST("fsident", "NTFS is named, and its label read out of $Volume through the fixups") {
    struct fs_ident id;
    KTEST_ASSERT(fresh());
    ntfs("Backup");
    KTEST_ASSERT(ident(&id));
    KTEST_ASSERT(!k_strcmp(id.type, "ntfs"));
    KTEST_ASSERT(!k_strcmp(id.label, "Backup"));
}

KTEST("fsident", "a torn NTFS record gives the type and NO label, never a wrong one") {
    struct fs_ident id;
    KTEST_ASSERT(fresh());
    ntfs("Backup");
    w16(16384 + 3 * 1024 + 1022, 0x0bad);   // the second stride's check fails
    KTEST_ASSERT(ident(&id));
    KTEST_ASSERT(!k_strcmp(id.type, "ntfs"));
    KTEST_ASSERT_EQ(id.label[0], 0);
}

KTEST("fsident", "tfs3 wins over an NTFS boot sector it never overwrote") {
    struct fs_ident id;
    KTEST_ASSERT(fresh());
    ntfs("Old");
    put(32768, "TFS3");
    g_img[32768 + 4] = 2;
    w32(32768 + 44, k_fnv1a(g_img + 32768, 44));
    KTEST_ASSERT(ident(&id));
    KTEST_ASSERT(!k_strcmp(id.type, "tfs3"));
    // ...and a superblock whose checksum fails is not tfs3.
    g_img[32768 + 8] ^= 1;
    KTEST_ASSERT(ident(&id));
    KTEST_ASSERT(!k_strcmp(id.type, "ntfs"));
}

KTEST("fsident", "ext2, ext3 and ext4 by their features, with the label") {
    struct fs_ident id;
    KTEST_ASSERT(fresh());
    w16(1024 + 56, 0xef53);
    put(1024 + 120, "data");
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.type, "ext2") && !k_strcmp(id.label, "data"));
    w32(1024 + 92, 0x4);                      // has_journal
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.type, "ext3"));
    w32(1024 + 96, 0x40);                     // extents
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.type, "ext4"));
}

KTEST("fsident", "exFAT's label comes from the root directory's label entry") {
    struct fs_ident id;
    KTEST_ASSERT(fresh());
    put(3, "EXFAT   ");
    g_img[108] = 9;                // 512-byte sectors
    g_img[109] = 3;                // 8 per cluster
    w32(88, 64);                   // the heap at sector 64
    w32(96, 4);                    // root at cluster 4 -> sector 64 + 16
    uint32_t e = (64 + 16) * 512 + 32;
    g_img[e - 32] = 0x81;          // an allocation bitmap entry ahead of it
    g_img[e] = 0x83;
    g_img[e + 1] = 5;
    put16(e + 2, "Stick");
    KTEST_ASSERT(ident(&id));
    KTEST_ASSERT(!k_strcmp(id.type, "exfat"));
    KTEST_ASSERT(!k_strcmp(id.label, "Stick"));
}

KTEST("fsident", "FAT32 and FAT16 by their BPB; NO NAME is no label") {
    struct fs_ident id;
    KTEST_ASSERT(fresh());
    w16(11, 512); g_img[13] = 8; w16(14, 32); g_img[16] = 2;
    g_img[510] = 0x55; g_img[511] = 0xaa;
    put(82, "FAT32   ");
    put(71, "NO NAME    ");
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.type, "fat32"));
    KTEST_ASSERT_EQ(id.label[0], 0);
    put(71, "TOYOS      ");
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.label, "TOYOS"));
    KTEST_ASSERT(fresh());
    w16(11, 512); g_img[13] = 4; w16(14, 1); g_img[16] = 2; w16(22, 32);
    g_img[510] = 0x55; g_img[511] = 0xaa;
    put(54, "FAT16   ");
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.type, "fat16"));
    // A BPB that is not sane is not FAT, whatever the string says.
    g_img[16] = 7;
    KTEST_ASSERT(!ident(&id));
    KTEST_ASSERT_EQ(id.type[0], 0);
}

KTEST("fsident", "btrfs, XFS, swap and ISO 9660 by their magic, with labels") {
    struct fs_ident id;
    KTEST_ASSERT(fresh());
    put(65536 + 0x40, "_BHRfS_M");
    put(65536 + 0x12b, "pool");
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.type, "btrfs") && !k_strcmp(id.label, "pool"));
    KTEST_ASSERT(fresh());
    put(0, "XFSB");
    put(108, "home");
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.type, "xfs") && !k_strcmp(id.label, "home"));
    KTEST_ASSERT(fresh());
    put(4086, "SWAPSPACE2");
    w32(1024, 1);
    put(1024 + 28, "swap0");
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.type, "swap") && !k_strcmp(id.label, "swap0"));
    KTEST_ASSERT(fresh());
    g_img[32768] = 1;
    put(32769, "CD001");
    put(32768 + 40, "TOYOS_LIVE                      ");
    KTEST_ASSERT(ident(&id) && !k_strcmp(id.type, "iso9660") && !k_strcmp(id.label, "TOYOS_LIVE"));
}

KTEST("fsident", "a blank volume and an MBR are nothing") {
    struct fs_ident id;
    KTEST_ASSERT(fresh());
    KTEST_ASSERT(!ident(&id));
    g_img[510] = 0x55; g_img[511] = 0xaa;     // a boot signature with no BPB behind it
    g_img[446 + 4] = 0x83;                    // one MBR entry
    KTEST_ASSERT(!ident(&id));
}
