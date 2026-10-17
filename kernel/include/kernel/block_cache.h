#ifndef BLOCK_CACHE_H
#define BLOCK_CACHE_H

#include <stdint.h>

struct block_device;

// The disk cache: a WRITE-THROUGH cache of 4 KiB disk pages under every
// filesystem, consulted by block.c's io_read() and kept current by its
// writes -- Linux's buffer cache (the block device's own page cache),
// at toy-os's size. docs/pagecache-design.md, "Stage 1b", has the
// measurement that chose it and what it does not do.
//
// KEYED BY THE WHOLE DISK AND THE ABSOLUTE SECTOR, never by the device a
// call came through: a partition forwards to its parent's driver without
// passing io_*() again, so a raw-disk write and a partition read of the
// same sector must meet at one key or one of them is served stale data.
// Only REGISTERED, PERSISTENT devices are cached (block.c decides): a
// KTEST's throwaway disk can be freed and its address reused, and a RAM
// image's I/O is already a memcpy.
//
// WRITE-THROUGH ONLY. Nothing here is ever dirty, so a crash, a failed
// flush or a bypass can cost a stale READ at worst, never lost data --
// the property that let this sit in the block layer where ata_cache.h
// explains a write-back cache must not.

// 1 = the whole range was cached and is now in `buf`. `disk`/`alba` are
// the normalised key (block.c's resolve); a miss changes nothing.
int bcache_read(const struct block_device *disk, uint64_t alba, int count, void *buf);

// Around a device READ that missed: a ticket before, then the data after.
// The fill is DROPPED if any of its sectors was written in between --
// the device may have returned the old bytes after the write updated the
// cache, and inserting them would undo it.
uint64_t bcache_ticket(const struct block_device *disk, uint64_t alba, int count);
void bcache_fill(const struct block_device *disk, uint64_t alba, int count,
                 const void *buf, uint64_t ticket);

// Around a device WRITE: _begin before it is issued, _end after with its
// result. A success leaves the new bytes cached; a failure (or a trim,
// `buf` NULL) drops the range, since what the device now holds is unknown.
void bcache_write_begin(const struct block_device *disk, uint64_t alba, int count);
void bcache_write_end(const struct block_device *disk, uint64_t alba, int count,
                      const void *buf, int ok);

// Forget everything cached for a disk: it is going away, and a new one
// could be registered at the same address.
void bcache_forget(const struct block_device *disk);

// The budget in MiB; 0 = automatic (RAM/32, 4..64 MiB). Takes effect at
// once: the cache is emptied and resized. The `nodiskcache` boot word
// turns it off for the boot, whatever this says.
void bcache_set_budget_mib(uint32_t mib);
uint32_t bcache_budget_mib(void);   // what was set; 0 = automatic

// Empty the cache, keeping its size: the next read of anything goes to
// the device. Linux's `vm.drop_caches`; `kernel.drop_caches` is the
// handle ring 3 holds, for a benchmark that must measure the disk.
void bcache_drop(void);

// `diskcache=verify` on the boot line: block.c re-reads every hit from
// the device and logs a mismatch. A diagnostic that halves its benefit.
int bcache_verify(void);

struct bcache_stats {
    uint64_t hits, misses;        // reads served / not served, in CALLS
    uint64_t hit_sectors;         // sectors those hits carried
    uint64_t fills, dropped_fills;  // misses inserted / refused by a racing write
    uint64_t evictions;
    uint32_t pages, cap_pages;    // resident / budget, 4 KiB each
    uint32_t active;              // of `pages`, seen at least twice
    int on;
};
void bcache_stats(struct bcache_stats *out);

#endif
