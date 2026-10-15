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
#include "ui/uui_layerlist.h"
#include "lib/uora.h"

enum paint_tool { TOOL_PENCIL, TOOL_BRUSH, TOOL_ERASER, TOOL_FILL, TOOL_PICK, TOOL_TEXT, TOOL_SHAPES, TOOL_SELECT,
                  TOOL_GRADIENT, TOOL_COUNT };

// The shape bar's gallery, in its order.
enum paint_shape { SHAPE_LINE, SHAPE_CURVE, SHAPE_RECT, SHAPE_ROUNDED, SHAPE_ELLIPSE, SHAPE_POLYGON,
                   SHAPE_TRIANGLE, SHAPE_ARROW, SHAPE_STAR, SHAPE_COUNT };
enum paint_shape_ink { SHAPE_INK_NONE, SHAPE_INK_1, SHAPE_INK_2 };   // an outline's Colour 1 is the fill's Colour 2

// The Brush tool's kinds, the panel's row in its order.
enum paint_brush { BRUSH_ROUND, BRUSH_SQUARE, BRUSH_SOFT, BRUSH_SPRAY, BRUSH_HIGHLIGHT, BRUSH_COUNT };

extern struct uimg *g_layer;           // the current layer's pixels: what every tool paints on
extern struct upixundo g_undo;
extern struct uui_paintcanvas g_canvas;
extern int g_tool;
extern int g_tool_before_pick;         // the picker hands back to this one
extern uint32_t g_colour[2];           // 0xAARRGGBB: primary (left button), secondary (right)
extern int g_brush_kind;               // enum paint_brush
extern int g_brush_size[BRUSH_COUNT];  // each kind keeps its own, pixels
extern int g_soft_hard;                // the soft brush's hardness, percent
extern int g_spray_density;            // percent
extern int g_erase_size, g_erase_hard; // hardness 100: MS Paint's hard square eraser
extern struct uui_fontpick g_fontpick;  // the text tool's face, in its floating bar
extern int g_text_fill;                 // text drawn on a Colour 2 box
extern int g_shape_kind;                // enum paint_shape
extern int g_shape_outline, g_shape_fill;   // enum paint_shape_ink: the bar's two drop-downs
extern int g_shape_width;               // the outline, pixels
extern int g_shape_aa;                  // smooth edges

// The pointer, in picture pixels, with which colour slot is painting
// (0 left, 1 right). DOWN starts a change, UP ends it.
void tools_pointer(enum uui_paint_phase phase, int x, int y, int slot, unsigned mods);   // HOVER too
int  tools_busy(void);                 // a stroke is under way
int  tools_tick(void);                 // the spray, held still: 1 when it painted
// The tool's tip, for the pointer's outline: w x h picture pixels, round
// or square. 0 for a tool with no tip (the fill, the picker...).
int  tools_outline(int *w, int *h, int *round);

// What the window must hear back. Defined in paint.c.
void paint_changed(const struct upix_rect *r);   // pixels changed: the view, redraw, title (NULL: all)
void paint_layers_changed(void);                 // the stack changed: the panel, the canvas
void paint_ask_rasterize(void);                  // a tool pressed on a text layer: ask first
void paint_colour_used(int slot);                // into Recent colours
void paint_colour_picked(int slot);              // the picker set a colour
void paint_tool_changed(void);
void paint_timer(int on);                        // the spray's ticks, while it is held
// A pasted picture is bigger than the canvas (`w` x `h`): ask Expand /
// Keep / Cancel, answered through paste_answer().
void paint_ask_paste(const char *name, int w, int h);
void paint_undo(void);                           // Ctrl+Z, as the menu does it
void paint_text_changed(void);                   // a text box opened, closed or moved

// --- paint_text.c: the text tool ---------------------------------------------
int  text_active(void);
// A press places a box or lets the open one go; over the open box it
// places the caret, selects, moves the box or sets its wrap width.
void text_pointer(enum uui_paint_phase phase, int x, int y, unsigned mods);
int  text_key(int key, unsigned mods);     // 1 when the open box took it
void text_commit(void);                    // the box into the picture, one undo step
void text_restyle(void);                   // the face or the fill changed
int  text_box(int *x, int *y, int *w, int *h);   // the open box, picture pixels
void text_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c);
void text_log_layout(void);                // "paint: layout text ...", for the test
void paint_note(const char *text);                // the status bar's note

// A TEXT LAYER'S WORDS AND HOW THEY ARE SET -- what re-opening one puts
// back in the box and the bar.
#define PAINT_TEXT_MAX 2048
struct paint_text {
    char buf[PAINT_TEXT_MAX];
    int len;
    int x, y, wrap;             // the box's top-left in picture pixels; wrap 0 = grows
    int w, h;                   // the box as last drawn, for a click to find it
    struct uui_fontchoice font;
    uint32_t ink, fill_colour;  // 0xAARRGGBB
    int fill;
};

// --- paint_shapes.c: the shapes tool ------------------------------------------
int  shapes_active(void);                  // a drawn shape is still editable
void shapes_pointer(enum uui_paint_phase phase, int x, int y, unsigned mods);
int  shapes_key(int key);                  // 1 when the open shape took it (Enter, Esc)
void shapes_commit(void);                  // the shape into the picture, one undo step
void shapes_restyle(void);                 // the bar changed: drawn again
void shapes_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c);
void shapes_log_layout(const struct uui_paintcanvas *c);

// --- paint_select.c: the selection ---------------------------------------------
// The Select tool's kinds, its bar's row in that order.
enum paint_sel_kind { SEL_RECT, SEL_ELLIPSE, SEL_LASSO, SEL_WAND, SEL_KIND_COUNT };
extern int g_sel_kind;                     // enum paint_sel_kind
extern int g_sel_mode;                     // enum upix_sel_op: the bar's New / Add / Subtract
extern int g_sel_tol;                      // the wand's tolerance, percent
int  select_any(void);
const struct upix_sel *select_get(void);   // the mask while something is selected, else NULL
void select_pointer(enum uui_paint_phase phase, int x, int y, unsigned mods);
// A tool painted `r` inside an open undo step: everything outside the
// selection in `r` put back from the step's copy. Nothing when none is.
void select_clip(const struct upix_rect *r);
void select_all(void);
void select_none(void);
void select_invert(void);
void select_delete(void);                  // the selected pixels cleared, one step
void select_crop(void);                    // the canvas cut to the selection's box, one step
int  select_copy(int cut);                 // onto the clipboard; 1 when there was a selection
int  select_key(int key, unsigned mods);   // Esc deselects, Delete deletes: 1 when taken
void select_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c);
void select_log_layout(const struct uui_paintcanvas *c);
// The status bar's pane: "Selection 241 x 141 at 180, 220", the size of
// one being dragged out, or empty.
void select_status(char *out, int cap);

// --- paint_gradient.c: the Gradient tool, and the bucket's gradient -------------
extern int g_grad_shape;                   // enum upix_grad: the bar's row, in its order
extern int g_grad_clear;                   // Transparency: Colour 1 fading to nothing
extern int g_grad_dither;
extern int g_fill_gradient;                // the bucket fills with the gradient, not Colour 1
int  gradient_active(void);                // ends still draggable
void gradient_pointer(enum uui_paint_phase phase, int x, int y, unsigned mods);
// The bucket's gradient: the region it would fill, the press to the release.
void gradient_bucket(enum uui_paint_phase phase, int x, int y, int slot);
int  gradient_key(int key);                // Enter keeps it, Esc drops it
void gradient_commit(void);                // into the layer, one undo step
void gradient_restyle(void);               // the bar or the colours changed
void gradient_flush(void);                 // the frame's render of what moved: from on_draw
void gradient_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c);
void gradient_log_layout(const struct uui_paintcanvas *c);

// --- paint_frame.c: a box being edited, its dashed frame and handles ------------
#define PAINT_HANDLE_PX 8                  // a handle, screen pixels
float paint_handle_band(void);             // a handle's reach, picture pixels at this zoom
// A box's eight handles: the corners clockwise from top-left, then the
// sides' middles from the top. Returns 8.
int  paint_box_handles(float x0, float y0, float x1, float y1, struct upix_pt *out);
int  paint_handle_at(const struct upix_pt *h, int n, float x, float y);   // -1: none
int  paint_handle_cursor(int h);           // a box handle's WIN_CURSOR_*
void paint_draw_frame(struct ugfx_surface *s, const struct uui_paintcanvas *c, float x0, float y0, float x1, float y1);
void paint_draw_handles(struct ugfx_surface *s, const struct uui_paintcanvas *c, const struct upix_pt *h, int n);
// "paint: layout <what>.handle i x y w h", screen pixels, for a test.
void paint_log_handles(const struct uui_paintcanvas *c, const char *what, const struct upix_pt *h, int n);

// --- paint_paste.c: a pasted picture, floating until it is let go ---------------
int  paste_active(void);
// Ctrl+V / Ctrl+Shift+V: the clipboard's picture, or the first picture
// file on it. Bigger than the canvas, it asks first (paint_ask_paste()).
void paste_from_clipboard(int new_layer);
void paste_from_file(const char *path, int new_layer);
// The question's answer: PASTE_EXPAND grows the canvas to fit, PASTE_KEEP
// floats it over the edge, PASTE_CANCEL forgets it.
enum paste_fit { PASTE_EXPAND, PASTE_KEEP, PASTE_CANCEL };
void paste_answer(enum paste_fit fit);
// A selection lifted to move: `im` (taken) floats at (x, y), the step that
// cleared it joined to the one that places it.
void paste_float(struct uimg *im, int x, int y);
void paste_pointer(enum uui_paint_phase phase, int x, int y, unsigned mods);
int  paste_key(int key);                   // Enter keeps it, Esc drops it
void paste_commit(void);                   // into the layer, one undo step
void paste_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c);
void paste_log_layout(const struct uui_paintcanvas *c);

// --- paint_layers.c: the layer stack -----------------------------------------
// Bottom first. Every change to the stack is one undo step; choosing the
// current layer is not.
#define PAINT_LAYERS_MAX 32
#define PAINT_LAYER_NAME 48
const struct uimg *layers_view(void);            // the picture as shown: what the canvas draws, saves flatten
void layers_update(const struct upix_rect *r);   // pixels in r changed (NULL: all)
int  layers_count(void);
int  layers_active(void);
int  layers_opacity(int i);
const char *layers_name(int i);
const struct uui_layerrow *layers_rows(void);    // for ui/uui_layerlist.h, bottom first
// A new picture: one layer, "Background", taking over `bottom`; the
// history cleared. 0 on no memory.
int  layers_reset(struct uimg *bottom);
int  layers_reset_many(struct uora_layer *ls, int n);   // an opened .ora; takes the pixels
int  layers_export(struct uora_layer *out);      // shallow: the pixels stay the stack's, the attrs are new
void layers_export_free(struct uora_layer *out, int n);   // ...and freed here
void layers_select(int i);
void layers_add(void);
void layers_duplicate(void);
void layers_delete(void);
void layers_move(int from, int to);
void layers_merge_down(void);
void layers_flatten(void);
void layers_set_hidden(int i, int hidden);
void layers_rename(int i, const char *name);
void layers_opacity_preview(int v);               // a slider drag, shown...
void layers_opacity_commit(void);                 // ...and kept as one step
// TEXT LAYERS. layers_text() is a layer's words, or NULL for pixels;
// layers_text_at() the topmost shown text layer whose box holds (x, y).
const struct paint_text *layers_text(int i);
int  layers_text_at(int x, int y);
// A box being edited draws into a LIVE layer: a new one above the
// current (`reopen` < 0), or text layer `reopen` emptied to be drawn
// again. g_layer is then that layer. live_end() keeps it as ONE step with
// `t`'s words -- or, with NULL (nothing typed), puts the stack back as it
// was (a reopened layer emptied is deleted, as a step). 0 on no memory.
int  layers_live_begin(int reopen);
void layers_live_end(const struct paint_text *t);
void layers_rasterize(int i);                     // a text layer becomes pixels, as a step
// Every layer replaced by fn's picture of it (index 0 the bottom) -- a
// rotate, a resize -- as one step; text layers become pixels. 0 when one
// failed and nothing changed.
typedef int (*layers_fn)(const struct uimg *in, struct uimg *out, int index, void *ctx);
int  layers_transform(layers_fn fn, void *ctx, const char *what);

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
