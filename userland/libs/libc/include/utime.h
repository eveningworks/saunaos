// <utime.h> -- set a file's access and modification times.
//
// **THE MODIFICATION TIME IS SET; THERE IS NO ACCESS TIME TO SET.**
// stat mirrors st_mtime into st_atime (<sys/stat.h>), so `actime` is
// accepted and has nowhere to go -- the `noatime` shape. SYS_UTIME does
// the work, and only tfs3 can be told a time: on FAT32, exFAT and the
// rest this is -1 with ENOTSUP, the answer of a filesystem without the
// capability. utimes() (<sys/time.h>) is the same with a timeval.
#ifndef _UTIME_H
#define _UTIME_H

#include <sys/types.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

struct utimbuf {
    time_t actime;    // access time
    time_t modtime;   // modification time
};

// `times` NULL: now. Returns 0, or -1 with ENOENT, ENOTSUP (above), or
// EINVAL for a time before the epoch, which no backend here stores.
int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
}
#endif

#endif
