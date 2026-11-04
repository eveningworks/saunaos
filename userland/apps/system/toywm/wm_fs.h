#ifndef WM_FS_H
#define WM_FS_H

#include <stdint.h>
#include "syscall_abi.h" // struct sys_dirent, SYS_O_*

// The ring-3 WM's filesystem helpers, over libsys.
//
// NOT a compatibility layer for `api/fs.h`. The kernel's `fs_*` are a
// different shape, and the port takes the ring-3 shape rather than
// wrapping the old one back into existence:
//
//   `fs_list(dir, cb)` walked a directory through a CALLBACK WITH NO
//   CONTEXT POINTER, which every caller worked around the same way --
//   collect names into a file-global first, then parse. Three separate
//   comments in this WM complain about it. `sys_listdir()` fills an
//   ARRAY, so the workaround simply disappears; callers loop.
//
// What is left here is the handful of questions libsys does not answer
// directly, each of which would otherwise be re-derived at three call
// sites: does this path exist, is it a directory, how big is it, and
// read a whole small file into my own buffer.

// Whether `path` exists at all, file or directory (sys_stat()).
int wm_fs_exists(const char *path);

// Whether `path` exists AND is a directory. Same lookup as above; a
// missing path answers 0 rather than being an error, since every caller
// here is asking "may I descend into this".
int wm_fs_is_dir(const char *path);

// Size in bytes, or 0 if the path is missing or is a directory.
uint32_t wm_fs_size(const char *path);

// Reads a whole file into the CALLER's buffer. Returns the byte count,
// or 0 on any failure -- including a file too big for `cap`, which is
// REFUSED rather than truncated. That refusal is the point: a truncated
// config file parses as a valid file with things missing, which is the
// failure mode this repo's `fs_read_into()` exists to avoid.
uint32_t wm_fs_read_into(const char *path, void *buf, uint32_t cap);

#endif
