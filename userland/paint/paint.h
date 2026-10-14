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
#include "ui/uui_fontpick.h"

enum paint_tool { TOOL_PENCIL, TOOL_BRUSH, TOOL_ERASER, TOOL_FILL, TOOL_PICK, TOOL_TEXT, TOOL_COUNT };

extern struct uimg g_img;              // the picture
extern struct upixundo g_undo;
extern struct uui_paintcanvas g_canvas;
extern int g_tool;
extern int g_tool_before_pick;         // the picker hands back to this one
extern uint32_t g_colour[2];           // 0xAARRGGBB: primary (left button), secondary (right)
extern int g_brush;                    // brush and eraser size, in pixels
extern enum upix_shape g_shape;
extern struct uui_fontpick g_fontpick;  // the text tool's face, in its floating bar
extern int g_text_fill;                 // text drawn on a Colour 2 box

// The pointer, in picture pixels, with which colour slot is painting
// (0 left, 1 right). DOWN starts a change, UP ends it.
void tools_pointer(enum uui_paint_phase phase, int x, int y, int slot);
int  tools_busy(void);                 // a stroke is under way

// What the window must hear back. Defined in paint.c.
void paint_changed(const struct upix_rect *r);   // pixels changed: redraw, title
void paint_colour_used(int slot);                // into Recent colours
void paint_colour_picked(int slot);              // the picker set a colour
void paint_tool_changed(void);
void paint_text_changed(void);                   // a text box opened, closed or moved

// --- paint_text.c: the text tool ---------------------------------------------
int  text_active(void);
void text_pointer_down(int x, int y);      // place a box, or let the open one go
int  text_key(int key);                    // 1 when the open box took it
void text_commit(void);                    // the box into the picture, one undo step
void text_restyle(void);                   // the face or the fill changed
int  text_box(int *x, int *y, int *w, int *h);   // the open box, picture pixels
void text_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c);
// The picture replaced by `out` (a rotate, a resize), as ONE undo step;
// `out` is taken over.
void paint_replace_picture(struct uimg *out);
void paint_note(const char *text);                // the status bar's note

// --- paint_resize.c: Image > Resize, two tabs ------------------------------
struct uapp;
struct uui_dialog;
void resize_init(void);
void resize_open(struct uapp *a, int canvas_tab);   // 0 Resize picture, 1 Canvas size
int  resize_is_open(void);
// The dialog's widget ids are PAINT_RESIZE_ID .. +PAINT_RESIZE_IDS-1.
#define PAINT_RESIZE_ID  60
#define PAINT_RESIZE_IDS 12
int  resize_on_widget(int id, int reason);           // 1 when it was the dialog's
int  resize_key(int key);                            // 1 when the dialog took it
int  resize_take(void);                              // the dialog's own id was named
// The canvas re-framed to w x h, the old picture at anchor (col, row) of
// 0..2 each, the new room in Colour 2 -- one undo step.
void resize_canvas_to(int w, int h, int col, int row);
struct uui_dialog *resize_dialog(void);

#endif
