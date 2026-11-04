#ifndef WM_NOTIFY_H
#define WM_NOTIFY_H

// NOTIFICATIONS: the history behind the tray's bell, Do Not Disturb, each
// app's own setting, and the server half of WIN_REQ_NOTIFY. The TOAST is
// crash_notice.c's card -- every card it puts up, from a crash to a
// client's notification, comes through wm_notify_note() first, which
// records it and says whether it may show. KDE's notification applet and
// Windows' notification center are the two shapes; this is the first's
// bell with the second's per-app switches.
//
//   desktop.notifications.dnd        on | off
//   desktop.notifications.dnd_for    1h | 4h | off -- how long it lasts
//   /etc/notify.conf  app.<name>=on | quiet | off, one per app seen
//                     (quiet: the history only; the name with every
//                     character a key cannot hold as `_`), and
//                     dnd_until=<seconds since 1970> when Do Not Disturb
//                     lifts, 0: until turned off
//
// CRITICAL urgency shows under Do Not Disturb and with an app quieted --
// a remote desktop request, the windows a Close all could not close --
// as freedesktop's critical does. A TRANSIENT one (the desktop's Undo)
// is never kept: it means nothing once its card is gone.

#include <stdint.h>
#include "win_proto.h"

// What a history row does when clicked.
enum nf_target {
    NF_TARGET_NONE = 0,
    NF_TARGET_FILE,     // open `target` with its default app
    NF_TARGET_REPORT,   // Crash Reports on report `target`
    NF_TARGET_CLIENT,   // WIN_EV_NOTIFY (CLICKED) to `pid`, notification `cid`
};

struct nf_note {
    const char *app, *icon, *title, *body;
    int urgency;           // WIN_NOTIFY_URGENCY_*
    int transient;
    enum nf_target target_kind;
    const char *target;
    int pid;               // NF_TARGET_CLIENT's sender
    uint32_t cid;          // ...and the id it was answered
};

// A card is about to go up: record it (unless transient or the app is
// off) and say whether the card may SHOW -- not under Do Not Disturb or
// for a quieted app, unless critical. *hist gets the history row's id,
// 0 when none was kept.
int wm_notify_note(const struct nf_note *n, uint32_t *hist);

// A card was acted on or dismissed: its history row goes with it, as a
// toast answered in Windows leaves its notification center.
void wm_notify_forget(uint32_t hist);

// The server half (wm_client.c): one WIN_REQ_NOTIFY piece. Returns 1 and
// sets *reply when the message was END and must be answered.
int  wm_notify_piece(int pid, int a, int b, int c, const char *text, int *reply);
void wm_notify_close_request(int pid, uint32_t id);
// A client's notification `id` was answered at its card (crash_notice.c).
void wm_notify_answer(int pid, uint32_t id, int action);

int  wm_notify_dnd(void);              // in force now
void wm_notify_set_dnd(int on);        // Quick Settings' tile, as the bell's switch
void wm_notify_tray_init(void);
void wm_notify_poll(void);             // per frame: settings, DND's end, the badge

// The bell's flyout -- an overlay in wm_overlay.c's table.
extern int wm_notify_open;
void wm_notify_draw(int mx, int my);
int  wm_notify_click(int mx, int my);
int  wm_notify_hover_at(int mx, int my);
void wm_notify_damage(void);
int  wm_notify_rect(int *x, int *y, int *w, int *h);
int  wm_notify_key(int key, uint8_t mods);
void wm_notify_close(void);

// For `gui notify --json`: the history newest first, the badge, DND, and
// the flyout's rows, switch and buttons where they are drawn.
struct wm_notify_row_info {
    uint32_t id;
    const char *app, *title, *body;
    int x, y, w, h;        // its row in the flyout, 0 x 0 when not drawn
    int cx, cy, cs;        // its x (shown while the row is hovered)
};
int wm_notify_state(int *unread, int *dnd, int *rx, int *ry, int *rw, int *rh,
                    int sw[4], int clear[4], int bell[4],
                    struct wm_notify_row_info *out, int max);

#endif // WM_NOTIFY_H
