#ifndef UUI_SIGNIN_H
#define UUI_SIGNIN_H

// A SIGN-IN DIALOG for any app with a uui_dialog: a title, a line or two
// saying who is asking, a user name, a password, and optionally a
// Remember row -- the form, its checks, and the wiping. NOT what the
// answer is for: the File Manager's network shares hand it to smbfsd
// (ui/uui_netshare.h), the Web Browser to /bin/webnet. GTK's
// GtkMountOperation, the dialog Chrome shows for HTTP authentication.
//
// THE PASSWORD IS THE CALLER'S FOR AS LONG AS IT TAKES TO HAND IT ON:
// uui_signin_answer() copies it out and wipes the field; the caller wipes
// its copy once sent. A refused answer comes back as uui_signin_reask(),
// which reopens the dialog with the error and the user name kept.
//
//     uui_signin_open(&si, &dlg, &spec);   // the body item is uui_signin_item(&si)
//     on a commit:
//         switch (uui_signin_answer(&si, code, user, sizeof user, pass, sizeof pass, &keep)) ...
#include <stddef.h>
#include "ui/uui_dialog.h"
#include "ui/uui_form.h"
#include "ui/uui_widget.h"

// What the dialog asks. Every string is copied; NULL leaves a part out.
struct uui_signin_spec {
    const char *title;           // "Sign in to example.com"
    const char *lines[3];        // under the title
    const char *user;            // the name the field starts with
    const char *remember;        // a Remember row's label
    const char *remember_hint;
    const char *note;            // the dialog's tinted line: a consequence ("not secure")
    const char *error;           // shown at once, as after a refusal
};

enum { UUI_SIGNIN_CANCELLED, UUI_SIGNIN_GIVEN, UUI_SIGNIN_REASKED };

struct uui_signin {
    struct uui_dialog *dlg;
    struct uui_form form;
    struct uui_item item;
    char title[96];
    char lines[3][160];
    const char *rows[3];
    int nrows;
    char note[160];
    char error[128];
    int f_user, f_pass, f_remember;   // f_remember -1 when there is none
};

void uui_signin_open(struct uui_signin *s, struct uui_dialog *d, const struct uui_signin_spec *spec);
// The dialog's body: in the app's widget list while the dialog is open.
struct uui_item *uui_signin_item(struct uui_signin *s);
int  uui_signin_is_open(const struct uui_signin *s);
// The dialog committed `code`. GIVEN writes the user name, the password
// and whether to remember it (0 with no Remember row), and closes the
// dialog; REASKED means it reopened itself (no user name typed);
// CANCELLED wrote nothing.
int  uui_signin_answer(struct uui_signin *s, int code, char *user, size_t ucap, char *pass, size_t pcap,
                       int *remember);
// The answer was refused: the dialog again, with `error`, the user name
// kept and the password field focused.
void uui_signin_reask(struct uui_signin *s, const char *error);

#endif
