// uui_transfers: a row's status line in each state, and which button a
// click on each of a row's actions reports -- through the widget's own ops
// table, the way the router calls it. The plain row (no Pause) is the
// Browser's downloads row, which must keep its one Cancel.
//
// Prints one line per check and exits with the number of failures.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui_transfers.h"
#include "lib/utest.h"

static void oks(const char *name, const char *got, const char *want) {
    char d[300];
    snprintf(d, sizeof d, "got '%s', want '%s'", got, want);
    utest_check_detail(strcmp(got, want) == 0, name, d);
}

static struct uui_xfer rows[3];
static struct uui_transfers t;

// Press and release on the middle of action `k` from the right of `row`
// (0 = the rightmost), found by scanning the row for where a press arms.
static int click_action(int row, int from_right) {
    int rh = uui_transfers_row_h(), y = t.y + row * rh + rh / 2, seen = 0, last = -1;
    for (int x = t.x + t.w - 1; x > t.x; x--) {
        uui_transfers_ops.press(&t, x, y, 0);
        int armed = t.armed_action;
        uui_transfers_ops.release(&t, -1, -1);   // released away: nothing commits
        if (armed && armed != last) {
            if (seen++ == from_right) {
                uui_transfers_ops.press(&t, x - 2, y, 0);
                uui_transfers_ops.release(&t, x - 2, y);
                int r = -1, ev = uui_transfers_take(&t, &r);
                return r == row ? ev : -100 - r;
            }
        }
        last = armed;
    }
    return -1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ugfx_font_init();
    utest_begin("transfers_test", "uui_transfers: status lines and actions", UTEST_VERDICT_FILE);
    char s[200];

    struct uui_xfer x;
    memset(&x, 0, sizeof x);
    x.state = UUI_XFER_RUNNING;
    x.got = 8ull << 20;
    x.total = 16ull << 20;
    uui_xfer_status(&x, s, sizeof s);
    oks("a download row: got of total, nothing more", s, "8.0 MiB of 16.0 MiB");

    x.rate = 1ull << 20;
    snprintf(x.note, sizeof x.note, "a.bin");
    uui_xfer_status(&x, s, sizeof s);
    oks("with a rate: the rate, the time left and the file", s,
        "8.0 MiB of 16.0 MiB \xb7 1.0 MiB/s \xb7 about 8 s left \xb7 a.bin");
    x.got = 0;
    x.total = 300ull << 20;
    uui_xfer_status(&x, s, sizeof s);
    oks("five minutes left is said in minutes", s,
        "0 B of 300.0 MiB \xb7 1.0 MiB/s \xb7 about 5 min left \xb7 a.bin");

    x.state = UUI_XFER_PAUSED;
    x.got = 8ull << 20;
    x.total = 16ull << 20;
    uui_xfer_status(&x, s, sizeof s);
    oks("paused: no rate, no time left", s, "Paused \xb7 8.0 MiB of 16.0 MiB \xb7 a.bin");

    memset(&x, 0, sizeof x);
    x.state = UUI_XFER_RUNNING;
    x.items = 1;
    x.got = 3;
    x.total = 7;
    uui_xfer_status(&x, s, sizeof s);
    oks("counted in items", s, "3 of 7 items");
    x.state = UUI_XFER_QUEUED;
    uui_xfer_status(&x, s, sizeof s);
    oks("waiting", s, "Waiting \xb7 starts when the one before it is done");

    // --- actions, through the ops table ------------------------------------
    uui_transfers_init(&t);
    memset(rows, 0, sizeof rows);
    rows[0].state = UUI_XFER_RUNNING; rows[0].can_pause = 1; rows[0].total = 10;
    rows[1].state = UUI_XFER_QUEUED;
    rows[2].state = UUI_XFER_RUNNING; rows[2].total = 10;   // the Browser's row
    t.items = rows;
    t.count = 3;
    uui_transfers_ops.set_geometry(&t, 0, 0, 600, 3 * uui_transfers_row_h());

    int ev = click_action(0, 0);
    utest_checkf(ev == UUI_XFER_EV_CANCEL, "a pausable row's rightmost button is Cancel (got %d)", ev);
    ev = click_action(0, 1);
    utest_checkf(ev == UUI_XFER_EV_PAUSE, "...and the one beside it is Pause (got %d)", ev);
    rows[0].state = UUI_XFER_PAUSED;
    ev = click_action(0, 1);
    utest_checkf(ev == UUI_XFER_EV_RESUME, "paused, the same place is Resume (got %d)", ev);
    ev = click_action(1, 0);
    utest_checkf(ev == UUI_XFER_EV_CANCEL, "a waiting row's one button is Cancel (got %d)", ev);
    ev = click_action(1, 1);
    utest_checkf(ev == -1, "...and it has no second one (got %d)", ev);
    ev = click_action(2, 1);
    utest_checkf(ev == -1, "a row without Pause keeps one button, as the Browser's (got %d)", ev);
    ev = click_action(2, 0);
    utest_checkf(ev == UUI_XFER_EV_CANCEL, "...which is Cancel (got %d)", ev);
    return utest_end();
}
