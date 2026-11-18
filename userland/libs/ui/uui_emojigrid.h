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

#ifdef __cplusplus
extern "C" {
#endif

struct uui_emojigrid {
    struct uui_grid grid;       // FIRST: the grid's own ops take this pointer
    const int *items;           // entry indices into uemojidata()
    int count;
    int px;                     // the emoji's em
    struct uglyph_face *face;   // NULL: uui_emojigrid_face()
    // The SKIN-TONE strip (uui_emojigrid_variants()): the entry it shows,
    // -1 when shut; its focused form (0 the entry itself, 1..5 the tones);
    // the form a click or Enter took, for uui_emojigrid_take_seq().
    int var_entry, var_index, var_sel;
    int took_entry, took_tone;
};

// `cell` is the smallest cell (columns widen from it), `px` the emoji's size.
void uui_emojigrid_init(struct uui_emojigrid *g, int cell, int px);
// Shows `items` (`count` entry indices); the first is focused.
void uui_emojigrid_set(struct uui_emojigrid *g, const int *items, int count);
// The focused cell's entry, or -1.
int  uui_emojigrid_focused(const struct uui_emojigrid *g);
// The entry a double click or Enter activated since the last call, or -1.
int  uui_emojigrid_take(struct uui_emojigrid *g);

// What was activated since the last call -- Enter or a double click on a
// cell, or a form chosen in the skin-tone strip -- as its entry (-1 for
// none) and its UTF-8 into `out`: the entry's own sequence, or the toned one
// the strip gave. Use this rather than uui_emojigrid_take() where a variant
// can be chosen.
int  uui_emojigrid_take_seq(struct uui_emojigrid *g, char *out, int cap);

// SKIN TONES, GTK's and macOS's shape: a right-click (the caller routes it
// here), the Menu key or Shift+F10 opens a strip over cell `index` (-1: the
// focused one) holding its six forms -- itself and its five tones, light to
// dark. Left/Right and the pointer move in it, Enter or a click takes one,
// Esc or a click elsewhere shuts it. Nothing is remembered. 1 when it
// opened: the entry takes tones.
int  uui_emojigrid_variants(struct uui_emojigrid *g, int index);
int  uui_emojigrid_variants_open(const struct uui_emojigrid *g);
void uui_emojigrid_variants_close(struct uui_emojigrid *g);

// The cell under (cx, cy), or -1.
int  uui_emojigrid_index_at(const struct uui_emojigrid *g, int cx, int cy);

// The emoji face, opened on first use and kept for the process; its `ok` is
// 0 when the fallback list names none or it will not open.
struct uglyph_face *uui_emojigrid_face(void);
// Draws entry `entry` -- in skin tone `tone` (0 none, 1..5) -- centred in
// the rect at `px`, in `face` (NULL: the shared one). 1 when drawn.
int uui_emojigrid_draw_toned(struct ugfx_surface *s, struct uglyph_face *face, int entry, int tone,
                             int px, int x, int y, int w, int h);
int uui_emojigrid_draw_entry(struct ugfx_surface *s, struct uglyph_face *face, int entry, int px,
                             int x, int y, int w, int h);

extern const struct uui_widget_ops uui_emojigrid_ops;

#ifdef __cplusplus
}
#endif

#endif
