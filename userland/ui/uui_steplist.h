#ifndef UUI_STEPLIST_H
#define UUI_STEPLIST_H

// THE STEPS OF A BATCH, numbered, in the order they will run: what is
// queued (each with a Remove button), and while they run how far each
// has got -- done, running with a bar, failed, or not run. GParted's
// pending-operations pane and its Apply dialog, KDE Partition Manager's
// Pending Operations; Disks' first, the installer's and Boot Manager's
// when they queue.
//
// A uui_table underneath, so it scrolls, selects and keys like every
// list: place `&sl->table` with uui_table_ops. The steps are the
// caller's array, read as the table is painted.

#include "ui/uui_table.h"

enum { UUI_STEP_WAITING, UUI_STEP_RUNNING, UUI_STEP_DONE, UUI_STEP_FAILED, UUI_STEP_NOT_RUN };

struct uui_step {
    const char *verb;     // "Resize"
    const char *text;     // "usb1p2 (Photos): 24.0 GiB to 36.0 GiB"
    int state;            // UUI_STEP_*
    int permille;         // RUNNING: how far, 0..1000; -1 not known
};

#define UUI_STEPLIST_REMOVE 0x1   // a Remove button on each waiting step
#define UUI_STEPLIST_STATE  0x2   // a State column: Waiting, Running 41%, Done...

struct uui_steplist {
    struct uui_table table;
    const struct uui_step *steps;   // caller-owned
    int count, flags;
    struct uui_table_column cols[5];
    int col_text, col_state, col_remove;
};

// MUST START ZEROED (the table's arrays: uui_table.h), as a static is.
void uui_steplist_init(struct uui_steplist *sl, int flags);
void uui_steplist_set(struct uui_steplist *sl, const struct uui_step *steps, int count);
// The step whose Remove was pressed since the last take, or -1.
int  uui_steplist_take_remove(struct uui_steplist *sl);
// The height that shows the header and `rows` steps.
int  uui_steplist_height(const struct uui_steplist *sl, int rows);

#endif
