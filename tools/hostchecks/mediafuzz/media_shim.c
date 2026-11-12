// The kernel services the media parsers call, on the host, over one
// sparse in-memory disk (mediafuzz.c has the input format). Each stub
// keeps the real one's CONTRACT where a parser leans on it: kmalloc
// refuses what a kernel heap could not hold, and the block layer answers
// 1/0 and refuses a range past the device.
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "heap.h"
#include "klog.h"
#include "kfmt.h"
#include "block.h"
#include "fs.h"
#include "mount.h"
#include "ktime.h"
#include "krandom.h"
#include "scheduler.h"
#include "kpath_buf.h"
#include "mediafuzz.h"

// ---- the heap: the kernel's kmalloc refuses what it cannot hold ----------
#define HEAP_CAP (64u << 20)
size_t fz_biggest_alloc;
void *kmalloc(size_t n) {
    if (n > fz_biggest_alloc) fz_biggest_alloc = n;
    if (n > HEAP_CAP) return NULL;
    return malloc(n ? n : 1);
}
void *kzalloc(size_t n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }
void *krealloc(void *p, size_t n) {
    if (n > fz_biggest_alloc) fz_biggest_alloc = n;
    if (n > HEAP_CAP) return NULL;
    if (!n) { free(p); return NULL; }
    return realloc(p, n);
}
void kfree(void *p) { free(p); }

int fz_verbose;
void klog_write(const char *s) { if (fz_verbose) fputs(s, stderr); }
void klog_printf(const char *fmt, ...) {
    if (!fz_verbose) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

// ---- the sparse disk ------------------------------------------------------
struct fz_disk fz;

static uint8_t *slot_of(uint64_t lba, int make) {
    uint64_t h = (lba * 0x9E3779B97F4A7C15ull) >> 40;
    for (uint32_t i = 0; i < FZ_SLOTS; i++) {
        uint32_t s = (uint32_t)((h + i) % FZ_SLOTS);
        if (fz.used[s] && fz.lba[s] == lba) return fz.data[s];
        if (!fz.used[s]) {
            if (!make) return NULL;
            if (fz.nused >= FZ_SLOTS * 3 / 4) return NULL; // full: the write is lost
            fz.used[s] = 1; fz.lba[s] = lba; fz.nused++;
            memset(fz.data[s], 0, 512);
            return fz.data[s];
        }
    }
    return NULL;
}
void fz_put_sector(uint64_t lba, const uint8_t *d) { uint8_t *p = slot_of(lba, 1); if (p) memcpy(p, d, 512); }
void fz_read_bytes(uint64_t off, void *buf, uint32_t len) {
    uint8_t *o = buf;
    while (len) {
        uint64_t lba = off / 512; uint32_t in = (uint32_t)(off % 512), n = 512 - in;
        if (n > len) n = len;
        uint8_t *p = slot_of(lba, 0);
        if (p) memcpy(o, p + in, n); else memset(o, 0, n);
        o += n; off += n; len -= n;
    }
}

static uint64_t d_count(const struct block_device *d) { (void)d; return fz.sectors; }
static int d_read(const struct block_device *d, uint64_t lba, int count, void *buf) {
    (void)d;
    if (count < 0 || lba > fz.sectors || (uint64_t)count > fz.sectors - lba) { fz.oob_io++; return 0; }
    fz_read_bytes(lba * 512, buf, (uint32_t)count * 512);
    fz.reads += (uint64_t)count;
    return 1;
}
static int d_write(const struct block_device *d, uint64_t lba, int count, const void *buf) {
    (void)d;
    if (count < 0 || lba > fz.sectors || (uint64_t)count > fz.sectors - lba) { fz.oob_io++; return 0; }
    fz.written += (uint64_t)count;
    // FZ_GUARD=lo:hi -- sectors a case says no write may reach.
    if (fz.guard_hi && lba < fz.guard_hi && lba + (uint64_t)count > fz.guard_lo) {
        fprintf(stderr, "INVARIANT: a write reached guarded sector %llu\n", (unsigned long long)lba);
        abort();
    }
    for (int i = 0; i < count; i++) fz_put_sector(lba + (uint64_t)i, (const uint8_t *)buf + i * 512);
    return 1;
}
static int d_max(const struct block_device *d) { (void)d; return 128; }
const struct block_device fz_dev = {
    .name = "fuzz", .driver = "fuzz", .sector_count = d_count,
    .read_sectors = d_read, .write_sectors = d_write, .max_sectors_per_xfer = d_max,
    .removable = 1, .block_size = 512,
};

// The block layer's own checks are part of what protects the kernel, so
// they are the real ones' shape: 1 on success, 0 for a range past the device.
int blkdev_read_sectors(const struct block_device *dev, uint64_t lba, int count, void *buf) {
    return dev->read_sectors(dev, lba, count, buf);
}
int blkdev_write_sectors(const struct block_device *dev, uint64_t lba, int count, const void *buf) {
    return dev->write_sectors(dev, lba, count, buf);
}
int blkdev_read_partial(const struct block_device *dev, uint64_t lba, int count, void *buf) {
    return blkdev_read_sectors(dev, lba, count, buf);
}
int blkdev_write_partial(const struct block_device *dev, uint64_t lba, int count, const void *buf) {
    return blkdev_write_sectors(dev, lba, count, buf);
}
int blkdev_max_sectors_per_xfer(const struct block_device *dev) { return dev->max_sectors_per_xfer(dev); }
int blkdev_flush(const struct block_device *dev) { (void)dev; return 1; }
uint64_t blkdev_sector_count(const struct block_device *dev) { return dev->sector_count(dev); }
uint32_t blkdev_block_size(const struct block_device *dev) { (void)dev; return 512; }
uint32_t blkdev_block_sectors(const struct block_device *dev) { (void)dev; return 1; }
const struct block_device *blk_root_disk(void) { return &fz_dev; }
const char *blk_device_name(const struct block_device *dev) { (void)dev; return "fuzz0"; }

// ---- an ISO image is a FILE on a parent mount ------------------------------
uint64_t fs_size(const char *path) { (void)path; return fz.sectors * 512; }
int fs_is_dir(const char *path) { (void)path; return 0; }
uint32_t fs_read_range(const char *path, uint64_t off, void *buf, uint32_t len) {
    (void)path;
    uint64_t size = fz.sectors * 512;
    if (off >= size) return 0;
    if (len > size - off) len = (uint32_t)(size - off);
    fz_read_bytes(off, buf, len);
    fz.reads += len / 512 + 1;
    return len;
}
int fs_lock_held_at(const char *path) { (void)path; return 0; }
int fs_lock_owner_at(const char *path) { (void)path; return 0; }
int mount_drop_begin(void *st, struct mount_drop *g) { (void)st; (void)g; return 0; }
int mount_drop_end(struct mount_drop *g) { (void)g; return 1; }
int mount_add_nodev(const struct fs_ops *fs, void *state, const char *point,
                    unsigned flags, const char *backing, const char **why) {
    (void)fs; (void)state; (void)point; (void)flags; (void)backing; (void)why; return 1;
}

// ---- the rest -------------------------------------------------------------
void ktime_read(struct rtc_time *out) { memset(out, 0, sizeof *out); out->year = 2026; out->month = 1; out->day = 1; }
void krandom_bytes(void *buf, size_t n) { memset(buf, 0x5A, n); }
int scheduler_current_pid(void) { return 1; }
struct fs_check_live *fs_format_live(void) { return NULL; }
uint64_t krandom_u64(void) { return 0x5A5A5A5A5A5A5A5Aull; }
uint64_t clocksource_now_ns(void) {   // ratelimit.h's clock
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
