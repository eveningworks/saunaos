#ifndef UUI_RESIZEBAR_H
#define UUI_RESIZEBAR_H

// uui_resizebar -- ONE REGION OF A RUN, ITS END DRAGGED: a partition on a
// small map of its disk, what is used in it hatched, a handle on its end,
// the free space after it dashed and what lies beyond drawn dim. GParted's
// and KDE Partition Manager's resize bar; Disks' Resize dialog, and the
// installer's "install beside another system" (which drags the end of
// the partition it shrinks).
//
// IT NAMES NO DISK: a run of any unit (sectors, for both callers), a
// fixed start, an end between two limits. What the numbers mean, and the
// text fields that echo them, are the caller's.
//
// The end is THE VALUE. A drag moves it (the press may land anywhere on
// the bar: the end jumps there, and follows); Left/Right step it by
// `step`, Page Up/Down by ten steps, Home/End to the limits. The caller
// reads `end` in on_widget, which a drag's release and every key reach.
#include <stdint.h>
#include "ui/uui_widget.h"

struct uui_resizebar {
    int x, y, w, h;
    // The run drawn, [0, span), in one unit. [0, start) is what lies
    // before the region, drawn dim (a neighbour, the disk's start); so is
    // [max_end, span).
    uint64_t span, start;
    uint64_t end;                 // the value: start < min_end <= end <= max_end <= span
    uint64_t min_end, max_end;
    uint64_t used;                // from `start`, hatched; 0 for none
    uint64_t step;                // a key's move, and a drag's snap; 0 = 1
    const char *label;            // in the region, on the hatching ("used 9.6 GiB"); NULL none
    int disabled;                 // drawn, not draggable: a region that cannot be resized
    // OWNED
    int dragging, hot, focused;
};

void uui_resizebar_init(struct uui_resizebar *b);
// The run and its limits; `end` is clamped into them and snapped.
void uui_resizebar_set(struct uui_resizebar *b, uint64_t span, uint64_t start, uint64_t end,
                       uint64_t min_end, uint64_t max_end, uint64_t used);
// Moves the end, clamped and snapped; 1 if it moved.
int  uui_resizebar_set_end(struct uui_resizebar *b, uint64_t end);
// The handle's rect, content-relative (a test drags it).
void uui_resizebar_handle(const struct uui_resizebar *b, int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops uui_resizebar_ops;

#endif
