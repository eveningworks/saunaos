#ifndef UUI_SWATCHES_H
#define UUI_SWATCHES_H

#include <stdint.h>
#include "ui/uui_widget.h"

// uui_swatches -- A GRID OF COLOURS TO PICK FROM: a paint program's
// palette and its recent colours, Windows Paint's colour box and
// KolourPaint's. Square cells, wrapped to the width it is given; a click
// parks the colour for uui_swatches_take(), the menu bar's arrangement.
//
// A SECONDARY click is the app's -- the toolkit routes only the primary
// button -- so it asks uui_swatches_at() from its own press handler,
// which is how both Paints set the second colour.

struct uui_swatches {
    int x, y, w, h;
    const uint32_t *colours;   // 0xRRGGBB, caller-owned
    int count;
    int cols;        // the natural width's columns; the grid wraps to what it is given
    int selected;    // ringed: the one in use, or -1
    int hot;         // OWNED
    int armed;       // OWNED: pressed, awaiting the release
    int picked;      // OWNED: parked by a release, -1 when none
};

void uui_swatches_init(struct uui_swatches *s, const uint32_t *colours, int count, int cols);
// The list changed under it (recent colours): same storage, new count.
void uui_swatches_set_count(struct uui_swatches *s, int count);
// The cell at (cx, cy), or -1.
int  uui_swatches_at(const struct uui_swatches *s, int cx, int cy);
// A committed click, once: 1 with the colour.
int  uui_swatches_take(struct uui_swatches *s, uint32_t *rgb);

extern const struct uui_widget_ops uui_swatches_ops;

#endif
