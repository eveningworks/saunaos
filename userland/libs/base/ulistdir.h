#ifndef ULIB_ULISTDIR_H
#define ULIB_ULISTDIR_H

// A WHOLE DIRECTORY LISTING, however many entries it has: SYS_GETDENTS's
// variable-length records read in pages into one malloc'd array of
// entries with a FIXED 256-byte name -- glibc's struct dirent over
// Linux's getdents64, so an entry copies by value and outlives nothing.
// For a caller that sorts or counts what it lists; one that walks a
// tree belongs on uwalk.h.
//
// A NAME LONGER THAN 255 BYTES (exFAT and SMB can have one: 255 UTF-16
// units are up to 765 bytes of UTF-8) is LEFT OUT, never cut, and said
// once on stderr; readdir() reports it whole.
//
// A directory written while it is paged can repeat or skip an entry --
// readdir()'s hazard, and the same answer.

#include <stdint.h>
#include "rt/sys.h"

#define ULIST_NAME_MAX 256   // FS_NAME_MAX (255) and its NUL

// One entry, with struct sys_dirent's field names (that struct is
// SYS_LISTDIR's, which keeps 63 bytes of a name; nothing here uses it).
struct ulist_entry {
    char name[ULIST_NAME_MAX];
    uint64_t size;            // bytes; 0 for a directory
    uint32_t is_dir;
    struct rtc_time modified;
};

// The entries of `path` into `*out` (free() it), returning how many; -1
// with errno set for a path that cannot be listed (ENOTDIR for a file),
// -1 with ENOMEM when the block cannot grow. `*out` is NULL on -1.
int ulistdir(const char *path, struct ulist_entry **out);

// 1 when `path` lists at least one entry, 0 when it is empty, -1 with
// errno when it cannot be listed -- without reading the rest.
int ulistdir_any(const char *path);

// SYS_GETDENTS into a buffer the caller keeps and this grows: the next
// page of `path` from `*start`, which it advances. Returns the bytes of
// records in `*buf` (walk them with ulistdir_next()), 0 at the end, -1
// with errno. For a caller that pages a directory it cannot hold whole.
int ulistdir_page(const char *path, uint32_t *start, char **buf, uint32_t *cap);

// The FIRST `max` entries of `path` into `out` -- a short look, not a
// listing: their order is the directory's. The count, or -1 with errno.
int ulistdir_first(const char *path, struct ulist_entry *out, int max);

// The next record at `*pos` in a page of `len` bytes, decoded into `*e`
// and `*pos` moved past it; 0 past the last. A name too long for `*e`
// is stepped over (the header's note).
int ulistdir_next(const char *page, int len, int *pos, struct ulist_entry *e);

#endif
