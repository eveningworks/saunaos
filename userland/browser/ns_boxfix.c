// A fix to NetSurf's box_coords(), as a link-time wrap rather than an
// edit to the vendored source (the Makefile's --wrap, as ns_find.c).
//
// THE BUG. box_coords() walks a box's ancestors and, for a float, jumps
// to its `float_container` -- with only an assert (compiled out here)
// that one exists. A float layout never placed has none: NetSurf
// assigns it while laying out floats in an inline container
// (layout.c), and box_manipulate.c starts every box at NULL. An image
// that finishes AFTER the page's layout (CONTENT_MSG_DONE in
// html/object.c) asks for its box's position to redraw it, and a float
// with no container on the way up dereferenced NULL. Measured
// 2026-10-13: Wikipedia's Main Page in a ~1100 px window over HTTPS on
// the ASUS, and in QEMU with every image delayed 3 s.
//
// THE FIX takes the parent instead -- the box the float sits in, which
// is where an unplaced float would be drawn from anyway. The trap: ld
// wraps only references from OTHER objects, so box_bounds() inside
// box_inspect.c still calls the unwrapped original.
#include <stdbool.h>
#include <stddef.h>

#include <dom/dom.h>

#include "utils/errors.h"
#include "netsurf/types.h"
#include "netsurf/mouse.h"
#include "content/handlers/html/box.h"
#include "desktop/scrollbar.h"

#include "ui/ulog.h"

void __wrap_box_coords(struct box *box, int *x, int *y) {
    static int logged;
    *x = box->x;
    *y = box->y;
    while (box->parent) {
        bool fl = box->type == BOX_FLOAT_LEFT || box->type == BOX_FLOAT_RIGHT;
        if (fl && box->float_container) {
            box = box->float_container;
        } else {
            if (fl && !logged++)
                ulogf("browser: a float with no container (parent type %d) -- taking its parent\n",
                      (int)box->parent->type);
            box = box->parent;
        }
        *x += box->x - scrollbar_get_offset(box->scroll_x);
        *y += box->y - scrollbar_get_offset(box->scroll_y);
    }
}
