// A menu taller than it may be scrolls (fixed-tables round 4, track B;
// Win32's and GTK's menus): a level of 60 rows in bounds that hold about
// a dozen is capped to them, shows its first rows with the last out of
// view, and the wheel, End, Home and a click on an arrow bring the rest
// in. A menu that fits does not scroll. The taskbar's window list stopped
// at 12 rows because a menu could not do this.
#include <stdio.h>
#include <string.h>
#include "uitest.h"
#include "ui/uui_menubar.h"
#include "kapi.h"   // KEY_HOME, KEY_END

#define ROWS 60

static struct uui_menubar m;
static struct uui_menu_item items[ROWS];
static char labels[ROWS][16];

static int visible(int i) {
    int x, y, w, h;
    return uui_menubar_item_rect(&m, 0, i, &x, &y, &w, &h);
}

// The first row with a rect.
static int first_shown(void) {
    for (int i = 0; i < ROWS; i++) if (visible(i)) return i;
    return -1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uitest_begin("menuscroll", "a menu taller than its bounds scrolls");
    for (int i = 0; i < ROWS; i++) {
        snprintf(labels[i], sizeof labels[i], "Window %d", i + 1);
        items[i] = (struct uui_menu_item){ .label = labels[i], .code = i };
    }

    // --- one that fits: no scrolling, every row there ----------------------
    uui_menubar_init(&m, 0, 0);
    uui_menubar_set_bounds(&m, 0, 0, 800, 2000);
    uui_menubar_open_at(&m, items, 8, 10, 10);
    utest_check(!m.level[0].scroll && visible(0) && visible(7), "eight rows in room for them: no scrolling");
    uui_menubar_close(&m);

    // --- one that does not ------------------------------------------------
    int bh = 300;
    uui_menubar_init(&m, 0, 0);
    uui_menubar_set_bounds(&m, 0, 0, 800, bh);
    uui_menubar_open_at(&m, items, ROWS, 10, 10);
    int px, py, pw, ph;
    uui_menubar_popup_rect(&m, 0, &px, &py, &pw, &ph);
    utest_checkf(m.level[0].scroll && ph <= bh && py >= 0 && py + ph <= bh,
                 "60 rows in %d px: capped (%d px at y %d) and scrolling", bh, ph, py);
    utest_check(visible(0) && !visible(ROWS - 1), "the first row shows, the last does not");
    int shown = 0;
    for (int i = 0; i < ROWS; i++) shown += visible(i);
    utest_checkf(shown >= 4 && shown < ROWS, "%d rows in view", shown);

    // The wheel: down is a negative notch, three rows a notch, under the pointer.
    uui_menubar_motion(&m, px + pw / 2, py + ph / 2);
    utest_check(uui_menubar_wheel(&m, -1) && first_shown() == 3, "a notch down scrolls three rows");
    utest_check(uui_menubar_wheel(&m, +1) && first_shown() == 0, "a notch up scrolls back");
    utest_check(!uui_menubar_wheel(&m, +1), "...and no further than the top");

    // End and Home: the highlight goes there, and comes into view.
    int code;
    uui_menubar_key(&m, KEY_END, &code);
    utest_checkf(m.level[0].hot == ROWS - 1 && visible(ROWS - 1) && !visible(0),
                 "End highlights the last row and shows it (hot %d)", m.level[0].hot);
    utest_check(!uui_menubar_wheel(&m, -1), "...where the wheel cannot go further down");
    uui_menubar_key(&m, KEY_HOME, &code);
    utest_check(m.level[0].hot == 0 && visible(0), "Home brings the first back");
    // Down past the last row in view scrolls one.
    int last = -1;
    for (int i = 0; i < ROWS; i++) if (visible(i)) last = i;
    for (int k = 0; k < last + 1; k++) uui_menubar_key(&m, KEY_ARROW_DOWN, &code);
    utest_checkf(m.level[0].hot == last + 1 && visible(last + 1) && first_shown() == 1,
                 "Down past the last row in view scrolls one (first %d)", first_shown());

    // A click on the bottom arrow: one row; it commits nothing.
    uui_menubar_popup_rect(&m, 0, &px, &py, &pw, &ph);
    int before = first_shown();
    uui_menubar_press(&m, px + pw / 2, py + ph - 3);
    int rc = uui_menubar_release(&m, px + pw / 2, py + ph - 3);
    utest_checkf(first_shown() == before + 1 && rc < 0 && uui_menubar_is_open(&m),
                 "a click on the bottom arrow scrolls one row and commits nothing (%d -> %d)",
                 before, first_shown());

    // A row in view still commits its own code after scrolling.
    int x, y, w, h, target = first_shown() + 2;
    uui_menubar_item_rect(&m, 0, target, &x, &y, &w, &h);
    uui_menubar_press(&m, x + w / 2, y + h / 2);
    rc = uui_menubar_release(&m, x + w / 2, y + h / 2);
    utest_checkf(rc == target, "a click on a scrolled row commits that row (%d, wanted %d)", rc, target);
    return uitest_end();
}
