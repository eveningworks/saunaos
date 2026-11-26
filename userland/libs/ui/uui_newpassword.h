#ifndef UUI_NEWPASSWORD_H
#define UUI_NEWPASSWORD_H

// uui_newpassword -- a NEW password asked twice: two masked fields, each
// with its label to its left, and a line under them saying what is wrong
// ("The two passwords differ."). The shape of GNOME's and KDE's "Change
// password" dialogs and Windows' "Create a password". System Settings'
// Users page and First-boot setup's Password step share it.
//
// **IT DECIDES NOTHING ABOUT THE PASSWORD** -- no length rule, no
// strength meter: what it says is whether the two fields AGREE, and the
// app's OK follows uui_newpassword_state(). Checking and storing it is
// privd's (base/upriv.h).
//
// A container: the two fields are its CHILDREN, so focus, Tab, the caret
// and the keys reach them through the router as any field's do; this
// paints the labels and the message line around them.

#include "ui/uui_focus.h"
#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

enum uui_newpassword_state {
    UUI_NEWPW_EMPTY = 0,   // nothing typed in either
    UUI_NEWPW_PARTIAL,     // the first typed, the confirmation not yet
    UUI_NEWPW_DIFFER,      // both typed, not the same
    UUI_NEWPW_MATCH,       // both typed and the same: OK may act
};

struct uui_newpassword {
    int x, y, w, h;
    const char *label, *confirm_label;   // not copied; "Password", "Confirm" when NULL
    struct uui_textbox field, confirm;   // OWNED, masked
    struct uui_item kids[2];             // OWNED: the fields as children
    int label_w;                         // set by set_geometry; OWNED
    // The label column's least width, so the fields line up with an
    // enclosing form's (uui_newaccount); 0 is the labels' own.
    int label_min;
};

void uui_newpassword_init(struct uui_newpassword *p, const char *label, const char *confirm_label);
// The password when the two agree, else NULL.
const char *uui_newpassword_text(const struct uui_newpassword *p);
enum uui_newpassword_state uui_newpassword_state(const struct uui_newpassword *p);
// Both fields emptied -- after a save, or a Cancel.
void uui_newpassword_clear(struct uui_newpassword *p);
// The ids the two fields report a change with (uapp_desc.on_widget), so
// the app can re-check uui_newpassword_state() as each key lands -- its
// OK greys until MATCH. 0 (the default) reports nothing.
void uui_newpassword_set_ids(struct uui_newpassword *p, int field_id, int confirm_id);
// The two fields for the app's focus ring (keys go where the RING's focus
// is), in Tab order: writes 2 entries to `out` and returns 2.
int uui_newpassword_focusables(struct uui_newpassword *p, struct uui_focusable *out);

extern const struct uui_widget_ops uui_newpassword_ops;

#ifdef __cplusplus
}
#endif

#endif
