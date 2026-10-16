// See uui_signin.h.
#include "ui/uui_signin.h"
#include <stdio.h>
#include <string.h>
#include "lib/icon_cache.h"
#include "ui/ugfx.h"

// Positive: an app dispatches a dialog answer only when its code is > 0.
enum { SI_OK = 1, SI_CANCEL = 2 };

static void show(struct uui_signin *s) {
    static const struct uui_dialog_button btns[2] = {
        { "Cancel", SI_CANCEL, 0 }, { "Sign in", SI_OK, UUI_DLG_PRIMARY },
    };
    uui_dialog_open(s->dlg, s->title, s->rows, s->nrows, btns, 2, 1, SI_CANCEL);
    int w, h;
    uui_form_ops.natural_size(&s->form, &w, &h);
    // The fields from the card's left edge, under the note, and as wide
    // as the message makes the card: a wider body would only leave space
    // beside the text.
    s->dlg->body_full = 1;
    uui_dialog_set_body(s->dlg, &s->item, ugfx_char_w() * 24, h);
    uui_dialog_focus(s->dlg, &s->item);
    uui_dialog_set_picture(s->dlg, icon_get("lock", ugfx_char_h() * 3));
    if (s->note[0]) uui_dialog_set_note(s->dlg, s->note);
    uui_form_set_error(&s->form, s->error[0] ? s->error : NULL);
    const char *user = uui_form_text(&s->form, s->f_user);
    uui_form_focus(&s->form, user[0] ? s->f_pass : s->f_user);
}

void uui_signin_open(struct uui_signin *s, struct uui_dialog *d, const struct uui_signin_spec *spec) {
    memset(s, 0, sizeof *s);
    s->dlg = d;
    uui_form_init(&s->form);
    s->item = (struct uui_item){ .ops = &uui_form_ops, .widget = &s->form, .name = "signin" };
    snprintf(s->title, sizeof s->title, "%s", spec->title ? spec->title : "Sign in");
    for (int i = 0; i < 3 && spec->lines[i]; i++) {
        snprintf(s->lines[i], sizeof s->lines[i], "%s", spec->lines[i]);
        s->rows[s->nrows++] = s->lines[i];
    }
    snprintf(s->note, sizeof s->note, "%s", spec->note ? spec->note : "");
    snprintf(s->error, sizeof s->error, "%s", spec->error ? spec->error : "");
    s->f_user = uui_form_add_text(&s->form, "User name", spec->user ? spec->user : "", NULL);
    s->f_pass = uui_form_add_password(&s->form, "Password", NULL);
    s->f_remember = spec->remember ? uui_form_add_check(&s->form, spec->remember, 0, spec->remember_hint) : -1;
    show(s);
}

struct uui_item *uui_signin_item(struct uui_signin *s) { return &s->item; }

int uui_signin_is_open(const struct uui_signin *s) {
    return s->dlg && uui_dialog_is_open(s->dlg) && s->dlg->body == &s->item;
}

void uui_signin_reask(struct uui_signin *s, const char *error) {
    snprintf(s->error, sizeof s->error, "%s", error ? error : "");
    show(s);
}

int uui_signin_answer(struct uui_signin *s, int code, char *user, size_t ucap, char *pass, size_t pcap,
                      int *remember) {
    uui_dialog_set_body(s->dlg, NULL, 0, 0);
    if (code != SI_OK) {
        uui_form_wipe_passwords(&s->form);
        return UUI_SIGNIN_CANCELLED;
    }
    const char *u = uui_form_text(&s->form, s->f_user);
    if (!u[0]) {
        uui_signin_reask(s, "Enter a user name.");
        return UUI_SIGNIN_REASKED;
    }
    snprintf(user, ucap, "%s", u);
    snprintf(pass, pcap, "%s", uui_form_text(&s->form, s->f_pass));
    *remember = s->f_remember >= 0 && uui_form_checked(&s->form, s->f_remember);
    uui_form_wipe_passwords(&s->form);
    return UUI_SIGNIN_GIVEN;
}
