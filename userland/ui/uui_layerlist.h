#ifndef UUI_LAYERLIST_H
#define UUI_LAYERLIST_H

// uui_layerlist -- A LAYER STACK, the top layer the top row: an eye that
// shows or hides it, a thumbnail on a checkerboard, its name and its
// opacity -- Krita's Layers docker, Paint.NET's Layers window, GIMP's.
// One row is current, filled in the accent; a row dragged up or down is
// a new order, shown by an insertion line while it moves.
//
// THE ROWS ARE THE CALLER'S, BOTTOM FIRST -- index 0 is the bottom
// layer, drawn as the last row -- the order a layered picture keeps
// them in. The list changes nothing itself: what a click did waits for
// uui_layerlist_take(), the menu bar's arrangement, and the caller
// changes its stack and the rows. Thumbnails are sampled from each
// row's pixels as it draws, so a stroke shows in its row with nothing
// to invalidate.

#include "lib/uimg.h"
#include "ui/uui_sbar.h"
#include "ui/uui_widget.h"

struct uui_layerrow {
    const char *name;          // not owned
    const struct uimg *img;    // the layer's pixels, for its thumbnail; not owned
    int opacity;               // 0..255
    int hidden;
    int text;                  // a text layer: a "T" on its thumbnail
};

enum uui_layer_event {
    UUI_LAYER_NONE,
    UUI_LAYER_PICK,     // a row clicked: make it current
    UUI_LAYER_EYE,      // its eye clicked: show or hide it
    UUI_LAYER_MOVE,     // a row dragged to `to` (bottom-first index it should end up at)
    UUI_LAYER_RENAME,   // a row double-clicked
};

struct uui_layerlist {
    int x, y, w, h;
    const struct uui_layerrow *rows;   // the caller's, bottom first
    int count;
    int current;                       // the caller's: the current row's index
    // --- OWNED --------------------------------------------------------
    struct uui_sbar sb;
    int top;                           // the first row shown, counted from the TOP row
    int hot;                           // the row under the pointer, or -1
    int press, eye_press;              // the row pressed (its body / its eye), or -1
    int press_y, dragging, drop_at;    // a drag: where it began, under way, the gap it would go in
    unsigned long long last_ns;        // the last click, for a double click
    int last_row;
    enum uui_layer_event ev;
    int ev_row, ev_to;
};

void uui_layerlist_init(struct uui_layerlist *l);
int  uui_layerlist_row_h(void);
// The last thing a click did, once: its event, the row, and for a move
// where it goes (bottom-first). UUI_LAYER_NONE when there is nothing.
enum uui_layer_event uui_layerlist_take(struct uui_layerlist *l, int *row, int *to);

extern const struct uui_widget_ops uui_layerlist_ops;

#endif
