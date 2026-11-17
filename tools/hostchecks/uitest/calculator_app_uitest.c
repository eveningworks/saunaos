// uitest-app: userland/apps/apps/calculator
//
// Calculator's REAL main(), its .ui file, its engine and its handlers,
// headless on the host (tools/lib/uitest/uitest_app.h, stage 2), driven
// by its widget map's names. Moved down from tools/tests/gui/
// calculator_client_test.py, whose checks these are; that tool keeps the
// ring-3 process on the real screen.
//
// NO DIGIT IS READ. Every check is a round trip on the display's pixels:
// a key changes them, and returning to the same state restores them
// EXACTLY -- arithmetic and rendering in one assertion, and stricter than
// reading a number, since it also catches the right number drawn in the
// wrong place. The check that matters most is the last: a press dragged
// OFF a button and released must not commit (docs/gui-guidelines.md).
#include <stdlib.h>
#include <string.h>

#include "uitest_app.h"

// The display's numeric strip: the lower two thirds of the display item,
// as calculator_client_test.py takes it.
static int dx, dy, dw, dh;

static uint32_t *strip(void) {
    int sh = dh * 2 / 3, sy = dy + dh - sh;
    uint32_t *px = malloc((size_t)dw * (size_t)sh * sizeof *px);
    if (!px) return NULL;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < dw; x++) px[y * dw + x] = uitest_app_pixel(dx + x, sy + y);
    return px;
}

static int same(const uint32_t *a, const uint32_t *b) {
    return a && b && !memcmp(a, b, (size_t)dw * (size_t)(dh * 2 / 3) * sizeof *a);
}

int main(void) {
    uitest_begin("calculator_app", "Calculator's own main, headless: buttons, keys, the display");
    char *argv[] = { "calculator", NULL };
    if (!uitest_app_start(1, argv)) return uitest_end();
    utest_check(!strcmp(uitest_app_title(), "Calculator"), "the window is titled Calculator");
    if (!uitest_app_widget("display", &dx, &dy, &dw, &dh)) return uitest_end();
    uint32_t *zero = strip();

    // --- the buttons are DRAWN, not just clickable -------------------------
    // A Calculator once shipped with no visible buttons: every click still
    // changed the display. A face pixel away from the label must differ from
    // the panel between the display and the grid.
    int x, y, w, h;
    uitest_app_widget("seven", &x, &y, &w, &h);
    uint32_t digit = uitest_app_pixel(x + 3, y + 3);
    uitest_app_widget("divide", &x, &y, &w, &h);
    uint32_t op = uitest_app_pixel(x + 3, y + 3);
    uint32_t gap = uitest_app_pixel(dx + 2, dy + dh + 1);
    utest_check(digit != gap && op != gap, "the buttons are drawn, not just clickable");
    utest_check(op != digit, "operator buttons are tinted differently from digits");

    // --- mouse arithmetic ---------------------------------------------------------
    uitest_app_click("seven");
    uint32_t *seven = strip();
    utest_check(!same(seven, zero), "a click on a button repaints the display");
    uitest_app_click("plus");
    uitest_app_click("three");
    uitest_app_click("equals");
    uint32_t *ten = strip();
    utest_check(!same(ten, seven) && !same(ten, zero), "7 + 3 = renders a result unlike either input");
    uitest_app_click("clear");
    uint32_t *cleared = strip();
    utest_check(same(cleared, zero), "C restores the display to exactly its first pixels");

    // --- the keyboard agrees with the mouse ------------------------------------------
    uitest_app_type("7+3");
    uitest_app_key('\n', 0);
    uint32_t *kb = strip();
    utest_check(same(kb, ten), "the keyboard's 7 + 3 Enter draws exactly the mouse's result");
    uitest_app_click("clear");

    // --- a press dragged OFF a button and released commits nothing ---------------------
    uitest_app_widget("nine", &x, &y, &w, &h);
    uitest_app_press_at(x + w / 2, y + h / 2);
    uitest_app_move_to(uitest_app_w() + 40, uitest_app_h() + 40, 1);
    uitest_app_release_at(uitest_app_w() + 40, uitest_app_h() + 40);
    uint32_t *dragged = strip();
    utest_check(same(dragged, zero), "a press on 9 dragged off and released leaves the display unchanged");
    // ...and the same button still works after the cancelled press.
    uitest_app_click("nine");
    uint32_t *nine = strip();
    utest_check(!same(nine, zero), "...and 9 still works after the cancelled press");

    uitest_app_keep("final");
    int status = uitest_app_close();
    uitest_check_int("closing the window ends the app cleanly", status, 0);
    free(zero); free(seven); free(ten); free(cleared); free(kb); free(dragged); free(nine);
    return uitest_end();
}
