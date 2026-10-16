#ifndef BROWSER_NS_SESSION_H
#define BROWSER_NS_SESSION_H

// The open tabs and each one's back/forward list, kept across a restart
// -- Firefox's sessionstore, Chrome's "Continue where you left off".
// /var/lib/browser/session.tsv, rewritten whole (write, then rename) a
// moment after anything in it changes, so a browser that is killed loses
// at most that moment.
#include "ui/uui_webview.h"

// One tab as read back: its list (malloc'd strings) and where it was.
struct ns_session_tab {
    struct uui_wv_entry e[UUI_WV_HISTORY_MAX];
    int n, at;
};

// Reads the file into `tabs` (at most `cap`); `*active` is the tab that
// was in front. Returns the count, 0 when there is no session.
int  ns_session_load(const char *dir, struct ns_session_tab *tabs, int cap, int *active);
void ns_session_free(struct ns_session_tab *tabs, int n);
// Writes the tabs' lists. Each view's scroll is noted first.
int  ns_session_save(const char *dir, struct uui_webview *const *tabs, int n, int active);

#endif
