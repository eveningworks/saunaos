#ifndef ULIB_UPINS_H
#define ULIB_UPINS_H

// PINNED FOLDERS: the folders a person added to Places, kept in
// /etc/places.conf one absolute path per line, optionally a TAB and the
// name it is shown by -- GTK's bookmarks file ("file:///path Name"),
// Explorer's Quick access pins. A TAB, not GTK's space, because a path
// here may hold spaces. The File Manager pins and unpins; it and
// the shared file dialog both list them, so a pin made in one is in the
// other. A pinned folder that has gone is left in the file and skipped
// when listed: a disk that is not mounted today may be tomorrow.

#define UPINS_FILE "/etc/places.conf"
#define UPINS_MAX  8
#define UPINS_PATH 256
#define UPINS_LABEL 24   // uui_places' label

// The pins, in the order they were made; how many. Only folders that
// exist now are returned.
int upins_load(char (*out)[UPINS_PATH], int cap);
int upins_has(const char *path);
// 0, -EEXIST (already pinned), -ENOSPC (UPINS_MAX already), -EINVAL
// (not an absolute path to a folder), or a write's -errno.
int upins_add(const char *path);
int upins_remove(const char *path);   // 0, or -ENOENT when it was not pinned
// The name a pin is shown by: its own when renamed, else the folder's.
void upins_label(const char *path, char *out, int cap);
// Renames a pin; "" (or the folder's own name) goes back to the folder's.
// 0, -ENOENT, -EINVAL (a TAB or newline in it), or a write's -errno.
int upins_set_label(const char *path, const char *label);
// Moves a pin `delta` places up (<0) or down (>0) the list, stopping at
// either end. 0, -ENOENT, -ERANGE (already at that end), or -errno.
int upins_move(const char *path, int delta);

#endif
