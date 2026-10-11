#ifndef ULIB_UPINS_H
#define ULIB_UPINS_H

// PLACES: the list down the top of a file window's side pane, in the
// person's order -- Explorer's Quick access and Dolphin's Places panel,
// ONE list: Recent, Home, the personal folders and the Recycle Bin are
// its default entries, and a folder pinned to it is an entry like them.
// Any entry can be unpinned, renamed or moved; "Restore default places"
// brings back a default that was unpinned.
//
// Kept in /etc/places.conf, one entry per line, optionally a TAB and the
// name it is shown by (GTK's bookmarks file says "file:///path Name";
// a TAB because a path here may hold spaces). An ENTRY is a folder's
// path, or one of
//     recent:/        Recent (the File Manager's)
//     trash:/         the Recycle Bin
//     udir:<key>      a personal folder BY NAME (lib/udirs.h): it
//                     follows the folder if /etc/user-dirs.conf moves it
// A file without the "#places 2" line is the old one -- pins only -- and
// is read as the defaults followed by those pins. A pinned folder that
// has gone is kept in the file and skipped when listed: a disk that is
// not mounted today may be tomorrow. The File Manager and the shared file
// dialog list the same entries.

#define UPINS_FILE  "/etc/places.conf"
#define UPINS_MAX   24
#define UPINS_PATH  256
#define UPINS_LABEL 24   // uui_places' label

// The entries to show, in order: folders that exist now, and every
// virtual one. How many.
int upins_load(char (*out)[UPINS_PATH], int cap);
// What an entry IS: the path to go to ("recent:/" stays itself), the
// name it is shown by (its own when renamed) and its icon. 1 if virtual
// (recent:/, trash:/), 0 for a folder.
int upins_resolve(const char *entry, char *path, int pcap, char *label, int lcap,
                  const char **icon);
// The entry a path is listed as (a personal folder's path is its
// udir:<key> entry), into `out`; 1, or 0 when the path is not in the list.
int upins_entry_of(const char *path, char *out, int cap);
int upins_has(const char *path);   // a path or an entry
// Adds a folder at the end (`before` NULL) or before the entry `before`.
// 0, -EEXIST (listed already), -ENOSPC (UPINS_MAX), -EINVAL (not an
// absolute path to a folder), or a write's -errno.
int upins_add(const char *path);
int upins_add_before(const char *path, const char *before);
int upins_remove(const char *entry);   // 0, or -ENOENT when it was not listed
// The name an entry is shown by: its own when renamed, else its default.
void upins_label(const char *entry, char *out, int cap);
// Renames an entry; "" (or its default name) goes back to the default.
// 0, -ENOENT, -EINVAL (a TAB or newline in it), or a write's -errno.
int upins_set_label(const char *entry, const char *label);
// Moves an entry `delta` places up (<0) or down (>0), stopping at either
// end. 0, -ENOENT, -ERANGE (already at that end), or -errno.
int upins_move(const char *entry, int delta);
// Moves an entry to just before `before` (NULL: the end). 0, -ENOENT, -errno.
int upins_move_before(const char *entry, const char *before);
// The defaults not in the list now, as the "Restore default places" row
// would bring back: how many.
int upins_missing_defaults(void);
// Appends each missing default. How many came back, or -errno.
int upins_restore_defaults(void);

// THE MOUNTS' ORDER, in the same file as "dev:<mount point>" lines: the
// volumes under This computer and the shares under Network, as the
// person dragged them (Dolphin's Devices). A mount not named keeps the
// kernel's order, after those that are; one named but not mounted today
// keeps its place for when it is.
#define UPINS_DEV_MAX 32
// The saved order, into `out`: how many.
int upins_dev_order(char (*out)[64], int cap);
// Saves `points` (the mounts of one group, in their new order): each
// takes its slot, and the saved points not among them keep theirs after.
// 0 or -errno.
int upins_dev_set_order(char (*points)[64], int n);

#endif
