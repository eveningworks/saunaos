// uitest-app: userland/libs/uixx/examples/counter
//
// The C++ layer (docs/uixx-design.md) end to end on the host: Counter
// rewritten over uixx/app.hpp, its real main(), headless. Each check is a
// round trip on the window's pixels, so the C++ objects are proven to have
// reached the C toolkit and back: an id that never reached a button, a
// handler never called, a Tab ring never built, or a nested Row the
// Column dropped each changes what is drawn.
#include <stdlib.h>
#include <string.h>

#include "uitest_app.h"

static int lx, ly, lw, lh;   // the label
static int bx, by, bw, bh;   // the bar

// The label and the bar together: the count's text and its fill.
static uint32_t *snap(void) {
    size_t n = (size_t)lw * (size_t)lh + (size_t)bw * (size_t)bh;
    uint32_t *px = malloc(n * sizeof *px), *p = px;
    if (!px) return NULL;
    for (int y = 0; y < lh; y++)
        for (int x = 0; x < lw; x++) *p++ = uitest_app_pixel(lx + x, ly + y);
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) *p++ = uitest_app_pixel(bx + x, by + y);
    return px;
}

static int same(const uint32_t *a, const uint32_t *b) {
    size_t n = (size_t)lw * (size_t)lh + (size_t)bw * (size_t)bh;
    return a && b && !memcmp(a, b, n * sizeof *a);
}

int main(void) {
    uitest_begin("counterxx_app", "Counter in C++ over uixx, headless: buttons, keys, the Tab ring");
    char *argv[] = { "counter++", NULL };
    if (!uitest_app_start(1, argv)) return uitest_end();
    utest_check(!strcmp(uitest_app_title(), "Counter"), "the window is titled Counter");

    // --- the tree reached the C toolkit -------------------------------------
    int ax, ay, aw, ah, rx, ry, rw, rh;
    int have = uitest_app_widget("label", &lx, &ly, &lw, &lh) && uitest_app_widget("bar", &bx, &by, &bw, &bh) &&
               uitest_app_widget("add", &ax, &ay, &aw, &ah) && uitest_app_widget("reset", &rx, &ry, &rw, &rh);
    utest_check(have, "the label, the bar and both buttons of the nested Row are laid out by name");
    if (!have) return uitest_end();
    utest_check(ay == ry && rx >= ax + aw, "the two buttons share a row, +1 first");
    utest_check(ay >= by + bh && by >= ly + lh, "the column stacks label, bar, then the row");
    utest_check(bw > aw + rw, "the bar fills the column's width (fill())");

    // A Widget subclass's draw() ran: the bar's track is outlined, so its
    // corner differs from its inside.
    utest_check(uitest_app_pixel(bx, by) != uitest_app_pixel(bx + 3, by + bh / 2),
                "the C++ bar widget draws its outlined track");
    uint32_t *zero = snap();

    // --- a click reaches the button's on_click lambda ------------------------
    uitest_app_click("add");
    uint32_t *one = snap();
    utest_check(!same(one, zero), "a click on +1 repaints the label and the bar");
    uitest_app_click("add");
    uint32_t *two = snap();
    utest_check(!same(two, one) && !same(two, zero), "a second click draws a count unlike either");
    uitest_app_click("reset");
    uint32_t *back = snap();
    utest_check(same(back, zero), "Reset restores the label and the bar to exactly their first pixels");

    // --- the window's on_key override -----------------------------------------
    uitest_app_key('+', 0);
    uint32_t *kb = snap();
    utest_check(same(kb, one), "the + key draws exactly what one click drew");
    uitest_app_key('r', 0);
    uint32_t *kr = snap();
    utest_check(same(kr, zero), "the r key resets");

    // --- the Tab ring set_root() built ----------------------------------------
    // Two clicks leave +1 focused; Tab moves to Reset, whose Space resets.
    // With no ring the Space reaches on_key instead, which counts it: 3.
    uitest_app_click("add");
    uitest_app_click("add");
    uitest_app_key('\t', 0);
    uitest_app_key(' ', 0);
    uint32_t *tab = snap();
    utest_check(same(tab, zero), "Tab then Space on Reset resets (the ring is built, the key spent)");

    // --- a press dragged OFF a button commits nothing ------------------------------
    uitest_app_press_at(ax + aw / 2, ay + ah / 2);
    uitest_app_move_to(uitest_app_w() + 40, uitest_app_h() + 40, 1);
    uitest_app_release_at(uitest_app_w() + 40, uitest_app_h() + 40);
    uint32_t *dragged = snap();
    utest_check(same(dragged, zero), "a press on +1 dragged off and released leaves the count at 0");

    uitest_app_keep("final");
    int status = uitest_app_close();
    uitest_check_int("closing the window ends the app cleanly", status, 0);
    free(zero); free(one); free(two); free(back); free(kb); free(kr); free(tab); free(dragged);
    return uitest_end();
}
