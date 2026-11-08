// <utime.h> -- set a file's access and modification times.
//
// **DECLARED, AND IT REFUSES: ENOTSUP for a file that exists.** No
// filesystem here can be told a time -- a backend stamps `modified`
// itself on every change and offers no operation to set it -- so the
// honest answer is the one a filesystem without the capability gives.
// It is declared because ported code calls it unconditionally (binutils'
// `strip -p` and `objcopy -p` restore a file's times through it, and
// print the refusal), and a build that cannot compile is the worse
// failure. docs/roadmap.md has the backend operation it waits for.
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

// -1 with ENOENT for a missing path, otherwise -1 with ENOTSUP.
int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
}
#endif

#endif
