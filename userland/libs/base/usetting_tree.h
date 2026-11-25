#ifndef ULIB_USETTING_TREE_H
#define ULIB_USETTING_TREE_H

// THE WHOLE DOTTED TREE: every setting in the merged registry and every
// scalar kernel fact, by qualified name, in one sorted list -- what
// `config get <branch>` walks and the Configuration Editor shows.
// Settings and facts share the namespace, BSD sysctl's arrangement
// (docs/settings-and-queries.md, "Names").
//
// THE ORDER IS THE TREE'S: names compare segment by segment, so a
// branch's children follow it directly ("a.b", "a.b.c", "a.b-c"). Plain
// strcmp() would put "a.b-c" between them, '-' sorting below '.', and a
// reader walking the list as an outline would see a branch split.
//
// A SNAPSHOT: values are the INFO walk's, read once. Re-collect after a
// change; nothing here watches.

#include <stdint.h>
#include "setting_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct usetting_entry {
    char name[SETTING_ABI_QUALIFIED_MAX];
    char value[SETTING_ABI_VALUE_MAX];
    // The merged registry's index, for SETTING_OP_INFO -- valid until a
    // RELOAD reorders it; -1 for a fact.
    int32_t index;
    uint8_t fact;   // a kernel fact: read-only, never stored
    // SETTING_ABI_TYPE_* for a setting; QUERY_TYPE_* for a fact.
    uint8_t type;
};

// Fills *out with a malloc()ed array (free() it) and returns how many,
// or -1 when nothing could be read (*out NULL then). A registry that is
// down leaves the settings out and still lists the facts; memory running
// out part way returns what was read, and says so on the console.
int usetting_tree_collect(struct usetting_entry **out);

// The tree's order, for a caller sorting names of its own: <0, 0, >0.
int usetting_tree_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif // ULIB_USETTING_TREE_H
