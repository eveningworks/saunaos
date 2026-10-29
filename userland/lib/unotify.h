#ifndef ULIB_UNOTIFY_H
#define ULIB_UNOTIFY_H

// Notifications: one call for any program, a window or none -- the
// compositor draws the toast, keeps the history behind the tray's bell,
// and applies Do Not Disturb and each app's own setting (WIN_REQ_NOTIFY,
// abi/win_proto.h). freedesktop's org.freedesktop.Notifications in shape:
// an app name, an icon, a title and body, up to three actions, an
// urgency, and an id the answer comes back with.

#include <stdint.h>
#include "win_proto.h"

struct unotify {
    const char *app;      // the sender's name as people know it; NULL: "Notification"
    const char *icon;     // an icon_cache name; NULL: the app's own, or a bell
    const char *title;    // required
    const char *body;     // may be NULL
    const char *actions[WIN_NOTIFY_ACTIONS];   // labels, NULL-terminated early
    int urgency;          // WIN_NOTIFY_URGENCY_*; 0 is LOW, so set it
    int transient;        // never kept in the history
    uint32_t replaces;    // an earlier id to update in place, or 0
};

// Sends it and waits for its id: nonzero, or 0 when no desktop is running
// or it was refused. Each field past its bound is cut between characters.
uint32_t unotify_send(const struct unotify *n);

// Takes notification `id` down -- card and history row.
int unotify_close(uint32_t id);

// Waits up to `timeout_ms` (<0: no limit) for the answer to `id`:
// WIN_NOTIFY_* or an action index. 1 with *action set, 0 on timeout or
// when the desktop went away. For a program with no event loop of its
// own -- a GUI app hears WIN_EV_NOTIFY through uapp_desc.on_notify.
int unotify_wait(uint32_t id, int timeout_ms, int *action);

#endif
