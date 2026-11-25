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

#ifdef __cplusplus
}
#endif
#endif
