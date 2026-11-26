#ifndef UUI_NEWACCOUNT_H
#define UUI_NEWACCOUNT_H

// uui_newaccount -- the form a NEW ACCOUNT is made from (users D3a): the
// full name, the account name (made from the full name until the person
// edits it), Standard or Administrator, and an optional password asked
// twice (uui_newpassword). GNOME's Add User dialog and Windows' "Create a
// user": System Settings' Add account dialog now, First-boot setup and
// the installer when they name the first account.
//
// It CHECKS the account name -- base/uusers.h's rule, and that nobody has
// it -- and says so under the field; the app's Add follows
// uui_newaccount_state(). Making the account is privd's (uusers_add()).
//
// A container, as uui_newpassword is: the fields are its CHILDREN, so
// focus, Tab, the caret and the keys reach them through the router.

#include "ui/uui_focus.h"
#include "ui/uui_newpassword.h"
#include "ui/uui_segmented.h"
#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

enum uui_newaccount_state {
    UUI_NEWACCT_NO_NAME = 0,  // no account name yet
    UUI_NEWACCT_BAD_NAME,     // not a name an account may have
    UUI_NEWACCT_TAKEN,        // an account or group has it
    UUI_NEWACCT_BAD_FULL,     // the full name holds a ':' or is past UUSERS_FULL_MAX
    UUI_NEWACCT_PW_UNSURE,    // a password typed once, or the two differ
    UUI_NEWACCT_READY,        // Add may act
};

struct uui_newaccount {
    int x, y, w, h;
    struct uui_textbox full, name;   // OWNED
    struct uui_segmented type;       // OWNED: Standard, Administrator
    struct uui_newpassword pw;       // OWNED
    struct uui_item kids[4];         // OWNED: the four as children
    int label_w;                     // set by set_geometry; OWNED
    char derived[40];                // OWNED: the name last made from the full name
    int edited;                      // OWNED: the person changed the account name
};

void uui_newaccount_init(struct uui_newaccount *a);
// A field changed (uapp_desc.on_widget, for any of the ids below): the
// account name follows the full name while the person has not edited it.
void uui_newaccount_changed(struct uui_newaccount *a);
enum uui_newaccount_state uui_newaccount_state(const struct uui_newaccount *a);
const char *uui_newaccount_full(const struct uui_newaccount *a);
const char *uui_newaccount_name(const struct uui_newaccount *a);
int uui_newaccount_admin(const struct uui_newaccount *a);
// The password when the two fields agree, "" when both are empty, NULL
// while they disagree.
const char *uui_newaccount_password(const struct uui_newaccount *a);
// Every field emptied, Standard chosen -- after an Add, or a Cancel.
void uui_newaccount_clear(struct uui_newaccount *a);
// The ids its children report a change with: full name, account name,
// type, password, confirmation. 0 reports nothing.
void uui_newaccount_set_ids(struct uui_newaccount *a, int full_id, int name_id, int type_id, int pw_id,
                            int confirm_id);
// Its focusables in Tab order: writes 5 entries to `out` and returns 5.
int uui_newaccount_focusables(struct uui_newaccount *a, struct uui_focusable *out);

extern const struct uui_widget_ops uui_newaccount_ops;

#ifdef __cplusplus
}
#endif

#endif
