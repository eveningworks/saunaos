#ifndef BROWSER_NS_FONT_H
#define BROWSER_NS_FONT_H

// Text for NetSurf -- the Web Browser's faces, the measuring table its
// layout calls, and drawing a run. TTF faces from /usr/share/fonts
// through lib/ttf.h's metrics and ui/uglyph.h's renderer.
#include <stddef.h>
#include <stdint.h>
#include "ui/ugfx.h"
#include "utils/errors.h"
#include "netsurf/plot_style.h"

struct gui_layout_table;
extern struct gui_layout_table *ns_layout_table;

// Opens the faces; 0 when none would, and every run then draws as boxes.
int ns_font_init(void);
// The pixel size NetSurf's point size means, at its DPI (ns_font.c).
int ns_font_px(const plot_font_style_t *st);
// A run of `len` bytes of UTF-8 at `baseline`; returns its advance --
// the width the layout table measured for it.
int ns_font_draw(struct ugfx_surface *s, const plot_font_style_t *st, int x, int baseline,
                 const char *text, size_t len, uint32_t color);

#endif
