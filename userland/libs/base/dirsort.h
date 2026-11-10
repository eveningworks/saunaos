#ifndef USERLAND_LIB_DIRSORT_H
#define USERLAND_LIB_DIRSORT_H

#include "syscall_abi.h" // struct ulist_entry
#include "base/ulistdir.h"   // struct ulist_entry

// Ordering a directory listing, in one place.
//
// WHY THIS IS SHARED RATHER THAN WRITTEN TWICE. SYS_LISTDIR returns
// entries in whatever order the filesystem walks them, which is not an
// order at all -- the same directory can list differently on two
// machines, and nothing a person reads should do that. So both /bin/ls
// and Notepad's file dialog sort, and THE TWO MUST AGREE: a test that
// derives a row index from `ls` output and then clicks that row in the
// dialog is only correct while they do. They disagreed for exactly one
// build, and that test is what caught it.
//
// IT SORTS THE ENTRIES, NOT A PERMUTATION, and that is a deliberate
// retreat: a permutation every caller must apply is a cycle walk that
// is easy to get subtly wrong, and this one WAS got wrong -- a listing
// that looked plausible and was in the wrong order. Inside, it sorts
// pointers and moves each entry once (dirsort.c).
enum dirsort_key {
    DIRSORT_NAME, // ascending, and the tie-break for the other two
    DIRSORT_TIME, // newest first, as coreutils' -t
    DIRSORT_SIZE, // largest first, as coreutils' -S
};

void dirsort(struct ulist_entry *e, int n, enum dirsort_key key, int reverse);

// The comparison dirsort() sorts BY, exposed on its own. <0, 0 or >0
// for a before/equal/after b, with `name` as the tie-break exactly as
// above.
//
// It exists because ui/uui_fileview.c sorts a VIEW rather than the
// array -- `uui_table` owns a permutation and asks the app to compare
// (uui_table.h says why), so it cannot call dirsort() at all. Sharing
// the comparison rather than reimplementing it is what keeps a file
// view's order identical to `/bin/ls`'s, which is the property this
// whole file exists to protect.
//
// NOTE THE DIRECTIONS, which are coreutils' and not a table header's:
// DIRSORT_SIZE puts the LARGEST first and DIRSORT_TIME the NEWEST, the
// way `ls -S` and `ls -t` do. A column header whose ascending arrow
// must mean smallest-first negates this rather than adding a third
// convention -- see uui_fileview.c.
int dirsort_cmp(const struct ulist_entry *a, const struct ulist_entry *b,
                 enum dirsort_key key);

#endif // USERLAND_LIB_DIRSORT_H
