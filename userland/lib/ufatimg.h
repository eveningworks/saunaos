#ifndef ULIB_UFATIMG_H
#define ULIB_UFATIMG_H

#include <stdint.h>
#include <stddef.h>

// The files on a FAT12 or FAT16 disk IMAGE held in memory -- the 1.44 MB
// floppies (and DMF's 1.68 MB) Windows 3.x and DOS shipped on, read so
// w16's installer can take `Disk01.img` as it is. Read-only, 8.3 names
// (long-name entries are skipped), subdirectories by their first cluster.
//
// Not the kernel's FAT32 driver, which mounts a block device: this reads
// a file somebody downloaded, in a sandboxed process, and nothing about
// it needs a mount.
//
// A PARSER HERE REJECTS rather than guesses: the boot sector's numbers
// must describe a FAT12 or FAT16 volume that fits the image, every
// cluster is checked against the volume, a chain that loops or ends
// before the file does is -EINVAL, and nothing is read past `n`.

struct ufatimg {
    const uint8_t *img;
    size_t n;
    uint32_t bps, spc;           // bytes per sector, sectors per cluster
    uint32_t fat_at, fat_bytes;  // the first FAT
    uint32_t root_at, root_entries;
    uint32_t data_at;            // cluster 2's first byte
    uint32_t clusters;           // data clusters, numbered 2 .. clusters + 1
    int fat16;
    char label[12];              // the volume label, "" when none
};

struct ufatimg_ent {
    char name[13];               // "NAME.EXT"
    uint8_t attr;                // 0x10 a directory
    uint32_t size;
    uint32_t cluster;            // the first cluster; 0 for an empty file
};

#define UFATIMG_DIR 0x10

// 0, or -EINVAL for an image that is not a FAT12/FAT16 volume this reads.
int ufatimg_open(struct ufatimg *v, const uint8_t *img, size_t n);

// Each file and directory in the directory starting at `dir` (0 = the
// root), skipping deleted entries, long-name pieces, the volume label and
// "." / "..". `cb` returns non-zero to stop. The count, or -EINVAL.
int ufatimg_list(const struct ufatimg *v, uint32_t dir,
                 int (*cb)(void *ctx, const struct ufatimg_ent *e), void *ctx);

// The entry named `name` (no case) in `dir`: 0, -ENOENT, or -EINVAL.
int ufatimg_find(const struct ufatimg *v, uint32_t dir, const char *name, struct ufatimg_ent *out);

// A file's bytes through `out`, a cluster at a time: 0, -EINVAL for a
// chain that loops, leaves the volume or ends before the file does, -EIO
// when `out` returns non-zero.
int ufatimg_read(const struct ufatimg *v, const struct ufatimg_ent *e,
                 int (*out)(void *ctx, const uint8_t *data, size_t n), void *ctx);

#endif
