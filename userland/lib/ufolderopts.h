#ifndef ULIB_UFOLDEROPTS_H
#define ULIB_UFOLDEROPTS_H

#include <stdint.h>
#include "lib/uprefs.h"

// ufolderopts -- how files are shown and opened, in File Manager windows
// AND on the desktop: Explorer's Folder Options, which apply to both. The
// File Manager's Options window edits them (its View, General and Rename
// and delete pages); the desktop only reads them.
//
// STORED IN THE FILE MANAGER'S OWN FILE, /etc/files.conf, under the keys
// it always had, so nothing moved for an existing install. One table
// (`ufolderopts_prefs`) for both readers, so they cannot disagree about a
// key or its default.

#define UFOLDEROPTS_CONF "/etc/files.conf"

struct ufolderopts {
    int single_click;     // a click opens, not a double click
    int thumbs;           // pictures and videos show themselves
    int thumb_max_mb;     // ... when the file is no bigger than this
    int hidden;           // names that start with a dot are listed
    int extensions;       // "dusk.jpg", not "dusk"
    int confirm_delete;   // Delete asks before moving to the Recycle Bin
};

extern const struct uprefs ufolderopts_prefs;

// The defaults, then the file.
void ufolderopts_load(struct ufolderopts *o);

// Whether a file of `size` bytes gets a thumbnail at all.
int ufolderopts_thumb_ok(const struct ufolderopts *o, uint64_t size);

#endif
