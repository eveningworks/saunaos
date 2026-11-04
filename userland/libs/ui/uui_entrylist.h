#ifndef UUI_ENTRYLIST_H
#define UUI_ENTRYLIST_H

// uui_entrylist -- a scrolling list of ENTRIES, each a title with a dim
// line under it (a page and its address, a file and its folder), grouped
// under HEADINGS ("Today", "Yesterday"): a click opens an entry, and a
// list that allows it shows a remove button on the hovered row. Firefox's
// bookmarks and history sidebar, Edge's history panel; the Web Browser's
// sidebar is the first user.
//
// THE CALLER OWNS THE ROWS -- headings are rows too, in order -- and
// filters them itself; the widget draws, scrolls, and reports a click as
// an EVENT with its row (uui_entrylist_take()).
#include "ui/uui_sbar.h"
#include "ui/uui_widget.h"

struct uui_entry {
    char title[96];
    char sub[128];      // the dim second line; empty for a heading
    int heading;        // 1: a group heading, not an entry
};

enum { UUI_ENTRY_EV_NONE, UUI_ENTRY_EV_OPEN, UUI_ENTRY_EV_REMOVE };

struct uui_entrylist {
    int x, y, w, h;
    const struct uui_entry *rows;   // caller-owned
    int count;
    int removable;                  // the hovered entry shows a remove button
    const char *empty;              // shown when there is nothing; may be NULL
    // OWNED
    struct uui_sbar sb;
    int top;                        // pixels scrolled
    int hot, hot_remove;            // the row under the pointer, and whether on its button
    int armed, armed_remove;
    int event, event_row;
};

void uui_entrylist_init(struct uui_entrylist *l, const char *empty);
// The rows changed: back to the top, and nothing held.
void uui_entrylist_reset(struct uui_entrylist *l);
int uui_entrylist_take(struct uui_entrylist *l, int *row);

extern const struct uui_widget_ops uui_entrylist_ops;

#endif
