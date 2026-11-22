// A window, then the same process's NEXT program's window, for
// exec_window_test: `execclient` opens "execclient before", quits the
// moment it is open, and execs `execclient after`, which opens
// "execclient after" and stays until 'q'.
//
// What it reproduces: ScummVM's front closing its card and execing
// ScummVM, whose window never came. The exec lets the old program's
// channel ring go and the new one opens a ring of the SAME NAME
// (toywm.<pid>) at once -- a small program does that before the
// compositor's next scan, so the compositor kept reading the dead ring
// (base/uchan.c's uchan_server_scan(), the ring's QUERY_SHM serial).
//
// Diagnostics go to stderr, the kernel log:
//   execclient: before open
//   execclient: after open
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ui/uapp.h"
#include "ui/ugfx.h"

static int g_after;

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    ugfx_fill(uapp_surface(d), g_after ? 0x2060C0 : 0xC06020);
}

static void on_open(struct uapp *a) {
    fprintf(stderr, "execclient: %s open\n", g_after ? "after" : "before");
    if (!g_after) uapp_quit(a, 0);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == 'q') uapp_quit(a, 0);
}

int main(int argc, char **argv) {
    g_after = argc > 1 && !strcmp(argv[1], "after");
    struct uapp_desc desc = {
        .title   = g_after ? "execclient after" : "execclient before",
        .w       = 320,
        .h       = 160,
        .on_open = on_open,
        .on_draw = on_draw,
        .on_key  = on_key,
    };
    int rc = uapp_run(&desc);
    if (g_after) return rc;
    char *next[] = { argv[0], "after", NULL };
    execv(argv[0], next);
    fprintf(stderr, "execclient: exec failed\n");
    return 1;
}
