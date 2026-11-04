#ifndef UUI_ANCHOR_H
#define UUI_ANCHOR_H

// uui_anchor -- WHICH CORNER, EDGE OR MIDDLE: a 3 x 3 grid of cells, one
// chosen -- Paint.NET's and Photoshop's Canvas Size anchor, and what a
// wallpaper or a watermark placement asks. The chosen cell is filled in
// the accent with arrows in its neighbours pointing away from it, so
// the grid reads as "the picture stays here, the room grows that way".
//
// The value is an index 0..8, row by row: 0 top-left, 4 the middle,
// 8 bottom-right; uui_anchor_xy() splits it into 0..2 per axis. A click
// commits on release; the arrows move it while it has focus.

#include "ui/uui_widget.h"

struct uui_anchor {
    int x, y, w, h;
    int selected;          // 0..8
    int hot, armed;        // OWNED: cells, -1 for none
    int focused;           // OWNED
};

void uui_anchor_init(struct uui_anchor *a, int selected);
// Column and row of the choice, 0 (left/top) .. 2 (right/bottom).
void uui_anchor_xy(const struct uui_anchor *a, int *col, int *row);

extern const struct uui_widget_ops uui_anchor_ops;

#endif
