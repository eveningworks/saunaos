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

// The close: settle the volume holding `path` if a deferred call there
// left anything owed. A no-op on a fixed disk, or when nothing is owed.
void fs_settle_path(const char *path);

#endif
