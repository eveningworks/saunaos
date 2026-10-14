#ifndef PAINT_H
#define PAINT_H

// Paint's units share their state through here, the File Manager's
// fm_internal.h arrangement: gui/apps/paint.c is the window -- menus,
// bars, the panel, files -- and paint_tools.c turns the pointer into
// pixels. The pixel work itself is lib/upixel.h's and the history
// lib/upixundo.h's, so neither is Paint's alone.

#include <stdint.h>
#include "lib/uimg.h"
#include "lib/upixel.h"
#include "lib/upixundo.h"
#include "ui/uui_paintcanvas.h"

enum paint_tool { TOOL_PENCIL, TOOL_BRUSH, TOOL_ERASER, TOOL_FILL, TOOL_PICK, TOOL_COUNT };

extern struct uimg g_img;              // the picture
extern struct upixundo g_undo;
extern struct uui_paintcanvas g_canvas;
extern int g_tool;
extern int g_tool_before_pick;         // the picker hands back to this one
extern uint32_t g_colour[2];           // 0xAARRGGBB: primary (left button), secondary (right)
extern int g_brush;                    // brush and eraser size, in pixels
extern enum upix_shape g_shape;

// The pointer, in picture pixels, with which colour slot is painting
// (0 left, 1 right). DOWN starts a change, UP ends it.
void tools_pointer(enum uui_paint_phase phase, int x, int y, int slot);
int  tools_busy(void);                 // a stroke is under way

// What the window must hear back. Defined in paint.c.
void paint_changed(const struct upix_rect *r);   // pixels changed: redraw, title
void paint_colour_used(int slot);                // into Recent colours
void paint_colour_picked(int slot);              // the picker set a colour
void paint_tool_changed(void);

#endif
