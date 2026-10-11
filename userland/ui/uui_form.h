#ifndef UUI_FORM_H
#define UUI_FORM_H

// A SMALL FORM: labelled text fields, password fields, checkboxes and
// notes stacked down a column, with Tab moving between them -- the body
// of a dialog that asks for more than one thing (Sign in, Add network
// share). QFormLayout's job, and GtkGrid-of-labels' in a GTK dialog.
//
// ONE WIDGET, not a layout of several: uui_dialog routes keys to ONE
// focused body item and cycles its own buttons on Tab, so a form whose
// fields were separate items could never move between them from the
// keyboard. The form owns its fields and its focus, and declines Return
// so the dialog commits.
//
// A label sits ABOVE its field (Windows 11's and GNOME's dialogs) rather
// than beside it: a narrow dialog then needs no label column, and a long
// label never squeezes the field.
#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_textbox.h"
#include "ui/uui_checkbox.h"

#define UUI_FORM_ROWS 8

enum { UUI_FORM_TEXT, UUI_FORM_PASSWORD, UUI_FORM_CHECK, UUI_FORM_NOTE };

struct uui_form_row {
    int kind;
    const char *label;         // above a field; beside a checkbox; the text of a note
    const char *hint;          // a dim line under it, or NULL
    struct uui_textbox field;  // TEXT and PASSWORD
    struct uui_checkbox check; // CHECK
    int y;                     // where the row starts, from set_geometry
};

struct uui_form {
    int x, y, w, h;
    struct uui_form_row rows[UUI_FORM_ROWS];
    int count;
    int focus;                 // the row keys go to, or -1
    int focused;               // the form holds the dialog's focus
    int pressed;               // the row a press landed in, -1 for none
    const char *error;         // a line in the danger colour at the foot, or NULL
    int edits;                 // bumped by every key a field takes: "the user typed"
};

void uui_form_init(struct uui_form *f);
// Each returns the row's index, or -1 when the form is full.
int uui_form_add_text(struct uui_form *f, const char *label, const char *initial, const char *hint);
int uui_form_add_password(struct uui_form *f, const char *label, const char *hint);
int uui_form_add_check(struct uui_form *f, const char *label, int checked, const char *hint);
int uui_form_add_note(struct uui_form *f, const char *text);

const char *uui_form_text(const struct uui_form *f, int row);
void uui_form_set_text(struct uui_form *f, int row, const char *text);
int uui_form_checked(const struct uui_form *f, int row);
void uui_form_set_error(struct uui_form *f, const char *error);
// Puts the keyboard in `row` (a field or a checkbox).
void uui_form_focus(struct uui_form *f, int row);
// Clears every password row -- after a sign-in is sent or cancelled.
void uui_form_wipe_passwords(struct uui_form *f);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_form_ops;

#endif
