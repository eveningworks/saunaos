#ifndef UUI_HEXVIEW_H
#define UUI_HEXVIEW_H

// uui_hexview -- BYTES AS A GRID: an offset column, the bytes in hex in
// groups of eight, and the same bytes as text -- HxD's, ImHex's and
// Okteta's main view, for any caller that shows bytes (the Hex Viewer, a
// crash report's memory, a device's registers, a disk's sectors).
//
// **IT HOLDS NO BYTES.** They are read through the caller's callback,
// a row at a time as rows are drawn, so a file of any size scrolls: the
// caller's reader caches (base/urange.h), the widget asks. A row offset is
// 64-bit; the scrollbar counts rows scaled down to fit its int.
//
// THE CURSOR AND THE SELECTION ARE THE WIDGET'S: a click puts the cursor
// on a byte, a drag or Shift selects from the anchor to it, and the
// selection fills BOTH columns in step -- the design language's soft fill
// with a 1px edge, the cursor's byte wearing the full accent edge in the
// column the caret is in (Tab moves it). The caller paints what the bytes
// MEAN under them: a tint per byte (a structure's regions) and find hits,
// and selection outranks both. The bytes are the selectable text; the
// copy is the caller's, from uui_hexview_selection().
//
// THE RIGHT-CLICK MENU IS THE CALLER'S ROWS (`menu`, greyed by
// `menu_flags` -- the same function its menu bar asks); a pick is parked
// for uui_hexview_take_cmd(). Keys arrive through uui_hexview_key(),
// called from the app's on_key, as a terminal's do.
#include <stddef.h>
#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_sbar.h"
#include "ui/uui_menubar.h"

typedef size_t (*uui_hexview_read_fn)(void *ctx, uint64_t off, void *buf, size_t n);
// A byte's wash, 0xRRGGBB, or UUI_HEXVIEW_NO_TINT.
typedef uint32_t (*uui_hexview_tint_fn)(void *ctx, uint64_t off);
#define UUI_HEXVIEW_NO_TINT 0xFF000000u
#define UUI_HEXVIEW_ROW_MAX 64      // bytes a row may show

struct uui_hexview {
    int x, y, w, h;
    uint64_t size;
    uui_hexview_read_fn read;
    void *read_ctx;
    int per_row;                       // a multiple of 8, up to UUI_HEXVIEW_ROW_MAX
    const struct ugfx_font *font;      // the grid's MONOSPACE face; NULL = whatever is current
    uui_hexview_tint_fn tint;
    void *tint_ctx;
    // Find hits: each `hit_len` bytes long, sorted by start; `hit_cur`
    // the one Find is on (-1 none). The caller's array.
    const uint64_t *hits;
    int nhits, hit_len, hit_cur;
    const struct uui_menu_item *menu;  // the right-click rows, the caller's
    int menu_count;
    unsigned (*menu_flags)(int code);
    int focused;                       // the caret's full accent edge; the caller's to keep

    uint64_t top;                      // the first row shown
    uint64_t cursor, anchor;           // the selection is anchor..cursor inclusive, when has_sel
    int has_sel;
    int text_col;                      // the caret is in the text column

    // OWNED
    struct uui_sbar bar;
    int shift;                         // the bar counts rows >> shift
    int dragging;
    int changed;                       // the cursor, selection or view moved
    int cmd;                           // a parked right-click pick
};

void uui_hexview_init(struct uui_hexview *v);
// A new source: the cursor and the view go back to the start.
void uui_hexview_set_source(struct uui_hexview *v, uint64_t size, uui_hexview_read_fn read, void *ctx);
// 16 or 32 bytes a row (any multiple of 8 up to the max); the cursor's row stays in view.
void uui_hexview_set_per_row(struct uui_hexview *v, int n);
// The cursor to `off`, with `len` bytes from there selected (0: none),
// scrolled into view.
void uui_hexview_goto(struct uui_hexview *v, uint64_t off, uint64_t len);
// The selection, inclusive at both ends. 1 when there is one.
int  uui_hexview_selection(const struct uui_hexview *v, uint64_t *lo, uint64_t *len);
// A key from the app's on_key: the arrows, Page Up/Down, Home/End (the
// row), Ctrl+Home/End (the file), Shift to extend, Tab between columns.
// 1 when taken.
int  uui_hexview_key(struct uui_hexview *v, int key, unsigned mods);
int  uui_hexview_visible_rows(const struct uui_hexview *v);
// Where byte `off` is drawn in the hex (`text` 0) or text column; 0 when
// it is not on screen.
int  uui_hexview_byte_rect(const struct uui_hexview *v, uint64_t off, int text,
                           int *x, int *y, int *w, int *h);
// The width the grid needs at `per_row` bytes a row, scrollbar included.
int  uui_hexview_width_for(const struct uui_hexview *v, int per_row);
// The parked right-click pick, or 0. Taken once.
int  uui_hexview_take_cmd(struct uui_hexview *v);
// 1 once after the cursor, the selection or the view moved.
int  uui_hexview_take_changed(struct uui_hexview *v);

extern const struct uui_widget_ops uui_hexview_ops;

#endif
