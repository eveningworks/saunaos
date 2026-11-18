// uitest-app: userland/libs/uixx/examples/widgets
//
// The C++ layer's value widgets (uixx/values.hpp) end to end on the host,
// through the example form's real main(). Each widget's on_change logs the
// value it was handed, so a check reads what the C++ slot received, after
// input that went through the real router. ON_CHANGE IS USER INPUT ONLY:
// the form's Reset sets every value back with set_*(), and that must log
// nothing but its own line.
#include <stdlib.h>
#include <string.h>

#include "keyboard.h"
#include "uitest_app.h"

// The last `widgets: <what> ...` line since `mark`, or "" -- the value
// part only.
static const char *said(size_t mark, const char *what) {
    static char line[160];
    char needle[48];
    snprintf(needle, sizeof needle, "widgets: %s ", what);
    if (!uitest_log_find(mark, needle, line, sizeof line)) return "";
    char *p = strstr(line, needle);
    p += strlen(needle);
    p[strcspn(p, "\n")] = '\0';
    return p;
}

static int said_any(size_t mark) {
    static const char *const names[] = { "name", "hidden", "dark", "size", "quality", "recent" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (*said(mark, names[i])) return 1;
    return 0;
}

int main(void) {
    uitest_begin("widgetsxx_app", "the C++ value widgets: on_change on input, never on set");
    size_t start = uitest_log_mark();
    char *argv[] = { "widgets++", NULL };
    if (!uitest_app_start(1, argv)) return uitest_end();
    utest_check(!said_any(start), "the constructor's set_*() calls report no change");

    int x, y, w, h;
    static const char *const all[] = { "name", "hidden", "dark", "size", "quality", "recent", "progress", "reset" };
    int laid = 1;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) laid &= uitest_app_widget(all[i], &x, &y, &w, &h);
    utest_check(laid, "every value widget is laid out by name");
    if (!laid) return uitest_end();

    // --- Textbox: every keystroke, with the whole text ---------------------------
    size_t m = uitest_log_mark();
    uitest_app_click("name");
    uitest_app_type("Ada");
    utest_check(!strcmp(said(m, "name"), "Ada"), "typing Ada reports the text \"Ada\"");
    // A click in the field moves the caret and reaches the app as input,
    // but the text is the same: no change, so no report.
    m = uitest_log_mark();
    uitest_app_click("name");
    utest_check(!*said(m, "name"), "a click in the field that changes no text reports nothing");

    // --- Checkbox and Switch: a click flips, and says which way ------------------
    m = uitest_log_mark();
    uitest_app_click("hidden");
    utest_check(!strcmp(said(m, "hidden"), "1"), "a click on the checkbox reports true");
    m = uitest_log_mark();
    uitest_app_click("hidden");
    utest_check(!strcmp(said(m, "hidden"), "0"), "a second click reports false");
    m = uitest_log_mark();
    uitest_app_click("dark");
    utest_check(!strcmp(said(m, "dark"), "1"), "a click on the switch reports true");

    // --- Slider: a click at the far end chooses the last stop, and the --------------
    // handler's set_value moves the progress bar.
    uitest_app_widget("progress", &x, &y, &w, &h);
    uint32_t before = uitest_app_pixel(x + w - 3, y + h / 2);
    uitest_app_widget("quality", &x, &y, &w, &h);
    m = uitest_log_mark();
    uitest_app_click_at(x + w - 2, y + h / 2);
    utest_check(!strcmp(said(m, "quality"), "4"), "a click at the slider's right end reports stop 4");
    uitest_app_widget("progress", &x, &y, &w, &h);
    utest_check(uitest_app_pixel(x + w - 3, y + h / 2) != before,
                "...and its handler filled the progress bar to the end");

    // --- Dropdown: the keyboard, as the router hands it a focused widget -------------
    m = uitest_log_mark();
    uitest_app_click("size");             // opens it, and focuses it
    uitest_app_key(KEY_ARROW_DOWN, 0);
    uitest_app_key('\n', 0);
    utest_check(!strcmp(said(m, "size"), "2"), "Down then Enter in the open dropdown reports index 2");

    // --- Listbox: a click on a row -------------------------------------------------
    uitest_app_widget("recent", &x, &y, &w, &h);
    m = uitest_log_mark();
    uitest_app_click_at(x + w / 3, y + 4);
    utest_check(!strcmp(said(m, "recent"), "0"), "a click on the first row reports row 0");
    m = uitest_log_mark();
    uitest_app_click_at(x + w / 3, y + 4);
    utest_check(!*said(m, "recent"), "a second click on the same row changes nothing, and reports nothing");

    // --- Reset: set_*() on every widget, and not one on_change ----------------------
    m = uitest_log_mark();
    uitest_app_click("reset");
    char line[64];
    utest_check(uitest_log_find(m, "widgets: reset", line, sizeof line), "Reset ran");
    utest_check(!said_any(m), "Reset's set_*() calls report no change");

    // ...and the next input is compared with what Reset SET, not with what
    // the person left: the switch was on, Reset turned it off, a click
    // turns it on -- a change only against Reset's value.
    m = uitest_log_mark();
    uitest_app_click("dark");
    utest_check(!strcmp(said(m, "dark"), "1"), "after Reset, a click on the switch reports true again");

    uitest_app_keep("final");
    uitest_check_int("closing the window ends the app cleanly", uitest_app_close(), 0);
    return uitest_end();
}
