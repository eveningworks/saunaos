#ifndef UUI_LISTEDIT_H
#define UUI_LISTEDIT_H

// AN EDITABLE LIST: a table of rows with a column of buttons beside it --
// Add..., Change..., Remove, Move up, Move down -- each greyed when it
// does not apply to the selected row. winecfg's Drives tab, Windows'
// Environment Variables dialog, KDE's lists of folders to index.
//
// A CONTAINER, so the router does its input: a button's click reaches
// the app as on_action(code) and the table's selection as
// on_widget(table_id). The rows are the app's, PULLED through the
// table's `cell` (uui_table.h); this holds no data.
//
// Whether a button applies to a row is the app's answer (`enabled`),
// asked as the buttons are painted, so a list that changed under it is
// never shown with a stale button.

#include "ui/uui_button.h"
#include "ui/uui_table.h"
#include "ui/uui_widget.h"

#define UUI_LISTEDIT_BUTTONS_MAX 8   // a column of buttons, not data

struct uui_listedit {
    int x, y, w, h;
    struct uui_table table;
    struct uui_button btn[UUI_LISTEDIT_BUTTONS_MAX];
    int nbtn;
    struct uui_item items[1 + UUI_LISTEDIT_BUTTONS_MAX];
    int rows_shown;                  // natural height, in rows
    // Is the button with `code` usable with `row` (-1: none selected)?
    // NULL: a button needs a selected row unless it is the first (Add).
    int (*enabled)(void *ctx, int code, int row);
    // Optional: the selected row's facts, "Label<TAB>value" lines, shown in
    // a card under the table -- what a column has no room for. The text is
    // the app's and lives until the next call; NULL or "" shows no card.
    const char *(*details)(void *ctx, int row);
    void *ctx;
};

// `labels`/`codes`: the buttons, top to bottom (labels outlive the
// widget); `names` their test names, or NULL. `table_id`/`table_name`
// are the table's. `cell` and `ctx` as uui_table_init.
void uui_listedit_init(struct uui_listedit *l, const struct uui_table_column *cols, int ncols,
                       uui_table_cell_fn cell, void *ctx, int table_id, const char *table_name,
                       const char *const *labels, const int *codes, const char *const *names, int nbtn);

void uui_listedit_set_rows(struct uui_listedit *l, int rows);   // the data changed
int  uui_listedit_selected(const struct uui_listedit *l);       // a row, or -1
void uui_listedit_select(struct uui_listedit *l, int row);

extern const struct uui_widget_ops uui_listedit_ops;

#endif
