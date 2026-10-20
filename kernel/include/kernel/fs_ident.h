#ifndef KERNEL_FS_IDENT_H
#define KERNEL_FS_IDENT_H

// WHICH FILESYSTEM A VOLUME HOLDS, BY ITS SIGNATURE -- libblkid's job
// (what `lsblk -f` and GNOME Disks show), done in the kernel because ring
// 3 has no raw block access. It names filesystems toy-os cannot mount
// (NTFS, ext4, ...) as well as its own, and reads nothing but a handful
// of fixed sectors: a recogniser, not a mount, and never written to.
//
// A signature that is there but fails its sanity checks is NOT reported:
// "unknown" is an honest answer, a wrong name is not.

#include <stdint.h>

struct block_device;

struct fs_ident {
    char type[16];   // blkid's id: "ntfs", "exfat", "ext4", "tfs3", "fat32"; "" = unknown
    char label[64];  // UTF-8, trailing blanks trimmed; "" when it has none
};

// Reads `len` (<= 4096) bytes at byte offset `off` of the volume; 1 on
// success. The seam the KTEST drives with an in-memory image.
typedef int (*fs_ident_read_fn)(void *ctx, uint64_t off, uint32_t len, void *buf);

// 1 and `out` filled when a signature matched, 0 otherwise. `bytes` is the
// volume's size, so nothing past its end is asked for.
int fs_ident_read(fs_ident_read_fn rd, void *ctx, uint64_t bytes, struct fs_ident *out);
int fs_ident(const struct block_device *dev, struct fs_ident *out);

#endif
