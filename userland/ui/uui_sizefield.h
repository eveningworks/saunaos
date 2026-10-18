#ifndef UUI_SIZEFIELD_H
#define UUI_SIZEFIELD_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_segmented.h"
#include "ui/uui_widget.h"

// A SIZE YOU CAN TYPE: a number and its unit, MiB or GiB -- Disk Mark's
// test size and Disks' new partition. KDE Partition Manager's size box
// with its unit picker, in this toolkit's parts: a uui_spinbox for the
// number and a uui_segmented for the unit.
//
// THE VALUE IS IN MiB, ALWAYS, and is DERIVED: the number times the
// unit. There is no third copy to drift from what is drawn.
//
// TWO WAYS TO CHANGE THE UNIT, AND THEY DIFFER ON PURPOSE. Clicking the
// other unit CONVERTS (1024 MiB becomes 1 GiB; 1536 MiB rounds to
// 2 GiB), because the size on screen is what the user already chose.
// Typing a unit letter -- `g` or `m` after the digits -- REINTERPRETS
// the digits being typed ("3" then `g` is 3 GiB), because those digits
// were meant in the unit that follows them.
//
// The range is in MiB too. A GiB the range cannot hold is refused: with
// a maximum under 1 GiB, clicking GiB leaves it on MiB.

struct uui_sizefield {
    int x, y, w, h;
    struct uui_spinbox num;
    struct uui_segmented unit;   // 0 MiB, 1 GiB
    uint64_t min_mib, max_mib;
    int part;                    // OWNED: where the press landed, 1 number, 2 unit
    int disabled;
};

void uui_sizefield_init(struct uui_sizefield *f, uint64_t mib, uint64_t min_mib, uint64_t max_mib);
// Clamps the value into the new range, keeping the unit when it can.
void uui_sizefield_set_range(struct uui_sizefield *f, uint64_t min_mib, uint64_t max_mib);
// Shown in GiB when it is a whole number of them, else in MiB.
void uui_sizefield_set_mib(struct uui_sizefield *f, uint64_t mib);
uint64_t uui_sizefield_mib(const struct uui_sizefield *f);
// Adopts what is typed (ui/uui_spinbox.h's rule). Call before reading
// the value from a button press, or the last thing typed is lost.
int  uui_sizefield_commit(struct uui_sizefield *f);

void uui_sizefield_natural_size(const struct uui_sizefield *f, int *out_w, int *out_h);
void uui_sizefield_set_geometry(struct uui_sizefield *f, int x, int y, int w, int h);
void uui_sizefield_draw(struct ugfx_surface *s, const struct uui_sizefield *f);

extern const struct uui_widget_ops uui_sizefield_ops;

#endif
