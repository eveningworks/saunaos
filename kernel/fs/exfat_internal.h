#ifndef EXFAT_INTERNAL_H
#define EXFAT_INTERNAL_H

// exFAT's state and helpers shared by its three files: exfat.c (mount,
// lookup, reading), exfat_write.c (allocation and every mutating op) and
// exfat_format.c (format, wipe, the up-case table it writes). Split by
// concern, userland/wm/'s rule. The format is Microsoft's published
// exFAT specification (2019); section numbers below are its.
#include <stdint.h>
#include "block.h"
#include "fs.h"
#include "fs_ops.h"

#define EXFAT_EOC       0xFFFFFFFFu
#define EXFAT_BAD       0xFFFFFFF7u

// Directory entry types (7.1): bit 7 InUse, bit 6 secondary, 5 benign.
#define ET_END          0x00
#define ET_BITMAP       0x81
#define ET_UPCASE       0x82
#define ET_LABEL        0x83
#define ET_FILE         0x85
#define ET_GUID         0xA0
#define ET_STREAM       0xC0
#define ET_NAME         0xC1
#define ET_INUSE        0x80

#define ATTR_RO         0x01
#define ATTR_DIR        0x10
#define ATTR_ARCHIVE    0x20

#define SF_ALLOC        0x01   // GeneralSecondaryFlags: AllocationPossible
#define SF_NOFATCHAIN   0x02   // the clusters are contiguous; the FAT says nothing

#define NAME_MAX16      255    // UTF-16 code units in a name (7.7)
#define NAME_PER_ENTRY  15
#define SET_MAX         (2 + (NAME_MAX16 + NAME_PER_ENTRY - 1) / NAME_PER_ENTRY)   // 19 entries
#define NAME8_MAX       (NAME_MAX16 * 3 + 1)   // a BMP code unit is at most 3 bytes of UTF-8

// A cluster run as a file or directory knows it (7.6). `first` 0 means
// no clusters at all. A directory's `length` is its allocated size; the
// root has none in any entry, so its length is its chain's.
struct exf_stream {
    uint32_t first;
    int nofat;            // SF_NOFATCHAIN
    uint64_t length;      // DataLength
    uint64_t valid;       // ValidDataLength: past it, reads are zeros
    // A cursor, so walking a FAT chain front to back is not quadratic.
    uint32_t cur_idx, cur_clus;
};

// Where a file's entry set lives: its parent directory's stream and the
// index of its File entry in it (32 bytes each).
struct exf_loc {
    struct exf_stream dir;
    uint32_t index;
    int count;            // entries in the set, File included
};

struct exf_node {
    int is_dir;
    uint16_t attr;
    struct exf_stream s;
    uint32_t crt, mod;    // exFAT timestamps (7.4.8)
    uint8_t crt_10ms, mod_10ms, crt_utc, mod_utc;
    struct exf_loc loc;   // index 0 / count 0 for the root
};

struct exfat_state {
    const struct block_device *dev;
    int mounted, writable;
    uint32_t bps, bps_shift;          // bytes per volume sector, 512..4096
    uint32_t spc_shift;               // sectors per cluster, as a shift
    uint32_t cluster_bytes;
    uint32_t dev_per_sec;             // 512-byte device units per volume sector
    uint64_t vol_sectors;
    uint32_t fat_offset, fat_length;  // in volume sectors
    uint32_t heap_offset;
    uint32_t cluster_count;
    uint32_t root;
    uint32_t serial;
    uint16_t vol_flags;
    // VolumeDirty (3.1.13.2): set before the FIRST write, so a read-only
    // mount writes nothing, and cleared at unmount only when this mount
    // set it -- a volume dirty on arrival stays dirty for a real check.
    // `boot` is sector 0's own buffer: exf_write_sector() marks the
    // volume from inside a write whose data may be in `sec`.
    uint8_t *boot;
    int was_dirty, dirtied;
    // The allocation bitmap, whole in memory, and where it lives on disk.
    struct exf_stream bitmap_s;
    uint8_t *bitmap;
    uint32_t bitmap_bytes;
    uint32_t free_clusters, alloc_hint;
    // The up-case table, expanded to every BMP code unit.
    uint16_t *upcase;
    // One-sector caches and per-call scratch, per mount: a backend call
    // runs under this mount's lock, and kernel frames are 1 KiB.
    uint8_t *sec;   uint64_t sec_lba;  int sec_valid;
    uint8_t *fsec;  uint64_t fsec_lba; int fsec_valid;
    uint8_t set[SET_MAX * 32];         // the set a lookup last read
    uint8_t nset[SET_MAX * 32];        // a set being built to write
    // store()'s own: a directory growing in the middle of a create
    // rewrites ITS set while the new file's waits in nset.
    uint8_t uset[SET_MAX * 32];
    uint16_t name16[NAME_MAX16 + 1];
    uint16_t want16[NAME_MAX16 + 1];   // exf_find()'s target name
    char name8[NAME8_MAX];
    char path[FS_PATH_MAX + 1];        // exf_parent()'s parent path
};

// ---- exfat.c ----------------------------------------------------------
int exf_read_sector(struct exfat_state *x, uint64_t sector, void *buf);
int exf_write_sector(struct exfat_state *x, uint64_t sector, const void *buf);
uint64_t exf_cluster_sector(struct exfat_state *x, uint32_t clus);
int exf_cluster_ok(struct exfat_state *x, uint32_t clus);
int exf_fat_get(struct exfat_state *x, uint32_t clus, uint32_t *out);
int exf_fat_set(struct exfat_state *x, uint32_t clus, uint32_t val);
int exf_fat_flush(struct exfat_state *x);
// The cluster holding byte `off` of a stream, or 0.
uint32_t exf_stream_cluster(struct exfat_state *x, struct exf_stream *s, uint64_t off);
uint32_t exf_stream_clusters(struct exfat_state *x, const struct exf_stream *s);
int exf_dir_entry(struct exfat_state *x, struct exf_stream *dir, uint32_t index, uint8_t *e32);
int exf_dir_put(struct exfat_state *x, struct exf_stream *dir, uint32_t index, const uint8_t *e32);
// A whole entry set, ONE write per sector it covers, the File entry's
// sector last -- so a pull mid-update never leaves a set whose checksum
// is stale (which a reader drops, file and all).
int exf_dir_put_set(struct exfat_state *x, struct exf_stream *dir, uint32_t index,
                    const uint8_t *set, int count);
uint16_t exf_set_checksum(const uint8_t *set, int count);
uint16_t exf_name_hash(struct exfat_state *x, const uint16_t *name, int len);
int exf_lookup(struct exfat_state *x, const char *path, struct exf_node *out);
int exf_parent(struct exfat_state *x, const char *path, struct exf_node *dir, const char **leaf);
int exf_find(struct exfat_state *x, struct exf_stream *dir, const char *name, struct exf_node *out);
int exf_utf8_to16(const char *in, uint16_t *out, int cap);
void exf_node_from_set(struct exf_node *n, const uint8_t *set);
uint32_t exf_now(uint8_t *ten_ms, uint8_t *utc);
uint32_t exf_le32(const uint8_t *p);
uint64_t exf_le64(const uint8_t *p);
void exf_put16(uint8_t *p, uint16_t v);
void exf_put32(uint8_t *p, uint32_t v);
void exf_put64(uint8_t *p, uint64_t v);
uint32_t exf_boot_checksum(const uint8_t *region, uint32_t bps);
uint32_t exf_table_checksum(const uint8_t *data, uint64_t len);
int exf_set_dirty(struct exfat_state *x, int dirty);

// ---- exfat_write.c ------------------------------------------------------
int exfat_touch(void *st, const char *path);
int exfat_create_excl(void *st, const char *path);
int exfat_write(void *st, const char *path, const char *data, int append);
int exfat_mkdir(void *st, const char *path);
int exfat_del(void *st, const char *path);
int exfat_write_range(void *st, const char *path, uint64_t offset, const void *buf, uint32_t len);
int exfat_rename(void *st, const char *oldpath, const char *newpath);
int exfat_truncate(void *st, const char *path, uint64_t size);

// ---- exfat_format.c ------------------------------------------------------
int exfat_format(void *st, const struct block_device *dev);
int exfat_wipe(void *st, const struct block_device *dev);

#endif
