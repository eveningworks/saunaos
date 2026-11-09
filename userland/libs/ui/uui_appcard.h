#ifndef UUI_APPCARD_H
#define UUI_APPCARD_H

// uui_appcard -- INSTALLABLE THINGS, each a card or a row: an icon tile,
// a name and its kind, a summary, a licence badge, a size, one button and
// a pending state ("Will be removed"). The Software app's store cards and
// its list (docs/software-design.md); anything that lists what can be
// added or taken away -- GNOME Software's and KDE Discover's tiles,
// Windows' Optional features list -- in the two layouts of one widget.
//
// THE ITEMS ARE THE CALLER'S ARRAY, read as the view paints: the widget
// owns the layout, the scrolling (the overlay bar, uui_sbar), the
// selection, hover and the keys. A click on an item's BUTTON is parked
// for uui_appcards_take_button(); a click elsewhere selects, a double
// click or Enter is parked for uui_appcards_take_activate(), Space
// presses the selected item's button.
//
// CARDS fill the width in equal columns (as many as the narrowest card
// fits, then widened), and rows scroll; ROWS is a table-like list --
// tile and name, Licence, Size, Status, the button -- under a header
// that stays put. Every size is the font's (ugfx_px() for the gaps).

#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_sbar.h"

enum { UUI_APPCARD_CARDS = 0, UUI_APPCARD_ROWS };

// What the button does, which is what colours it.
enum { UUI_APPCARD_BTN_ADD = 0,     // accent fill: Install
       UUI_APPCARD_BTN_REMOVE,      // danger text on the ground: Remove
       UUI_APPCARD_BTN_PLAIN };     // a plain control: Keep, Cancel

struct uui_appcard {
    const char *name;
    const char *kind;       // "Internet · NetSurf", or NULL
    const char *summary;    // CARDS only: up to two lines, the rest elided
    const char *badge;      // the licence, short ("GPL-2.0+"), or NULL
    int badge_warm;         // a copyleft licence: the warm badge, else the green
    const char *size;       // "8.0 MB", or NULL
    const char *status;     // ROWS' Status column ("Installed"), or NULL
    const char *button;     // "Remove", or NULL for none
    int button_kind;        // UUI_APPCARD_BTN_*
    int button_greyed;      // drawn, and does nothing
    const char *pending;    // "Will be removed": the item is marked, or NULL
    int pending_danger;     // ...in the danger colour (a removal), else the accent
    const char *icon;       // an icon NAME (media/icon_cache.h), or NULL
    const char *glyph;      // the tile's letters when there is no icon
    uint32_t tile;          // the tile's colour behind the glyph
};

struct uui_appcards {
    int x, y, w, h;
    int layout;                       // UUI_APPCARD_*
    const struct uui_appcard *items;  // caller-owned
    int count;
    const char *empty;                // drawn when count is 0, or NULL
    int selected;                     // -1 for none
    // OWNED
    int top;                          // pixels scrolled
    int hot, hot_button, armed, armed_button, focused;
    int pressed, activated;           // parked: a button click, an activation
    int last_click;
    uint64_t last_click_ns;
    struct uui_sbar sb;
};

// MUST START ZEROED, as a static is.
void uui_appcards_init(struct uui_appcards *v, int layout);
// A new array (or the same one changed): the selection and scroll are
// kept where they still fit.
void uui_appcards_set(struct uui_appcards *v, const struct uui_appcard *items, int count);
void uui_appcards_set_layout(struct uui_appcards *v, int layout);
// Selects `index` (or -1) and scrolls it into view.
void uui_appcards_select(struct uui_appcards *v, int index);
// The item whose button was clicked (or pressed with Space), or -1.
int  uui_appcards_take_button(struct uui_appcards *v);
// The item double-clicked or Entered, or -1.
int  uui_appcards_take_activate(struct uui_appcards *v);
int  uui_appcards_columns(const struct uui_appcards *v);
// Where item `i` is, and its button; 0 when it is scrolled out of view.
int  uui_appcards_item_rect(const struct uui_appcards *v, int i, int *x, int *y, int *w, int *h);
int  uui_appcards_button_rect(const struct uui_appcards *v, int i, int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops uui_appcards_ops;

#endif
