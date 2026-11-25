#ifndef CONFEDIT_INTERNAL_H
#define CONFEDIT_INTERNAL_H

// The Configuration Editor's two halves share this: confedit_model.c
// holds the data -- every setting and fact (base/usetting_tree.h), the
// branches of the dotted tree, the rows the table shows and where each
// value came from -- and confedit.c is the window. Split by concern, as
// toywm is; nothing here names a widget.

#include <stdint.h>
#include "base/usetting_tree.h"
#include "setting_abi.h"

// Where a row's value comes from: the From column.
enum ce_from {
    CE_FROM_DEFAULT = 0,   // the schema's Default=
    CE_FROM_MACHINE,       // /etc
    CE_FROM_USER,          // the person's ~/.config
    CE_FROM_KERNEL,        // a setting the kernel registered, or a fact
};

// Per entry, worked out when its row is first listed (a declaration and
// up to two files read) and kept until the next reload.
struct ce_info {
    signed char known;     // 0 until worked out
    signed char from;      // enum ce_from
    signed char declared;  // a schema in /etc/settings.d declares it
    signed char scope;     // USCHEMA_SCOPE_*, for a declared one
    signed char person;    // a write goes to the person's file in this process
};

// A branch of the tree: a proper prefix of some name ("desktop",
// "desktop.transparency"). Its entries -- its own value first, when it
// has one, then everything under it -- are CONTIGUOUS in the tree's
// order, entries[first .. last).
struct ce_branch {
    char name[SETTING_ABI_QUALIFIED_MAX];
    char label[SETTING_ABI_QUALIFIED_MAX];   // the last segment
    char note[24];                            // "69", "16 facts": top level only
    int depth, first, last, facts, kids, open;
};

struct ce_model {
    struct usetting_entry *e;
    struct ce_info *info;
    int n;
    struct ce_branch *b;
    int nb;
    int *rows;   // entry indices the table shows, in order
    int nrows;
    int branch;  // the branch the rows are of, -1 for a find
    char *find;  // the find text, sized to it; "" for none, never NULL
};

extern struct ce_model g_m;

// Re-reads everything. Branches that were open stay open, by name.
// 0, or -1 when the registry could not be read (the old data stays).
int  ce_model_load(void);
// The rows for `branch`, or for the find text when it is not empty.
void ce_model_rows(int branch, const char *find);
// Works out where entry `i`'s value comes from, once.
const struct ce_info *ce_model_info(int i);
// The branch named `name`, or -1.
int  ce_model_branch_named(const char *name);
// The branch an entry is listed under (its parent), or -1.
int  ce_model_parent_of(int entry);
// The entry named `name`, or -1.
int  ce_model_entry_named(const char *name);
// Re-reads one entry's value after a change, and forgets its origin.
void ce_model_refresh(int i);

const char *ce_from_word(int from);
const char *ce_type_word(const struct usetting_entry *e);

#endif
