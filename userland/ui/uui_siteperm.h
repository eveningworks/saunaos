#ifndef UUI_SITEPERM_H
#define UUI_SITEPERM_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_segmented.h"

// uui_siteperm -- A SITE'S PERMISSION, SHOWN TWO WAYS: as a ROW to set it
// (a caption, then Default / Allow / Block -- Chrome's page-info rows),
// and as an ASK when something was refused (a card under the badge that
// says so, with "Allow on this site" -- Chrome's blocked-content bubble).
// The Web Browser's JavaScript first; images, pop-ups and autoplay are the
// same two shapes. The values are lib/usiteperm.h's.

// --- the row ------------------------------------------------------------

struct uui_siteperm {
    int x, y, w, h;
    int caption_w;               // the caption's column: the caller's, to line up
    const char *label;           // caller-owned ("JavaScript")
    char dflt[32];               // "Default (On)": what Default means today
    const char *opts[3];
    struct uui_segmented seg;    // .selected is the value, USITEPERM_*
};

// `default_on`: whether the global setting allows it, for the Default
// segment's label.
void uui_siteperm_init(struct uui_siteperm *p, const char *label, int value, int default_on);
int  uui_siteperm_value(const struct uui_siteperm *p);
void uui_siteperm_natural_size(const struct uui_siteperm *p, int caption_w, int *out_w, int *out_h);
void uui_siteperm_set_geometry(struct uui_siteperm *p, int x, int y, int caption_w);
// `bg`: the ground it sits on, for the caption's text.
void uui_siteperm_draw(struct ugfx_surface *s, const struct uui_siteperm *p, uint32_t bg);

struct uui_widget_ops;
// As a widget of its own (caption_w 0: the caption's own width). Its
// press, motion and release are the segmented control's; the router names
// a value change with the row's id.
extern const struct uui_widget_ops uui_siteperm_ops;

// --- the ask ------------------------------------------------------------

enum { UUI_PERMASK_NONE, UUI_PERMASK_SECONDARY, UUI_PERMASK_PRIMARY, UUI_PERMASK_CLOSED };

#define UUI_PERMASK_TEXT 128

// An OVERLAY in the app's window, like a menu: while open every press is
// offered to it first, and one outside closes it and goes on to what is
// under it. Esc closes it.
struct uui_permask {
    int open;
    int x, y, w, h;
    char title[UUI_PERMASK_TEXT];
    char line[2][UUI_PERMASK_TEXT];   // "" for none
    char secondary[32], primary[32];  // "" for no secondary button
    // OWNED
    int bx[2], by, bw[2], bh;         // [0] secondary, [1] primary
    int armed, hot, event;            // button + 1, or 0
};

// Opens with its RIGHT edge at `right` and its top at `top` -- under a
// badge at the end of a bar -- kept inside the bounds (the window's
// content, `bound_w` x `bound_h`).
void uui_permask_open(struct uui_permask *a, const char *title, const char *line1,
                      const char *line2, const char *secondary, const char *primary,
                      int right, int top, int bound_w, int bound_h);
void uui_permask_close(struct uui_permask *a);
// What happened since last asked: UUI_PERMASK_* (a button closes it too).
int uui_permask_take(struct uui_permask *a);

extern const struct uui_widget_ops uui_permask_ops;

#endif
