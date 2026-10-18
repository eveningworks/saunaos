#ifndef UUI_DROPROWS_H
#define UUI_DROPROWS_H

// A DROPDOWN'S ROWS IN THREE PARTS: the fixed choices, then a few the
// person added (a custom size, a folder they chose), newest first, then
// one row that opens a dialog to add another ("Custom...", "Choose
// folder..."). CrystalDiskMark's drive list ending in "Select Folder",
// and KDiskMark's "Add a directory". Disk Mark's size and location.
//
// THE "MORE" ROW IS AN ACTION, NEVER A VALUE: when it is picked the
// caller opens its dialog and calls uui_droprows_reselect(), so the box
// goes on showing the value in force behind the dialog -- and after a
// Cancel. The selection this keeps is the last one SELECTED, by kind
// and index, so rebuilding the rows never moves it to a neighbour.

#include "ui/uui_dropdown.h"

#define UUI_DROPROWS_FIXED 8
#define UUI_DROPROWS_OWN   4
#define UUI_DROPROWS_LABEL 256   // a path: UUI_FILEVIEW_PATH_MAX

enum uui_droprow_kind {
    UUI_DROPROW_NONE = -1,
    UUI_DROPROW_FIXED,
    UUI_DROPROW_OWN,
    UUI_DROPROW_MORE,
};

struct uui_droprows {
    struct uui_dropdown *dd;
    const char *fixed[UUI_DROPROWS_FIXED];   // the caller's, outliving the rows
    int nfixed;
    char own[UUI_DROPROWS_OWN][UUI_DROPROWS_LABEL];
    int nown, max_own;
    const char *more;
    const char *items[UUI_DROPROWS_FIXED + UUI_DROPROWS_OWN + 1];
    int rows;
    int sel_kind, sel_index;
};

// `dd` is already initialised (its geometry is kept). `max_own` is
// clamped to UUI_DROPROWS_OWN; `more` is the action row's label.
void uui_droprows_init(struct uui_droprows *r, struct uui_dropdown *dd, int max_own,
                       const char *more);
// Replaces the fixed rows; more than UUI_DROPROWS_FIXED are dropped.
void uui_droprows_set_fixed(struct uui_droprows *r, const char *const *labels, int n);

// Puts `label` FIRST among the person's rows -- moved up if it is there
// already, the oldest dropped past max_own -- and selects it. An empty
// label, or one too long to keep whole, adds nothing: 0, else 1.
int uui_droprows_add_own(struct uui_droprows *r, const char *label);
void uui_droprows_remove_own(struct uui_droprows *r, int index);
// The index of `label` among the person's rows, or -1.
int uui_droprows_find_own(const struct uui_droprows *r, const char *label);
// The person's row `index`, oldest last; NULL past the end.
const char *uui_droprows_own(const struct uui_droprows *r, int index);

// Selects a row by kind and index (MORE is refused: it is never a value).
void uui_droprows_select(struct uui_droprows *r, int kind, int index);
// Shows the last selection again -- after the MORE row was picked.
void uui_droprows_reselect(struct uui_droprows *r);
// What dropdown row `row` is, and its index within its kind.
int uui_droprows_kind(const struct uui_droprows *r, int row, int *index);
// The same for the row the dropdown shows now.
int uui_droprows_picked(const struct uui_droprows *r, int *index);

#endif
