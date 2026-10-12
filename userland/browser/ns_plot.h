#ifndef BROWSER_NS_PLOT_H
#define BROWSER_NS_PLOT_H

// NetSurf's plotter table, drawing onto a ugfx surface. A redraw passes
// a target: the surface, and the view's rectangle in surface pixels,
// which every clip NetSurf asks for is cut to.
#include "ui/ugfx.h"

struct ns_target {
    struct ugfx_surface *s;
    int cx0, cy0, cx1, cy1;   // the view: [cx0, cx1) x [cy0, cy1)
};

struct redraw_context;
// Fills `ctx` to draw through these plotters into `t`, which must
// outlive the redraw.
void ns_plot_context(struct redraw_context *ctx, struct ns_target *t);

#endif
