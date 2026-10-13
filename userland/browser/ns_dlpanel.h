#ifndef BROWSER_NS_DLPANEL_H
#define BROWSER_NS_DLPANEL_H

// The Downloads flyout -- D1 on the stage 4 canvas: a card under the
// toolbar's down arrow, "Downloads" over the list (ui/uui_transfers.h,
// the shared widget, as the router's child) and two links at its foot.
// Drawn in the window as an overlay, like the lock's card: a click
// outside closes it and still reaches what is under it.
#include "ui/uui_transfers.h"
#include "ui/uui_widget.h"

enum { NS_DLP_NONE, NS_DLP_OPEN_FOLDER, NS_DLP_CLEAR };

struct ns_dlpanel {
    int open;
    int x, y, w, h;
    struct uui_transfers list;
    struct uui_item child;
    // OWNED
    int link_x[3], link_w[3], link_y, link_h;   // [1] open folder, [2] clear
    int hot, armed, event;
};

void ns_dlpanel_init(struct ns_dlpanel *p, int list_id);
// Opens under (ax, ay) -- its RIGHT edge on ax + aw, as a menu from a
// button at the toolbar's right end opens -- inside the bounds.
void ns_dlpanel_open(struct ns_dlpanel *p, int ax, int ay, int aw, int bw, int bh);
// The list changed: re-measures, keeping the right edge where it was.
void ns_dlpanel_sync(struct ns_dlpanel *p, int bw, int bh);
void ns_dlpanel_close(struct ns_dlpanel *p);
int ns_dlpanel_take(struct ns_dlpanel *p);

extern const struct uui_widget_ops ns_dlpanel_ops;

#endif
