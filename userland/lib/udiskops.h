#ifndef ULIB_UDISKOPS_H
#define ULIB_UDISKOPS_H

// A BATCH OF DISK CHANGES, queued and then run in order: create, delete,
// resize and format, each on one partition -- GParted's and KDE Partition
// Manager's pending operations. Disks queues them, draws each disk as it
// will be (udiskops_project()), and Apply runs them one at a time
// (udiskops_run()), stopping at the first that fails.
//
// A PARTITION IS NAMED BY ITS DISK AND FIRST SECTOR, NEVER ITS DEVICE: a
// create rewrites the whole table (SYS_MKPART), which renumbers the
// partitions after it, and one an earlier step creates has no device
// until that step has run. No step moves a partition's start.
//
// EVERY STEP IS CHECKED AGAINST THE DISK AS THE STEPS BEFORE IT LEAVE IT,
// so "delete p2, grow p1 into the room" is only valid in that order --
// and a step removed takes with it every later one that no longer holds.

#include <stddef.h>
#include <stdint.h>
#include "partition_abi.h"
#include "lib/udiskscan.h"

enum { UDISKOP_CREATE = 1, UDISKOP_DELETE, UDISKOP_RESIZE, UDISKOP_FORMAT };

// A step's text, sized from what goes into it -- a device and a disk, a
// volume label (udisk_part's), a GPT name, and the words and sizes
// around them -- so it is never cut.
#define UDISKOP_WHAT_MAX (2 * MKPART_DEVICE_MAX + sizeof(((struct udisk_part *)0)->label) + MKPART_NAME_MAX + 96)

struct fs_resize_info;

struct udiskop {
    int kind;                      // UDISKOP_*
    char disk[16];                 // "usb1"
    uint64_t start;                // the partition's first sector
    uint64_t sectors;              // CREATE, RESIZE: its new length
    char fs[8];                    // FORMAT: "tfs3", "fat32"
    char name[MKPART_NAME_MAX];    // CREATE: the table's name for it
    int has_fs;                    // RESIZE: its filesystem grows with it
    int unsafe;                    // RESIZE: FS_RESIZE_UNSAFE, a filesystem with no journal
    char what[UDISKOP_WHAT_MAX];   // the step as a person reads it, made when it was queued
};

// Zero-initialised is empty; the array grows (lib/ugrow.h).
struct udiskops {
    struct udiskop *ops;
    int count, cap;
};

// What the queued steps do to one partition of a projected disk.
#define UDISKOP_MARK_NEW     0x1   // created by a step
#define UDISKOP_MARK_RESIZED 0x2
#define UDISKOP_MARK_FORMAT  0x4

// The verb for a step's kind: "Create", "Delete", "Resize", "Format".
const char *udiskop_verb(int kind);

// `real` as the queue's steps for it leave it, into `out`, in disk order;
// `marks` (UDISK_PARTS_MAX of them, or NULL) gets UDISKOP_MARK_* per
// partition of `out`. A step that no longer holds is skipped.
void udiskops_project(const struct udiskops *q, const struct udisk *real, struct udisk *out, int *marks);

// Why `op` cannot be done to `d` (a projection, with its marks), or NULL.
const char *udiskops_check(const struct udisk *d, const int *marks, const struct udiskop *op);

// Queue `op` on the disk `real` is, MERGED where a step on the same
// partition already says it: a second resize or format replaces the
// first (and a resize back to the size it has is no step), and a delete
// takes every step that partition had -- all of them, and queues
// nothing, when a step created it. The steps it replaces come out
// first, and it is checked against, described against and queued after
// the rest. Fills op->what. NULL, or why it was refused; `dropped` (may
// be NULL) gets how many OTHER steps no longer held and went with it.
const char *udiskops_add(struct udiskops *q, const struct udisk *real, struct udiskop *op, int *dropped);

// Remove step `i`, then every later step that no longer holds without it.
// The number of later steps removed with it.
int udiskops_remove(struct udiskops *q, struct udisk_scan *scan, int i);

// Drop the steps a fresh scan no longer allows (a partition mounted, a
// disk gone): the number dropped.
int udiskops_revalidate(struct udiskops *q, struct udisk_scan *scan);

// The steps queued on the disk `name`.
int udiskops_count_on(const struct udiskops *q, const char *name);

void udiskops_clear(struct udiskops *q);

// The first `n` steps RAN: off the queue, the rest NOT replayed -- the
// disk they ran on is not the scan's any more, so rescan, then
// udiskops_revalidate().
void udiskops_ran(struct udiskops *q, int n);

// RUN ONE STEP, on the disk as the kernel has it now (a fresh scan).
// `started`, when not NULL, is told the partition's device just before
// the call that blocks, so a caller can ask that device how far it has
// got (MKFS_PROGRESS, FSRESIZE_PROGRESS). 0, or -errno with `why`.
int udiskops_run(const struct udiskop *op, char *why, size_t cap,
                 void (*started)(void *ctx, const char *dev), void *ctx);

// RESIZE A PARTITION, THE TABLE FIRST AND THEN ITS FILESYSTEM: a grow cut
// off between the two leaves room the filesystem does not use yet, and
// the same resize again finishes it. `part resize` and Apply's Resize.
// `now` is its length, `want` the new one (equal: the filesystem only);
// `info` (may be NULL) gets the filesystem's answer. 0, or -errno with the
// reason in `why` and `*stage` 1 when the table refused, 2 the filesystem.
int udiskops_resize_part(const char *dev, uint64_t now, uint64_t want, int has_fs, struct fs_resize_info *info,
                         char *why, size_t cap, int *stage);

#endif
