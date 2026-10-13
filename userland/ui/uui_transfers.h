#ifndef UUI_TRANSFERS_H
#define UUI_TRANSFERS_H

// uui_transfers -- a list of FILE TRANSFERS: each row a file's name, a
// progress bar while it runs and a status line under it ("14.2 MiB of
// 23.0 MiB", "4.0 MiB, done", "Failed: the connection closed"), and at
// the right the one action its state allows -- cancel while it runs,
// show it in its folder once done, retry after a failure. Firefox's
// downloads panel and Chrome's download bubble have this row; the Web
// Browser's Downloads flyout is the first user.
//
// **THE CALLER OWNS THE TRANSFERS; THE WIDGET ONLY DRAWS AND ASKS.** It
// is handed an array and reports a clicked action as an EVENT with its
// row (uui_transfers_take()); fetching, writing and cancelling are the
// app's, as they are uui_findbar's.
#include "ui/uui_widget.h"

enum uui_xfer_state { UUI_XFER_RUNNING, UUI_XFER_DONE, UUI_XFER_FAILED, UUI_XFER_CANCELLED };

struct uui_xfer {
    char name[96];
    int state;                     // enum uui_xfer_state
    unsigned long long got, total; // bytes; total 0 when the size is not known
    char why[96];                  // FAILED: what went wrong, a sentence
};

enum { UUI_XFER_EV_NONE, UUI_XFER_EV_CANCEL, UUI_XFER_EV_SHOW, UUI_XFER_EV_RETRY };

struct uui_transfers {
    int x, y, w, h;
    const struct uui_xfer *items;  // caller-owned, drawn top to bottom
    int count;
    // OWNED
    int hot, hot_action;           // the row under the pointer, and whether on its action
    int armed;                     // the row whose action a press armed, or -1
    int event, event_row;
};

void uui_transfers_init(struct uui_transfers *t);
int uui_transfers_row_h(void);
// The action a click committed (UUI_XFER_EV_*) and its row, once.
int uui_transfers_take(struct uui_transfers *t, int *row);

extern const struct uui_widget_ops uui_transfers_ops;

#endif
