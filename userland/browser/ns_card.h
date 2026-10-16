#ifndef BROWSER_NS_CARD_H
#define BROWSER_NS_CARD_H

// The lock's connection card -- P1 on the stage 4 canvas: the page's
// host and whether its connection is secure; for https, the protocol,
// the cipher and the site's certificate; the cookies the site keeps,
// with Clear; and for a web page, its JavaScript (J2 on the JavaScript
// canvas: Default / Allow / Block). Chrome's page info bubble and
// Firefox's site panel.
//
// Drawn in the window, over the page, as an OVERLAY: while it is open
// every press is offered to it first, and one outside closes it and
// goes on to whatever is under it.
#include "browser/ns_site.h"
#include "ui/uui_siteperm.h"
#include "ui/uui_widget.h"

// NS_CARD_JS: the JavaScript row changed; its value is ns_card_js().
enum { NS_CARD_NONE, NS_CARD_CLEAR, NS_CARD_CLOSED, NS_CARD_JS };

struct ns_card {
    int open;
    int x, y, w, h;
    struct ns_site site;
    // OWNED
    int clear_x, clear_y, clear_w, clear_h;   // w 0: no button
    int has_js;                               // a web page: the row is shown
    struct uui_siteperm js;
    int armed, hot, event;                    // armed 2: a press on the row
};

// Opens under (ax, ay), its left edge on ax, kept inside the bounds
// (the window's content).
void ns_card_open(struct ns_card *c, const struct ns_site *site, int ax, int ay, int bw, int bh);
void ns_card_close(struct ns_card *c);
int ns_card_take(struct ns_card *c);
// The JavaScript row's value, USITEPERM_*.
int ns_card_js(const struct ns_card *c);

extern const struct uui_widget_ops ns_card_ops;

#endif
