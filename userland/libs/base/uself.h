#ifndef ULIB_USELF_H
#define ULIB_USELF_H

// THE RUNNING PROGRAM'S OWN PATH -- Linux's /proc/self/exe, Windows'
// GetModuleFileName(NULL): the full path this process was spawned or
// exec'd from, as the KERNEL recorded it (QUERY_PROCPATH), never argv[0],
// which the caller chose. What a ported library asks to find files
// installed beside its program (SDL_GetBasePath()).
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Copies the path into `buf` (NUL-terminated). Its length, or -1 when
// the kernel has no path for this process or it does not fit in `cap`.
int uself_path(char *buf, size_t cap);

// The same for any live process `pid`: the path it was spawned from, as
// the kernel recorded it (QUERY_PROCPATH), never anything it said.
int uproc_path(int pid, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
