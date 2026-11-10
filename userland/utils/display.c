// display -- the screen off and back, and which it is (SYS_DISPLAY_POWER).
//
// TRANSIENT, as the desktop's own "Turn off the screen after" timer is:
// the panel's power (or the backlight) goes down and comes back, and the
// brightness setting is never touched. Any key or motion at the desktop
// also brings the screen back. KDE's `kscreen-doctor --dpms off` in shape.
#include "rt/sys.h"
#include "base/uargs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const struct uargs_opt OPTS[] = { { 0 } };

static const struct uargs_prog PROG = {
    .name = "display",
    .usage = "off | on | status",
    .summary = "Turn the screen off or back on for now, or say which it is.\n"
               "Off is not saved: any key or motion at the desktop turns it on.",
    .opts = OPTS,
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc != 1) return uargs_error(&PROG, "off, on or status?");
    const char *verb = a.argv[0];
    int op = !strcmp(verb, "off") ? 0 : !strcmp(verb, "on") ? 1 : !strcmp(verb, "status") ? 2 : -1;
    if (op < 0) return uargs_error(&PROG, "off, on or status?");
    int r = sys_display_power(op);
    if (r < 0) {
        printf("display: %s\n", errno == ENOTSUP ? "this screen cannot be turned off from here"
                                                 : strerror(errno));
        return 1;
    }
    printf("display: the screen is %s\n", r ? "on" : "off");
    return 0;
}
