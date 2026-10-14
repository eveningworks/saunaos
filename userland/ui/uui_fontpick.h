#ifndef UUI_FONTPICK_H
#define UUI_FONTPICK_H

// uui_fontpick -- A FONT, CHOSEN: the family from what /usr/share/fonts
// holds, a size in pixels, Bold and Italic -- Windows 11 Paint's text
// bar and every word processor's. One row of standard controls (a
// dropdown, a spinbox, B I U S toggles -- ui/uui_togglebar.h), uui_colourpick's
// arrangement: embed uui_fontpick_item(), add the focusables, offer each
// on_widget id to uui_fontpick_on_widget() first.
//
// It names a FILE, never loads one: uui_fontpick_path() is the .ttf for
// the choice (its -bold / -italic / -bold-italic face when the family
// has one), and whoever draws loads it at the size (ugfx_font_load()).
// Underline and strikethrough are lines the drawer adds; a face has none.

#include "ui/uui_widget.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_layout.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_togglebar.h"
#include "ui/uui_focus.h"

#define UUI_FONTPICK_IDS 3
#define UUI_FONTPICK_FAMILIES 8

struct uui_fontpick {
    int id_base;
    int bold, italic, underline, strike;
    int nfam;
    const char *labels[UUI_FONTPICK_FAMILIES];   // "Liberation Sans"
    const char *files[UUI_FONTPICK_FAMILIES];    // "liberation-sans"
    struct uui_dropdown family;
    struct uui_spinbox size;
    struct uui_togglebar style;   // B I U S
    struct uui_item it[3];
    struct uui_layout row;
};

// `family` names a file stem ("liberation-sans"); an unknown one is the
// first installed.
void uui_fontpick_init(struct uui_fontpick *p, int id_base, const char *family, int px,
                       int bold, int italic);
struct uui_item uui_fontpick_item(struct uui_fontpick *p);
int  uui_fontpick_focusables(struct uui_fontpick *p, struct uui_focusable *out, int max);
// 1 when `id` was one of its controls AND the font changed.
int  uui_fontpick_on_widget(struct uui_fontpick *p, int id);
int  uui_fontpick_px(const struct uui_fontpick *p);
// The face's file, and whether it must be emboldened by the rasterizer
// (Bold chosen, the family has no bold file). 1, or 0 when none fits.
int  uui_fontpick_path(const struct uui_fontpick *p, char *out, int cap, int *embolden);

#endif
