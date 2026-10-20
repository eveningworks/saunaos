#ifndef ULIB_UDISKSCAN_H
#define ULIB_UDISKSCAN_H

// What is on this machine's disks, read in one pass: every whole disk
// (model, size, table kind), its partitions (where, what role, which
// block device), what is mounted on each and how full, and the volumes
// that live in memory. Disks' drive list and the installer's disk
// chooser, so the two describe a disk the same way -- lsblk's and
// GNOME Disks' udisks shape, from QUERY_BLKDEV, QUERY_PARTTABLE,
// QUERY_PARTITION and QUERY_FSINFO.
//
// The SYSTEM disk (the root's, or one carrying a partition the root is
// on) comes first. A live session's RAM image is a disk too, with
// `persistent` 0.

#include <stddef.h>
#include <stdint.h>

#define UDISK_MAX       8
#define UDISK_PARTS_MAX 16
#define UDISK_RAM_MAX   6
#define UDISK_MIB       2048ull     // sectors
#define UDISK_GPT_TAIL  34          // the backup header and entries at the end

enum { UDISK_ROLE_DATA, UDISK_ROLE_ESP, UDISK_ROLE_BIOS, UDISK_ROLE_OTHER };

struct udisk_part {
    int number;                    // 1-based slot in the table; 0 = a whole-disk volume
    uint64_t start, count;         // 512-byte sectors
    int mbr_type, role;
    uint8_t type_guid[16];
    char name[40];
    char dev[16];                  // its block device, "" when it has none yet
    char point[64], fs[16];        // where it is mounted, and as what; "" when not
    // What its signature says it holds, mounted or not (QUERY_BLKDEV's
    // fstype/label): "ntfs", "Backup". `readable`: this system can mount it.
    char type[16], label[64];
    int readable;
    uint64_t used, total;
    int root;
};

struct udisk {
    char name[16], model[48], driver[16];
    uint64_t sectors, block_size;
    int kind;                      // QUERY_PART_*
    uint8_t guid[16];
    int system, persistent;
    int nparts;
    struct udisk_part parts[UDISK_PARTS_MAX];
};

struct udisk_ram { char point[64], fs[16]; uint64_t used, total; };

struct udisk_scan {
    struct udisk disks[UDISK_MAX];
    int ndisks;
    struct udisk_ram ram[UDISK_RAM_MAX];
    int nram;
};

// Fill `s` from the kernel's tables. ~10 KB: static, or the heap.
void udisk_scan(struct udisk_scan *s);
struct udisk *udisk_named(struct udisk_scan *s, const char *name);

// Partitions in disk order (sorts `d` in place).
void udisk_sort(struct udisk *d);

// Is the root a LIVE image -- mounted from a RAM disk GRUB loaded, so
// nothing written survives power off? A ramfs root with no device is not
// one: that is a machine whose disk failed to mount.
int udisk_live_root(void);

// "EFI", "BIOS boot", or NULL for a data partition.
const char *udisk_role_name(const struct udisk_part *p);
// What a person calls the volume: its filesystem's label, else what the
// table calls it (udisk_part_name()).
void udisk_part_title(const struct udisk_part *p, char *out, size_t cap);
// The table's name for it: the GPT name, else its role, else "Partition 3";
// "Whole disk" for a volume with no table.
void udisk_part_name(const struct udisk_part *p, char *out, size_t cap);
// A filesystem type as a person reads it ("ntfs" -> "NTFS"); an unknown
// one as it came.
const char *udisk_fs_title(const char *type);
// The filesystem it holds, for display: as mounted, else as recognised;
// "" for neither.
const char *udisk_part_fs(const struct udisk_part *p);

#endif
