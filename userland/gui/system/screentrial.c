// screentrial -- the answer to a screen size tried at this boot
// (lib/ubootmode.h): "Keep this resolution?" with a countdown that
// restarts on the old size when nobody answers -- which is what makes a
// size the monitor cannot show safe to try. Or, when the firmware did not
// offer the size, a notice that says so and remembers it.
//
//   screentrial --boot    exit at once unless this boot is a trial
//
// Started by init after toywm (data/etc/services.d/screentrial). Windows'
// "Keep these display settings?", across a restart because GRUB, not
// toy-os, sets the mode on this display.
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/uui_countdown.h"
#include "lib/ubootmode.h"
#include "lib/uinitctl.h"
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>

#define KEEP_SECONDS    15
#define RESTART_SECONDS 10

enum { A_KEEP = 1, A_BACK, A_RESTART, A_STAY };

static struct ubootmode g_m;
static char g_title[96], g_text[256], g_left[96], g_stay[48];
static int g_answer;

static void done(void *ctx, int code) {
    (void)ctx;
    g_answer = code;
}

static void restart(void) {
    sys_sync();
    if (uinitctl_shutdown(1) < 0) ulog("screentrial: could not restart\n");
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--boot")) {
        fprintf(stderr, "usage: screentrial --boot\n");
        return 2;
    }
    ubootmode_read(&g_m);
    if (!g_m.tried[0]) return 0;

    char now[UBOOTMODE_SIZE];
    snprintf(now, sizeof now, "%dx%d", g_m.now_w, g_m.now_h);
    const char *before = g_m.kept[0] ? g_m.kept : "the size it had before";
    struct uui_countdown_spec s = { .window_title = "Display" };

    if (!strcmp(now, g_m.tried)) {
        snprintf(g_title, sizeof g_title, "Keep this resolution?");
        snprintf(g_text, sizeof g_text,
                 "The screen is now %s. If you do not answer, the computer restarts with %s.",
                 now, before);
        s.title = g_title;
        s.text = g_text;
        s.remaining = "Restarting in %d seconds";
        s.seconds = KEEP_SECONDS;
        s.primary_label = "Keep";
        s.primary_code = A_KEEP;
        s.other_label = "Go back";
        s.other_code = A_BACK;
        s.timeout_code = A_BACK;
        s.close_code = A_BACK;
    } else {
        // GRUB fell back to its own choice: the firmware does not offer
        // the size. Remembered, so Settings offers it greyed from now on.
        ubootmode_refuse(g_m.tried);
        ubootmode_drop();
        ulogf("screentrial: %s not offered, came up at %s\n", g_m.tried, now);
        snprintf(g_title, sizeof g_title, "%s is not offered by this display", g_m.tried);
        snprintf(g_text, sizeof g_text,
                 "Its start-up firmware turned the size down, so the screen came up at %s. "
                 "%s is greyed in Settings from now on.", now, g_m.tried);
        s.title = g_title;
        s.text = g_text;
        // A restart helps only when the kept size is not what is on screen.
        if (g_m.kept[0] && strcmp(g_m.kept, now)) {
            snprintf(g_left, sizeof g_left, "Restarting with %s in %%d seconds", g_m.kept);
            snprintf(g_stay, sizeof g_stay, "Stay at %s", now);
            s.remaining = g_left;
            s.seconds = RESTART_SECONDS;
            s.primary_label = "Restart now";
            s.primary_code = A_RESTART;
            s.other_label = g_stay;
            s.other_code = A_STAY;
            s.timeout_code = A_RESTART;
            s.close_code = A_STAY;
        } else {
            s.primary_label = "OK";
            s.primary_code = A_STAY;
            s.close_code = A_STAY;
        }
    }

    struct uapp_desc d = { 0 };
    uui_countdown_app_desc(&d, &s, done, 0);
    d.app_id = "screentrial";
    int rc = uapp_run(&d);

    ulogf("screentrial: %s -> answer %d\n", g_m.tried, g_answer);
    switch (g_answer) {
    case A_KEEP:
        if (ubootmode_keep() < 0) ulog("screentrial: could not keep the size\n");
        break;
    case A_BACK:
        ubootmode_drop();
        restart();
        break;
    case A_RESTART:
        restart();
        break;
    default:
        break;
    }
    return rc;
}
