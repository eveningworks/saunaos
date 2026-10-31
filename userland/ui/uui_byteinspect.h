#ifndef UUI_BYTEINSPECT_H
#define UUI_BYTEINSPECT_H

// uui_byteinspect -- A DATA INSPECTOR: what the bytes at the cursor are as
// each type a hex editor's inspector reads them as (HxD's Data Inspector,
// ImHex's, Okteta's Decoding Table) -- binary, integers of each width,
// floats, a UTF-8 character -- in either byte order, the value as an
// offset to go to, and the selection with its copy and save buttons.
// The maths is lib/ubyteval.h; this draws it and takes the clicks.
//
// THE CALLER HANDS IT BYTES (uui_byteinspect_set(), up to 8 from the
// cursor, fewer near the end -- a row that needs more says so) and the
// selection's first few for its preview. What a person did is PARKED as
// an event (uui_byteinspect_take()): the byte order switched, Go to, Copy
// as hex, Copy as C array, Save as -- the caller owns the file and the
// clipboard's big copies. A value row's own right-click menu copies that
// value, here.
#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_segmented.h"
#include "ui/uui_button.h"

enum uui_byteinspect_event {
    UUI_BI_NONE = 0,
    UUI_BI_ENDIAN,      // `big_endian` changed
    UUI_BI_GOTO,        // go to uui_byteinspect_offset()
    UUI_BI_COPY_HEX,
    UUI_BI_COPY_C,
    UUI_BI_SAVE,
};

#define UUI_BYTEINSPECT_PREVIEW 8

struct uui_byteinspect {
    int x, y, w, h;
    const struct ugfx_font *mono;        // the values' face; NULL = the current one
    int have;                            // there are bytes (a file is open)
    uint64_t at, size;                   // the cursor's offset; the file's length
    uint8_t bytes[8];
    int avail;                           // how many of `bytes` the file has there
    int big_endian;
    int has_sel;
    uint64_t sel_lo, sel_len;
    uint8_t preview[UUI_BYTEINSPECT_PREVIEW];
    int preview_n;
    int save_ok;                         // Save as... can act (the caller's sandbox allows it)

    // OWNED
    struct uui_segmented seg;
    struct uui_button btn[3];            // Copy as hex, Copy as C array, Save as...
    int hot, armed;                      // a part (below), or -1
    int event;
    int menu_row;                        // the row a right-click menu is about
};

void uui_byteinspect_init(struct uui_byteinspect *bi);
void uui_byteinspect_set(struct uui_byteinspect *bi, uint64_t at, const uint8_t *bytes, int avail,
                         uint64_t size);
void uui_byteinspect_set_selection(struct uui_byteinspect *bi, int has, uint64_t lo, uint64_t len,
                                   const uint8_t *preview, int n);
// The value at the cursor read as an offset into the file (lib/ubyteval.h). 1 with `*out`.
int  uui_byteinspect_offset(const struct uui_byteinspect *bi, uint64_t *out);
// The last event, taken once.
int  uui_byteinspect_take(struct uui_byteinspect *bi);

extern const struct uui_widget_ops uui_byteinspect_ops;

#endif
