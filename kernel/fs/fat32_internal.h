// FAT32's internals, shared by the driver's two files:
//
//   fat32.c         the filesystem: the FAT, directories, files, format
//   fat32_resize.c  growing or shrinking a volume nothing has mounted (SYS_FS_RESIZE)
//
// fat32.c's top comment has the format's decisions; nothing outside
// kernel/fs/ includes this (fat32.h is the backend's public face).
#ifndef FAT32_INTERNAL_H
#define FAT32_INTERNAL_H

#include <stdint.h>
#include "block.h"
#include "fs.h"
#include "fs_ops.h"

#define SECTOR 512u

// FAT special values, after masking off the top four bits (which are
// reserved and must be PRESERVED on write -- a driver that writes a
// full 32-bit value clobbers them).
#define FAT_MASK      0x0FFFFFFFu
#define FAT_FREE      0x00000000u
#define FAT_BAD       0x0FFFFFF7u
#define FAT_EOC_MIN   0x0FFFFFF8u
#define FAT_EOC       0x0FFFFFFFu

// The most clusters a FAT32 volume is given: mkfs.fat's MAX_CLUST_32,
// a little under the spec's 0x0FFFFFF5 so no cluster number reaches the
// reserved values from 0x0FFFFFF7 (bad) up.
#define FAT32_MAX_CLUSTERS ((1u << 28) - 16u)

// ---- state ----------------------------------------------------------
//
// PER MOUNT, in struct fat32_state, and handed as `sbi` to every
// function that touches a volume (fs_ops.h). The scratch sectors in it
// each have a SINGLE purpose, deliberately: one general-purpose scratch
// shared between a directory walk and the FAT lookup that walk makes
// would corrupt the walk halfway through, which is the exact shape of
// the bug tfs3.c's blk/ptr_blk split exists to prevent.

struct fat_vol {
    const struct block_device *dev;
    int mounted;
    uint32_t k;                 // 512-byte sectors per FAT sector (bytes_per_sector / 512)
    uint32_t sectors_per_cluster;
    uint32_t reserved;          // sectors before the first FAT
    uint32_t num_fats;
    uint32_t fat_sectors;       // per FAT
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t first_data_sector;
    uint32_t cluster_count;     // data clusters, so the last is cluster_count + 1
    uint32_t fsinfo_sector;     // 0 when the BPB names none
    uint32_t free_hint;         // FSInfo's "next free" hint; 0xFFFFFFFF = none
    uint32_t free_count;        // FSInfo's free count; 0xFFFFFFFF = unknown
};

struct fat32_state {
    struct fat_vol v;

    // The cached FAT sector. IN HERE rather than beside the scratch
    // because it can be DIRTY across calls, and a dirty sector written
    // back to whichever volume happens to be current is corruption.
    uint8_t fatsec[SECTOR];
    uint32_t fatsec_lba;        // which one, 0 = nothing cached
    int fatsec_dirty;

    // Per-call scratch, per MOUNT because one call runs per volume at a
    // time (its lock) and two volumes' calls may run at once.
    uint8_t dirsec[SECTOR];     // a directory sector during a walk
    uint8_t datasec[SECTOR];    // file data for a partial-sector read/write
    uint8_t tmpsec[SECTOR];     // probe, format, FSInfo -- never held across a walk
};

// ---- little-endian accessors ----------------------------------------

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// fat32.c
//
// Reads the BPB on `dev` into sbi->v (every field in 512-byte sectors):
// 1 a FAT32 this driver serves, 0 not one, -1 unreadable.
int fat32_parse_bpb(struct fat32_state *sbi, const struct block_device *dev);
// Every cluster's FAT entry from 2 up, in big reads; `fn` returning
// non-zero stops the walk and is returned. 0 when all were seen, -1 when
// a read failed.
int fat32_scan(struct fat32_state *sbi, int (*fn)(void *ctx, uint32_t clus, uint32_t val), void *ctx);

// fat32_resize.c
//
// What a grow to `want` sectors would make of the volume `v` describes:
// the new size, FAT size, data start and cluster count, and whether the
// data region MOVES (its FATs had to grow). 1 for a grow, 0 when `want`
// gains no cluster. Pure arithmetic, for the KTEST.
struct fat_grow_plan {
    uint32_t total, fat_sectors, first_data, clusters;
    int moves;
};
int fat32_grow_plan(const struct fat_vol *v, uint64_t want, struct fat_grow_plan *out);
int fat32_resize(void *st, const struct block_device *dev, uint64_t sectors,
                 struct fs_resize_info *out);

#endif
