// See wm_lid.h. Closing the lid, acted on.
#include "wm_internal.h"
#include "wm_lid.h"
#include "wm_idle.h"
#include "wm_login.h"
#include "leave_page.h"
#include "base/usetting.h"
#include "base/ulog.h"
#include "kapi.h"
#include "rt/sys.h"

#define SET_LID "desktop.lid"

static int g_seen;            // QUERY_POWER read once
static uint64_t g_changes;    // its lid_changes last acted on
static int g_present, g_open = 1, g_acted;
static const char *g_last = "";

void wm_lid_state(int *present, int *open, int *acted, const char **last) {
    *present = g_present;
    *open = g_open;
    *acted = g_acted;
    *last = g_last;
}

static void act_closed(void) {
    char v[16] = "lock";
    usetting_get(SET_LID, v, sizeof v);
    // The login screen has no session to lock and no apps to close.
    if (wm_greeter && k_strcmp(v, "nothing")) k_strlcpy(v, "screen", sizeof v);
    if (!k_strcmp(v, "nothing")) {
        g_last = "nothing";
    } else if (!k_strcmp(v, "shutdown")) {
        g_last = "shutdown";
        leave_page_run(LEAVE_SHUTDOWN);   // runs it: nobody can click a chooser with the lid shut
    } else if (!k_strcmp(v, "screen")) {
        g_last = "screen";
        wm_idle_lid(1);
    } else {
        g_last = "lock";
        if (!wm_login_open) wm_login_show(WM_LOGIN_LOCK);
        wm_idle_lid(1);
    }
    g_acted++;
    ulogf("wm: the lid closed -- %s\n", g_last);
}

void wm_lid_poll(void) {
    struct query_power q;
    if (sys_query_record(QUERY_POWER, 0, &q, sizeof q) < (int)sizeof q) return;
    g_present = (q.flags & QUERY_POWER_LID_PRESENT) != 0;
    if (!g_present) return;
    int open = (q.flags & QUERY_POWER_LID_OPEN) != 0;
    // The state at startup is not a change: a desktop started with the lid
    // shut (an external screen) does nothing until it moves.
    if (!g_seen) {
        g_seen = 1;
        g_changes = q.lid_changes;
        g_open = open;
        return;
    }
    if (q.lid_changes == g_changes) return;
    g_changes = q.lid_changes;
    if (open == g_open) return;   // shut and opened again between two polls
    g_open = open;
    if (open) {
        ulogf("wm: the lid opened\n");
        wm_idle_lid(0);
        return;
    }
    act_closed();
}
