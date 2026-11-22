// The calls the fd layer makes for a file it holds open for writing:
// fs.h's, except that on a removable disk the change is SETTLED when the
// file closes (fs_settle_path()), or by a later write once a second has
// passed, rather than before each call returns -- vfs.c's owe(). For a
// call by path, which has no close to wait for, use fs.h's.
#ifndef FS_DEFER_H
#define FS_DEFER_H

#include <stdint.h>

int fs_touch_deferred(const char *path);
int fs_create_excl_deferred(const char *path);
int fs_write_deferred(const char *path, const char *data, int append);
int fs_write_range_deferred(const char *path, uint64_t offset, const void *buf, uint32_t len);

// fs_read_range() from a lock-free SYS_READ (no kernel lock, interrupts
// off): the bytes read, or -1 to DECLINE having changed nothing -- a
// backend that is not fs_ops.mpsafe, a mount table mid-change, a busy
// mount lock, or the cache simulator on. Never sleeps.
int fs_read_range_lockless(const char *path, uint64_t offset, void *buf, uint32_t len);

// fs_write_range_deferred() from a lock-free SYS_WRITE: -1 to DECLINE
// having changed nothing (the read's reasons, a read-only mount, a path
// the image cache may hold), 0 when the backend refused or ran out (a
// write may be partial, so never declined after), 1 written, 2 written
// and a watcher is OWED its note -- the caller takes the kernel lock for
// fs_note_watchers(). `offset` may be FS_OFFSET_END; `*size_out` is the
// file's size after the write, read under the same mount lock.
int fs_write_range_lockless(const char *path, uint64_t offset, const void *buf, uint32_t len,
                            uint64_t *size_out);
// Whether `path` is on an fs_ops.mpsafe mount with no device, for open()'s
// hint (open_file.file.lockless). Under the kernel lock.
int fs_mpsafe_at(const char *path);
// The fswatch note a lock-free write owed, under the kernel lock.
void fs_note_watchers(const char *path);

// The close: settle the volume holding `path` if a deferred call there
// left anything owed. A no-op on a fixed disk, or when nothing is owed.
void fs_settle_path(const char *path);

#endif
