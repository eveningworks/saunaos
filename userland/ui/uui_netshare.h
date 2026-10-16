#ifndef UUI_NETSHARE_H
#define UUI_NETSHARE_H

// THE TWO NETWORK-SHARE DIALOGS, for any app with a uui_dialog: "Add
// network share" (an address, a name, open it now) and "Sign in to
// <server>" (a user, a password, Remember). The File Manager's Network
// rows use both; System Settings or the desktop can open the same ones.
//
// What they DO is not the app's business either: Add writes
// /etc/mounts.d/<name> (lib/unetmount.h) and waits for smbfsd to mount
// it; Sign in hands the password to the share's smbfsd worker through
// the kernel (USERFS_CTL_LOGIN) on a THREAD -- a server that is slow to
// answer must not freeze the window -- and the dialog says "Signing
// in…" until it does. The password goes from the field to the worker
// and is wiped here; only its hash is ever saved, and only by the
// worker, and only when Remember is ticked. The Sign in form itself is
// the toolkit's (ui/uui_signin.h); what is done with it is this file's.
//
//     uui_netshare_add(&ns, &dlg);             // or _signin(&ns, &dlg, "/net/media", ...)
//     ... the dialog's body item is uui_netshare_item(&ns) ...
//     on a commit:  uui_netshare_answer(&ns, code);
//     every tick:   switch (uui_netshare_poll(&ns, point, sizeof point)) { ... }
#include <stddef.h>
#include "ui/uui_dialog.h"
#include "ui/uui_form.h"
#include "ui/uui_signin.h"
#include "ui/uui_widget.h"

enum {
    UUI_NETSHARE_IDLE = 0,   // nothing happening (closed, or waiting for the user)
    UUI_NETSHARE_BUSY,       // a sign-in or a mount is under way: keep polling
    UUI_NETSHARE_OPEN,       // done: open the folder poll() wrote out
    UUI_NETSHARE_CLOSED,     // done: cancelled, or added without opening
};

struct uui_netshare {
    struct uui_dialog *dlg;
    struct uui_form form;
    struct uui_item item;
    int kind;                       // 1 add, 2 sign in
    int busy;
    int state;                      // what poll() reports next
    char title[96];
    char row[160];
    const char *rows[1];
    char point[128];                // the share's folder (/net/<name>)
    char name[32];                  // add: the file written
    char server[96];
    char error[128];
    char suggested[32];             // add: the name last suggested from the address
    int f_addr, f_name, f_open;     // add's rows
    struct uui_signin si;           // sign-in's dialog
    unsigned long long since;       // add: when the file was written (ms)
    volatile int done;              // the sign-in thread's
    int result;                     // and its answer (0 or -errno)
    void *ctl;                      // the message the thread sends
};

void uui_netshare_add(struct uui_netshare *n, struct uui_dialog *d);
// `point` is the share's mount point; `server` is shown in the title;
// `user` fills the user name (NULL or "" for an empty field).
void uui_netshare_signin(struct uui_netshare *n, struct uui_dialog *d, const char *point,
                         const char *server, const char *user);
// The dialog's body: hand it to the app's widget list while the dialog
// is open (uui_dialog_set_body() is done here).
struct uui_item *uui_netshare_item(struct uui_netshare *n);
// The dialog committed `code`. Starts the work, or reopens the dialog
// with an error ("Give the share a name").
void uui_netshare_answer(struct uui_netshare *n, int code);
// Call every tick while BUSY (and after any key the body took). Writes
// the folder to open for UUI_NETSHARE_OPEN.
int uui_netshare_poll(struct uui_netshare *n, char *out_point, size_t cap);
int uui_netshare_is_open(const struct uui_netshare *n);

#endif
