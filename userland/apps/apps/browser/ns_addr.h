#ifndef BROWSER_NS_ADDR_H
#define BROWSER_NS_ADDR_H

// The address field as the toolbar shows it -- S1 on the stage 4 canvas:
// one frame holding the page's SECURITY CHIP (a lock; a red "Not secure";
// "File"), the address itself, and at the right a "Scripts blocked" badge
// (J3 on the JavaScript canvas: a page whose scripts did not run), a zoom
// badge ("125%", shown only off 100%) and the bookmark star.
//
// A CONTAINER, not a text field: the field is the app's own uui_textbox,
// drawn `bare` inside this frame as the router's child, so it keeps its
// focus-ring slot, its editing and its edit menu. A press the field does
// not take -- on the chip, the badge, the star -- is this widget's, and
// arrives as ns_addr_take().
#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"

enum { NS_ADDR_NONE, NS_ADDR_CHIP, NS_ADDR_ZOOM, NS_ADDR_STAR, NS_ADDR_SCRIPTS, NS_ADDR_PARTS };

struct ns_addr {
    int x, y, w, h;
    struct uui_item field;     // the child: the app's textbox
    // The app's to set; then ns_addr_sync().
    int state;                 // enum ns_site_state
    int zoom;                  // percent
    int show_star, starred;
    int scripts_blocked;       // the page has scripts that did not run
    // OWNED: the parts' rects (w 0 = absent), and the pointer.
    int part_x[NS_ADDR_PARTS], part_w[NS_ADDR_PARTS];
    int armed, hot, event;
};

void ns_addr_init(struct ns_addr *a, struct uui_textbox *field, int field_id, const char *field_name);
// Re-places the parts and the field after `state`, `zoom` or the star changed.
void ns_addr_sync(struct ns_addr *a);
// The part a click committed (NS_ADDR_*), once; NS_ADDR_NONE when none.
int ns_addr_take(struct ns_addr *a);
// A part's rect, for anchoring the card under the chip. 0 when absent.
int ns_addr_part_rect(const struct ns_addr *a, int part, int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops ns_addr_ops;

#endif
