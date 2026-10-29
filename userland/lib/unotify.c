// Notifications, the sender's half -- see unotify.h.
#include "lib/unotify.h"
#include "lib/uwmchan.h"
#include "lib/uutf8.h"
#include "lib/uchan.h"
#include "ui/uapp_internal.h"   // g_wmchan: a windowless sender reads its answer there
#include "rt/sys.h"
#include <string.h>

// A field in pieces of WIN_TITLE_LEN - 1 bytes, each cut BETWEEN
// CHARACTERS so the compositor's concatenation is the string sent.
static int send_field(int field, const char *s, int bound) {
    if (!s || !*s) return 1;
    int len = (int)strlen(s);
    if (len > bound - 1) len = uutf8_cut(s, len, bound - 1);
    char part[WIN_TITLE_LEN];
    for (int at = 0, piece = 0; at < len; piece++) {
        int n = uutf8_cut(s + at, len - at, WIN_TITLE_LEN - 1);
        if (n <= 0) break;
        memcpy(part, s + at, (size_t)n);
        part[n] = '\0';
        if (!wmchan_send(WIN_REQ_NOTIFY, 0, field | (piece << 8), 0, 0, part)) return 0;
        at += n;
    }
    return 1;
}

uint32_t unotify_send(const struct unotify *n) {
    if (!n || !n->title || !n->title[0]) return 0;
    if (!send_field(WIN_NOTIFY_FIELD_APP, n->app, WIN_NOTIFY_APP_MAX) ||
        !send_field(WIN_NOTIFY_FIELD_ICON, n->icon, WIN_NOTIFY_ICON_MAX) ||
        !send_field(WIN_NOTIFY_FIELD_TITLE, n->title, WIN_NOTIFY_TITLE_MAX) ||
        !send_field(WIN_NOTIFY_FIELD_BODY, n->body, WIN_NOTIFY_BODY_MAX))
        return 0;
    for (int k = 0; k < WIN_NOTIFY_ACTIONS && n->actions[k]; k++)
        if (!send_field(WIN_NOTIFY_FIELD_ACTION + k, n->actions[k], WIN_NOTIFY_ACTION_MAX)) return 0;
    int flags = (n->urgency & WIN_NOTIFY_URGENCY_MASK) | (n->transient ? WIN_NOTIFY_F_TRANSIENT : 0);
    // ANSWERED: a sender that exits next would otherwise take its ring,
    // and the half-delivered notification, with it.
    int id = wmchan_call(WIN_REQ_NOTIFY, 0, WIN_NOTIFY_FIELD_END, flags, (int)n->replaces, 0, 0);
    return id > 0 ? (uint32_t)id : 0;
}

int unotify_close(uint32_t id) {
    return id && wmchan_send(WIN_REQ_NOTIFY_CLOSE, 0, (int)id, 0, 0, 0);
}

int unotify_wait(uint32_t id, int timeout_ms, int *action) {
    if (!wmchan()) return 0;
    uint64_t end = timeout_ms < 0 ? 0 : sys_monotonic_ns() + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        struct win_event ev;
        while (uchan_client_recv(&g_wmchan, &ev, sizeof ev) == 1) {
            if (ev.type != WIN_EV_NOTIFY || (uint32_t)ev.a != id) continue;
            *action = ev.b;
            return 1;
        }
        if (!uchan_client_server_alive(&g_wmchan)) return 0;
        uint64_t now = sys_monotonic_ns();
        if (end && now >= end) return 0;
        int ms = end ? (int)((end - now) / 1000000ull) + 1 : 1000;
        uchan_client_wait(&g_wmchan, ms > 1000 ? 1000 : ms);
    }
}
