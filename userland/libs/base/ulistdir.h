#ifndef ULIB_ULISTDIR_H
#define ULIB_ULISTDIR_H

// A WHOLE DIRECTORY LISTING, however many entries it has: SYS_LISTDIR_AT
// paged into one malloc'd array, for a caller that sorts or counts what
// it lists. One SYS_LISTDIR call stops at SYS_LISTDIR_MAX entries and
// says nothing about the rest, which is how `ls` came to print 256 names
// of a 400-entry folder. A caller that only walks belongs on uwalk.h.
//
// A directory written while it is paged can repeat or skip an entry --
// readdir()'s hazard, and the same answer.

#include "rt/sys.h"

// The entries of `path` into `*out` (free() it), returning how many; -1
// with errno set for a path that cannot be listed (ENOTDIR for a file),
// -1 with ENOMEM when the array cannot grow. `*out` is NULL on -1.
int ulistdir(const char *path, struct sys_dirent **out);

#endif
