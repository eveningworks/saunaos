// A process with more windows than uapp's first eight slots
// (UAPP_SLOTS_FIXED, ui/uapp_internal.h): it opens twelve dialog
// windows over its own and says how many it got and which handles held.
// The fixture for tools/tests/gui/uapp_test.py's check_many_windows; the
// old fixed table of eight refused the eighth window silently.
#include <stdio.h>
#include "rt/sys.h"
#include "ui/uapp.h"
#include "base/ulog.h"

#define MANY 12

static struct uapp_window *g_win[MANY];
static char g_title[MANY][24];
static int g_opened;

static int on_tick(struct uapp *a) {
    if (g_opened) return 0;
    g_opened = 1;
    int got = 0;
    for (int i = 0; i < MANY; i++) {
        snprintf(g_title[i], sizeof g_title[i], "Many %d", i + 1);
        struct uapp_window_desc d = { .title = g_title[i], .w = 160, .h = 60 };
        g_win[i] = uapp_window_open(a, &d);
        got += g_win[i] != 0;
    }
    // A handle must still be the window it named after the table grew.
    int same = 1;
    for (int i = 0; i < MANY; i++)
        if (g_win[i] && uapp_window_width(g_win[i]) != 160) same = 0;
    ulogf("manywin: opened %d of %d, handles %s\n", got, MANY, same ? "kept" : "MOVED");
    return 0;
}

int main(void) {
    struct uapp_desc desc = {
        .title   = "Many Windows",
        .w       = 200,
        .h       = 80,
        .on_tick = on_tick,
        .tick_ms = 100,
    };
    return uapp_run(&desc);
}
