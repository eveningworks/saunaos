#ifndef ULIB_UDATADIR_H
#define ULIB_UDATADIR_H
#ifdef __cplusplus
extern "C" {
#endif
// A program's DATA FOLDERS (the XDG Base Directory rules, under their
// own variables): where a program WRITES its own
// data ($XDG_DATA_HOME, else $HOME/.local/share) and where it LOOKS for
// data, that folder first and then each of $XDG_DATA_DIRS (else
// /usr/local/share:/usr/share) -- so what a person downloads is theirs
// and needs no administrator, and what the system ships still counts.
// `rel` is relative ("doom/freedoom1.wad"). Paths are allocated; free().

// $XDG_DATA_HOME/`rel` -- where a write goes. NULL only when out of memory.
char *udatadir_home(const char *rel);
// udatadir_home(), with every folder above it made -- where a write is
// about to go. NULL only when out of memory.
char *udatadir_prepare(const char *rel);
// The first of the data folders holding `rel`, or NULL.
char *udatadir_find(const char *rel);

// A person's STATE -- a history, a recent list, the tabs open at exit:
// $XDG_STATE_HOME/`rel`, else $HOME/.local/state -- and CACHE, what can
// be made again: $XDG_CACHE_HOME/`rel`, else $HOME/.cache (users stage
// D1c). ROOT HAS NEITHER: for euid 0 `machine`, the /var path, is
// answered instead, so a root process never leaves a root-owned file in
// a home (base/uconf.h's rule for the person's settings). Allocated;
// NULL only when out of memory. The _prepare calls make every folder
// above it first.
char *udatadir_state(const char *rel, const char *machine);
char *udatadir_state_prepare(const char *rel, const char *machine);
char *udatadir_cache(const char *rel, const char *machine);
char *udatadir_cache_prepare(const char *rel, const char *machine);
// udatadir_state_prepare(), REMEMBERED: the same string for the same
// `rel` for the life of the process -- what a state file's #define
// becomes. Never freed. NULL only when out of memory.
const char *udatadir_state_file(const char *rel, const char *machine);

#ifdef __cplusplus
}
#endif
#endif
