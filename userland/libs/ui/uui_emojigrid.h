#ifndef UUI_EMOJIGRID_H
#define UUI_EMOJIGRID_H

// uui_emojigrid -- a GRID OF EMOJI, drawn in colour from the fallback list's
// emoji face (base/ufontfallback.h): the emoji picker's (Super+period) and
// Character Map's Emoji page. A uui_grid (ui/uui_grid.h) whose cells are
// entries of base/uemojidata.h, so it has the grid's layout, scrolling and
// overlay bar, hover, focus and keys: arrows move, Enter or a double click
// activates (uui_emojigrid_take()).
//
// THE CALLER OWNS THE LIST: an array of entry indices (a group, a search's
// matches, the recents), kept alive while the grid shows it.
#include "ui/uui_grid.h"
#include "ui/uglyph.h"

struct uui_emojigrid {
    struct uui_grid grid;       // FIRST: the grid's own ops take this pointer
    const int *items;           // entry indices into uemojidata()
    int count;
    int px;                     // the emoji's em
    struct uglyph_face *face;   // NULL: uui_emojigrid_face()
};

// `cell` is the smallest cell (columns widen from it), `px` the emoji's size.
void uui_emojigrid_init(struct uui_emojigrid *g, int cell, int px);
// Shows `items` (`count` entry indices); the first is focused.
void uui_emojigrid_set(struct uui_emojigrid *g, const int *items, int count);
// The focused cell's entry, or -1.
int  uui_emojigrid_focused(const struct uui_emojigrid *g);
// The entry a double click or Enter activated since the last call, or -1.
int  uui_emojigrid_take(struct uui_emojigrid *g);

// The emoji face, opened on first use and kept for the process; its `ok` is
// 0 when the fallback list names none or it will not open.
struct uglyph_face *uui_emojigrid_face(void);
// Draws entry `entry` centred in the rect at `px`, in `face` (NULL: the
// shared one). 1 when drawn.
int uui_emojigrid_draw_entry(struct ugfx_surface *s, struct uglyph_face *face, int entry, int px,
                             int x, int y, int w, int h);

extern const struct uui_widget_ops uui_emojigrid_ops;

#endif
