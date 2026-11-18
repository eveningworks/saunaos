// uitest-app: userland/libs/uixx/examples/notes
//
// The C++ layer's stage 3 (docs/uixx-design.md) through the Notes
// example's real main(): menu shortcuts bound from the text the menu
// shows, a greyed item whose shortcut does nothing, a menu opened and
// committed from the keyboard, a timer on the toolkit's clock, a busy
// bar that moves while ticked, and a notebook's pages. The About box is
// a separate window, which this harness refuses -- the guest test opens it.
#include <stdlib.h>
#include <string.h>

#include "keyboard.h"
#include "uitest_app.h"

static int logged(size_t mark, const char *needle) {
    char line[160];
    return uitest_log_find(mark, needle, line, sizeof line);
}

// Ctrl+letter arrives as its control code (api/keyboard.h).
#define CTRL(c) ((c) & 0x1f)

int main(void) {
    uitest_begin("notesxx_app", "uixx menus, shortcuts, timers, a busy bar and tabs");
    char *argv[] = { "notes++", NULL };
    if (!uitest_app_start(1, argv)) return uitest_end();

    int mx, my, mw, mh, tx, ty, tw, th, x, y, w, h;
    int have = uitest_app_widget("menu", &mx, &my, &mw, &mh) && uitest_app_widget("tabs", &tx, &ty, &tw, &th) &&
               uitest_app_widget("text", &x, &y, &w, &h);
    utest_check(have, "the menu bar, the tab strip and the Note page's field are laid out by name");
    if (!have) return uitest_end();
    utest_check(!uitest_app_has_widget("clock"), "the Clock page is hidden while Note is shown");

    // --- a click on a menu title opens its menu INSIDE the window, below the bar ------------
    // FIRST, before any key: a key also fits the menu's bounds to the window,
    // so only a click that comes before one tests that the press does.
    // Clamped to the bar's own rect instead of the window's, the menu would
    // be pushed UP over the bar: the View title beside File would vanish.
    uint32_t below = uitest_app_pixel(mx + 12, my + mh + 6);
    uint32_t *bar = malloc((size_t)mw * sizeof *bar);
    for (int i = 0; i < mw; i++) bar[i] = uitest_app_pixel(mx + i, my + mh / 2);
    uitest_app_click_at(mx + 12, my + mh / 2);
    utest_check(uitest_app_pixel(mx + 12, my + mh + 6) != below, "a click on File opens its menu under the bar");
    int kept = 0;
    for (int i = mw / 3; i < mw; i++) kept += bar[i] == uitest_app_pixel(mx + i, my + mh / 2);
    utest_check(kept == mw - mw / 3, "...below it: the bar right of File, View's title included, is untouched");
    free(bar);
    uitest_app_key(0x1b, 0);


    // --- a greyed item's shortcut does nothing --------------------------------------
    size_t m = uitest_log_mark();
    uitest_app_key(CTRL('l'), KEY_MOD_CTRL);
    utest_check(!logged(m, "notes: cleared"), "Ctrl+L does nothing while Clear is greyed (the field is empty)");
    uitest_app_click("text");
    uitest_app_type("hi");
    m = uitest_log_mark();
    uitest_app_key(CTRL('l'), KEY_MOD_CTRL);
    utest_check(logged(m, "notes: cleared"), "...and runs Clear once the field has text");

    // --- the menu from the keyboard: F10 opens File on its first row, Right
    // moves to View's (Note), Down to Clock, Enter commits.
    m = uitest_log_mark();
    uitest_app_key(KEY_F10, 0);
    uitest_app_key(KEY_ARROW_RIGHT, 0);      // View, on Note
    uitest_app_key(KEY_ARROW_DOWN, 0);       // Clock
    uitest_app_key('\n', 0);
    utest_check(logged(m, "notes: tab") == 0, "View > Clock from the keyboard reports no tab change (it is set, not chosen)");
    utest_check(uitest_app_widget("clock", &x, &y, &w, &h), "...and shows the Clock page");

    // --- a hidden page's controls take no keys: the field still has the focus
    // from its click, but its page is not shown. Typing must not reach it, so
    // Clear stays greyed once Note is back.
    uitest_app_type("x");
    uitest_app_key('1', KEY_MOD_CTRL);       // a digit keeps its code; only letters become control codes
    m = uitest_log_mark();
    uitest_app_key(CTRL('l'), KEY_MOD_CTRL);
    utest_check(!logged(m, "notes: cleared"), "keys typed while the field's page was hidden did not reach it");
    uitest_app_key('2', KEY_MOD_CTRL);

    // --- a timer on the toolkit's clock -------------------------------------------------
    m = uitest_log_mark();
    uitest_app_key(CTRL('k'), KEY_MOD_CTRL);
    utest_check(logged(m, "notes: clock 1"), "Ctrl+K ticks Run clock on");
    uitest_advance_ms(400);
    uitest_app_tick(1);
    utest_check(!logged(m, "notes: tick"), "400 ms in, the 1-second timer has not fired");
    uitest_advance_ms(700);
    uitest_app_tick(1);
    utest_check(logged(m, "notes: tick 1"), "1.1 s in, it fired once");
    uitest_advance_ms(1000);
    uitest_app_tick(1);
    utest_check(logged(m, "notes: tick 2"), "...and again a second later");
    m = uitest_log_mark();
    uitest_app_key(CTRL('k'), KEY_MOD_CTRL);
    uitest_advance_ms(3000);
    uitest_app_tick(1);
    utest_check(logged(m, "notes: clock 0") && !logged(m, "notes: tick"), "Ctrl+K again stops it: no tick after");

    // --- a busy bar moves while ticked -------------------------------------------------------
    uitest_app_widget("progress", &x, &y, &w, &h);
    uitest_app_key(CTRL('b'), KEY_MOD_CTRL);
    uitest_advance_ms(50);
    uitest_app_tick(1);
    uint32_t *a = malloc((size_t)w * sizeof *a);
    for (int i = 0; i < w; i++) a[i] = uitest_app_pixel(x + i, y + h / 2);
    uitest_advance_ms(50);
    uitest_app_tick(1);
    int moved = 0;
    for (int i = 0; i < w; i++) moved |= a[i] != uitest_app_pixel(x + i, y + h / 2);
    utest_check(moved, "Ctrl+B makes the bar busy, and its block moves from one tick to the next");
    free(a);
    uitest_app_key(CTRL('b'), KEY_MOD_CTRL);

    // --- a click on the strip's first tab is a CHOICE: on_change hears it --------------------------
    // The strip is the notebook's top row.
    m = uitest_log_mark();
    uitest_app_click_at(tx + 8, ty + 4);
    utest_check(logged(m, "notes: tab 0"), "a click on the Note tab reports tab 0");
    utest_check(uitest_app_widget("text", &x, &y, &w, &h), "...and shows the Note page again");

    uitest_app_keep("final");
    uitest_check_int("closing the window ends the app cleanly", uitest_app_close(), 0);
    return uitest_end();
}
