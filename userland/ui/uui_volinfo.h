#ifndef UUI_VOLINFO_H
#define UUI_VOLINFO_H

// uui_volinfo -- a mounted volume's or network share's card, drawn from
// a struct uvolinfo (lib/uvolinfo.h): its icon, name and kind, its facts
// as label/value rows, and how full it is. Properties shows the FULL
// form, a used/free RING under the facts (Explorer's drive sheet); the
// File Manager's details pane the COMPACT one, centred, with a bar.
//
// A reading, not a control: no `hit`, so a click passes through.

#include <stdint.h>
#include "ui/ugfx.h"
#include "lib/uvolinfo.h"

struct uui_volinfo {
    int x, y, w, h;
    const struct uvolinfo *vi;   // caller-owned; NULL draws nothing
    int compact;
    uint32_t bg;                 // 0 = the theme's panel (compact: a step lighter)
};

void uui_volinfo_init(struct uui_volinfo *w, const struct uvolinfo *vi, int compact);
// The height it needs at `width`.
int  uui_volinfo_height(const struct uui_volinfo *w, int width);
// The colour its meter is drawn in: danger near full, else by kind
// (a share teal, RAM violet, read-only orange, a disk green).
uint32_t uui_volinfo_colour(const struct uvolinfo *vi);
void uui_volinfo_draw(struct ugfx_surface *s, const struct uui_volinfo *w);
// The ring's bounding square (the full form, a known size): 1, else 0.
int  uui_volinfo_ring_rect(const struct uui_volinfo *w, int *x, int *y, int *d);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_volinfo_ops;

#endif
