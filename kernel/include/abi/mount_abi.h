#ifndef MOUNT_ABI_H
#define MOUNT_ABI_H

#include <stdint.h>

// The kernel<->userland contract for SYS_MOUNT and SYS_UMOUNT.
//
// WHAT REAL SYSTEMS TAKE. Linux's mount(2) is
// `mount(source, target, filesystemtype, flags, data)` -- five
// arguments, the last of them a free-form per-filesystem option string;
// Windows has no mount syscall at all (a volume is an object and
// mountvol/SetVolumeMountPoint work through the object manager).
//
// This is Linux's shape with the two parts SaunaOS cannot honestly
// carry left out. There is no `data` string, because no backend here
// has per-filesystem options and inventing a parser for none would be
// the second implementation of nothing. And SOURCE IS NOT A PATH: this
// OS has no /dev, so a volume is named by its PARTITION NUMBER on the
// boot disk (`2`), or by a backend name for one that needs no volume
// (`ramfs`). A struct rather than five registers, for the same reason
// SYS_MKPART takes one: one user pointer to validate instead of four.
//
// NO PRIVILEGE CHECK, and that is stated rather than implied -- this
// kernel has no uid. Mounting is not destructive (nothing is formatted,
// nothing is overwritten), so unlike SYS_MKPART there is not even a
// confirm flag to stand in for one. What it CAN do is hide a directory
// and expose a volume's contents, so a real system would gate it; the
// place to put that check when uids arrive is sys_mount().

// Per-mount flags. Mirrors kernel/mount.h's MNT_* one for one -- two
// names for one bit, because an ABI header may not include a kernel
// one, and the kernel-side mount_add() is what enforces it.
#define SYS_MNT_RDONLY 0x1

#define MOUNT_SOURCE_MAX 16 // "2", "ramfs" -- a partition number or a backend name
#define MOUNT_FSTYPE_MAX 16 // "fat32", "tfs3", or empty to probe
#define MOUNT_POINT_MAX  64 // FS_PATH_MAX, spelled out since abi/ has no fs.h

struct mount_request {
    char source[MOUNT_SOURCE_MAX];
    char fstype[MOUNT_FSTYPE_MAX];  // empty = probe every backend that could claim it
    char point[MOUNT_POINT_MAX];    // a normalized absolute path that already exists
    uint32_t flags;                 // SYS_MNT_*
    // A SIZE LIMIT FOR THIS MOUNT, in MiB; 0 means the backend decides.
    //
    // This is the `data` string's job in Linux, done as a typed field
    // instead -- the header above argues against inventing a parser for
    // options no backend had, and ramfs is the first backend that has
    // one. A number needs no parser and cannot be mistyped into
    // something that silently means nothing.
    //
    // It fits in what used to be `reserved`, so the struct is the same
    // size it always was and a caller built before this still compiles
    // to a zero here, which is the default. That is why the field was
    // reserved in the first place.
    uint32_t size_mib;
};

// --- SYS_MKFS ---------------------------------------------------------
//
// WHY THIS IS NOT `fsformat` WITH AN ARGUMENT. fs_format_backend() --
// the ring-0 `fsformat` command -- reformats the volume this machine is
// RUNNING FROM, unmounting everything first and re-probing after. That
// is a different operation from "put a filesystem on that other
// partition", which is what an installer wants and which must not
// disturb the running system at all.
//
// The device is named the same way mount_request.source names one, so
// there is one spelling of "which volume" across the ABI.
struct mkfs_request {
    char source[MOUNT_SOURCE_MAX];  // "ahci0p3" -- a partition, never a disk
    char fstype[MOUNT_FSTYPE_MAX];  // "tfs3", "fat32" -- never probed
    uint32_t flags;                 // MKFS_CONFIRM
    uint32_t reserved;
};

// The caller states that it knows this destroys whatever is on the
// volume. Same stand-in as MKPART_CONFIRM, and the same caveat: it is a
// speed bump, not a permission check, because this OS has no privilege
// model to make it one.
#define MKFS_CONFIRM 0x1
// **NOT A FORMAT: A QUESTION ABOUT THE ONE RUNNING** on `source`. RSI is
// then a `struct fs_check_progress` (below), filled without waiting for
// the format, so a second thread can draw its stages; -ESRCH when none is
// running there. FSCK_PROGRESS's shape, for the same dialog.
#define MKFS_PROGRESS 0x2

// --- SYS_FS_CHECK -----------------------------------------------------
//
// THE CHECK RUNS IN THE KERNEL, AGAINST THE MOUNTED VOLUME; a ring-3
// `fsck` asks for it and prints the result. That is XFS's online scrub
// and `btrfs scrub`, not e2fsck: the checker walks the backend's live
// state and repairs through its journal, which a process reading the
// raw device could not do without unmounting -- and the root is never
// unmounted. The volume is named by any PATH on it, as statfs(2) does.
//
// The counts below are BLOCKS (or, for records_used, inodes), with the
// meanings fs.h's fs_check() documents.
struct fs_check_result {
    uint32_t records_used;        // in-use table slots walked
    uint32_t blocks_referenced;   // distinct blocks reachable from those records
    uint32_t leaked;              // marked allocated, referenced by nothing
    uint32_t referenced_but_free; // referenced by a record, marked free
    uint32_t double_allocated;    // referenced from more than one place
    uint32_t out_of_range;        // pointers naming a block outside the usable range
    // Only nonzero on a repair pass -- what was actually changed.
    uint32_t reclaimed;           // leaked blocks returned to the free bitmap
    uint32_t marked_allocated;    // referenced-but-free blocks marked allocated
    uint32_t pointers_cleared;    // out-of-range pointers zeroed
    // WHICH VOLUME the path named -- filled by SYS_FS_CHECK after the
    // pass, so a caller need not resolve the path a second time.
    char fstype[MOUNT_FSTYPE_MAX];  // "tfs3"
    char point[MOUNT_POINT_MAX];    // "/" -- the mount the path resolved to
    // Not blocks. Each was only a kernel log line, so a pass that found
    // them still printed "clean".
    uint32_t counts_wrong;        // groups whose stored free counts disagree with the bitmaps
    uint32_t links_wrong;         // records whose link count is not the names that reach them
    uint32_t bad_names;           // names reaching a record that fails its checksum (never repaired)
    uint32_t dir_errors;          // corrupt entry chains, wrong `.`/`..`, a directory with two names
    uint32_t counts_fixed;        // repair: free counts recomputed
    uint32_t links_fixed;         // repair: link counts rewritten
};

// Everything the pass found, of every kind -- what "clean" means. ONE
// sum, so a reader cannot leave a kind out of it.
static inline uint32_t fs_check_problems(const struct fs_check_result *r) {
    return r->leaked + r->referenced_but_free + r->double_allocated + r->out_of_range +
           r->counts_wrong + r->links_wrong + r->bad_names + r->dir_errors;
}

// What a repair pass leaves behind: the kinds it never touches, and any
// link count it could not rewrite.
static inline uint32_t fs_check_unfixed(const struct fs_check_result *r) {
    return r->double_allocated + r->bad_names + r->dir_errors +
           (r->links_wrong - r->links_fixed);
}

// Also fix what can be fixed without guessing (fs.h's fs_check()).
// Refused with -ENOTSUP by a backend that can only report.
#define FSCK_REPAIR 0x1

// **NOT A CHECK: A QUESTION ABOUT THE ONE RUNNING.** RDX is then a
// `struct fs_check_progress`, filled WITHOUT the volume's lock -- the
// running pass holds it -- so a second thread can draw a bar while the
// first sits in the check. chkdsk's "Stage 2 of 3, 64 percent".
#define FSCK_PROGRESS 0x2
// Ask the READ-ONLY check running on the volume to stop; it then returns
// -ECANCELED. -ESRCH when none is running, -EBUSY for a repair: half a
// repair is a volume in a state nobody chose.
#define FSCK_STOP 0x4

#define FSCK_STAGES_MAX 4

// A snapshot, so the fields can disagree by one step: each is stored on
// its own, and nothing here is worth a lock the check would have to share.
struct fs_check_progress {
    uint32_t running;     // a pass is in flight; everything else is stale when 0
    uint32_t repair;      // ...and it is a repair, which cannot be stopped
    uint32_t stage;       // 0-based, < stages
    uint32_t stages;      // 0: the backend reports no progress (draw it busy)
    uint32_t done, total; // this stage's units; total 0 is "not known yet"
    uint32_t problems;    // found so far, every kind together
    uint32_t records;     // fs_check_result's records_used, so far
    uint32_t blocks;      // ...and blocks_referenced
    // Each stage's name ("Compare the allocation maps") and what its
    // units count ("groups"), so a caller lists stages it has not reached.
    char names[FSCK_STAGES_MAX][32];
    char units[FSCK_STAGES_MAX][12];
};

// --- SYS_FS_RESIZE ----------------------------------------------------
//
// THE FILESYSTEM HALF OF A PARTITION RESIZE, on a volume NOTHING HAS
// MOUNTED: resize2fs's job, a step apart from the table edit
// (SYS_PARTEDIT) exactly as resize2fs is apart from sfdisk. The safe
// order is the CALLER's -- growing, the table first and then this;
// shrinking, this first and then the table -- which is what `part
// resize` and Disks do. Only a GROW is built (tfs3); every refusal says
// why in `info.why`, so a caller shows the kernel's reason rather than
// guessing one.
#define FS_RESIZE_WHY_MAX 120

struct fs_resize_info {
    char fstype[MOUNT_FSTYPE_MAX];   // "tfs3"; "" when nothing recognises the volume
    uint32_t flags;                  // FS_RESIZE_CAN_*
    uint32_t reserved;
    uint64_t size_bytes;             // the FILESYSTEM's size now, not the partition's
    uint64_t used_bytes;             // what its files and directories take
    // The sizes it can be resized to, inclusive. An empty volume (nothing
    // recognised) is 0 and UINT64_MAX: the table alone changes.
    uint64_t min_bytes, max_bytes;
    char why[FS_RESIZE_WHY_MAX];     // what keeps it from growing or shrinking; "" when nothing
};

#define FS_RESIZE_CAN_GROW   0x1
#define FS_RESIZE_CAN_SHRINK 0x2

struct fsresize_request {
    char source[MOUNT_SOURCE_MAX];   // the partition, as mkfs_request names it
    uint32_t flags;                  // FSRESIZE_*
    uint32_t reserved;
    uint64_t sectors;                // the new size in 512-byte sectors; 0 = fill the volume
    struct fs_resize_info info;      // OUT: what is possible, then (after a resize) what it is
};

// Required for a resize: the same speed bump as MKFS_CONFIRM.
#define FSRESIZE_CONFIRM  0x1
// Only fill `info`; change nothing. Needs no CONFIRM.
#define FSRESIZE_INFO     0x2
// **NOT A RESIZE: A QUESTION ABOUT THE ONE RUNNING** on `source` -- RSI is
// a `struct fs_check_progress`, MKFS_PROGRESS's shape; -ESRCH when none is.
#define FSRESIZE_PROGRESS 0x4

#endif
