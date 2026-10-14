// Per-pane navigation history -- back and forward.
//
// **PER PANE, NOT PER APP.** Each pane is its own place, the way each
// tab is in Explorer and Dolphin; a shared stack would send the left
// pane back to somewhere the RIGHT pane had been, which reads as the
// window losing its mind rather than as a feature.
//
// The list itself is lib/unavhist.h's, shared with the file chooser:
// the browser's shape, a bounded ring.
#include "fm_internal.h"
#include "ui/uui_fileview.h"
#include "lib/unavhist.h"
#include "kpath.h"
#include <string.h>
#include <stdio.h>

static struct unavhist g_hist[2];

// Set while history itself is driving the pane, so the set_dir() that
// results does not record a new entry on top of the one it is
// replaying -- which would make Back push Back onto the stack and the
// button would never reach anything older.
static int g_replaying;

// A TAB OWNS ITS PANES' HISTORIES (fm_tabs.c): saved when it is left,
// put back when it is chosen -- Back in a tab never goes to another
// tab's folders, as in Explorer and every browser.
int fm_history_size(void) { return (int)sizeof g_hist[0]; }
void fm_history_save(int pane, void *out) { memcpy(out, &g_hist[pane], sizeof g_hist[pane]); }
void fm_history_load(int pane, const void *in) {
    if (in) memcpy(&g_hist[pane], in, sizeof g_hist[pane]);
    else unavhist_init(&g_hist[pane]);
}
// Put a pane where a restored history says it is, without recording it.
void fm_history_replay(int pane, const char *dir) {
    g_replaying = 1;
    uui_fileview_set_dir(&g_pane[pane], dir);
    g_replaying = 0;
}

void fm_history_record(int pane, const char *dir) {
    if (pane < 0 || pane > 1 || g_replaying) return;
    unavhist_record(&g_hist[pane], dir);
}

int fm_history_can_back(int pane)    { return pane >= 0 && pane <= 1 && unavhist_can(&g_hist[pane], -1); }
int fm_history_can_forward(int pane) { return pane >= 0 && pane <= 1 && unavhist_can(&g_hist[pane], 1); }

// The bin and Recent are always there; a folder must still exist.
static int still_there(void *ctx, const char *dir) {
    (void)ctx;
    struct sys_stat st;
    if (!strcmp(dir, FM_BIN) || !strcmp(dir, FM_RECENT)) return 1;
    return sys_stat(dir, &st) == 0 && st.is_dir;
}

// Move by one and put the pane there. Returns 1 on arriving, 0 when
// there is nowhere to go, -1 when every entry that way has gone away.
//
// A DIRECTORY THAT HAS GONE AWAY since it was visited is STEPPED OVER,
// checked before the pane moves: set_dir() on it would leave the pane
// showing a dead path as an empty folder (ui/uui_fileview.h). The entry
// stays, because it may exist again by the time Back comes past.
static int go(int pane, int delta) {
    if (pane < 0 || pane > 1) return 0;
    const char *dead;
    const char *to = unavhist_step(&g_hist[pane], delta, still_there, 0, &dead);
    if (to) {
        g_replaying = 1;
        uui_fileview_set_dir(&g_pane[pane], to);
        g_replaying = 0;
        if (dead)
            snprintf(g_stat_note, sizeof g_stat_note, "skipped %s -- it no longer exists",
                     k_path_basename(dead));
        return 1;
    }
    if (!dead) return 0;
    snprintf(g_stat_note, sizeof g_stat_note, "no such folder: %s", k_path_basename(dead));
    return -1;
}

int fm_history_back(int pane)    { return go(pane, -1); }
int fm_history_forward(int pane) { return go(pane, +1); }

// **AND THIS IS THE BACKSTOP, BECAUSE A PANE IS A WIDGET AND A WIDGET
// NAVIGATES ON ITS OWN.** `uui_fileview_release()` activates a row on a
// double click, from inside the widget, so a MOUSE descent passes
// through none of the three functions below -- exactly as Enter and
// Backspace did from inside the key handler. That bypass was fixed for
// the keyboard by comparing the directory across the call and missed
// for the mouse, which left Back dead for anyone navigating the way
// people actually navigate a file manager.
//
// So this records by RESULT rather than by route: whatever moved a
// pane, and whatever moves one in future, is caught. Safe to call as
// often as you like -- record() already ignores a repeat of the same
// place and anything during a replay -- which is what lets the caller
// be a per-frame tick instead of a list of call sites somebody has to
// keep complete.
void fm_history_sync(void) {
    for (int p = 0; p < 2; p++)
        fm_history_record(p, uui_fileview_dir(&g_pane[p]));
}

// THE ONE PLACE A PANE'S DIRECTORY CHANGES. It used to change at four
// call sites with nothing in common, which is exactly how a history
// ends up describing somewhere the user has never been -- so the
// recording lives here rather than at each of them, and a fifth call
// site cannot forget.
int fm_goto(int pane, const char *dir) {
    if (pane < 0 || pane > 1) return 0;
    // A network share that needs a password asks for it instead of
    // listing empty, and the Add row opens a dialog (fm_network.c).
    if (net_intercept(pane, dir)) return 1;
    char want[PATH_MAX_LEN];
    strlcpy(want, dir, sizeof want);       // `dir` may be a buffer the listing rewrites
    if (!uui_fileview_set_dir(&g_pane[pane], want)) return 0;
    fm_history_record(pane, uui_fileview_dir(&g_pane[pane]));
    net_after_goto(pane, want);
    return 1;
}

// The same, for the two moves the fileview makes on its own -- it
// resolves the destination itself, so the record is taken AFTER from
// wherever it landed rather than from a path computed here.
int fm_goto_up(int pane) {
    if (pane < 0 || pane > 1) return 0;
    uui_fileview_up(&g_pane[pane]);
    fm_history_record(pane, uui_fileview_dir(&g_pane[pane]));
    return 1;
}

int fm_goto_activate(int pane) {
    if (pane < 0 || pane > 1) return 0;
    if (!uui_fileview_activate(&g_pane[pane])) return 0;
    fm_history_record(pane, uui_fileview_dir(&g_pane[pane]));
    return 1;
}
