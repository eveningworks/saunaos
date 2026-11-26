// See ui/uui_newaccount.h.
#include "ui/uui_newaccount.h"
#include "ui/utheme.h"
#include "base/uusers.h"
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>

static const char *const TYPES[] = { "Standard", "Administrator" };
static const char *const LABEL[] = { "Full name", "Account name", "Account type" };

static int gap(void)   { return ugfx_char_w(); }
static int row_h(void) { return ugfx_char_h() + ugfx_px(10); }
static int note_h(void) { return ugfx_char_h() + ugfx_px(6); }

void uui_newaccount_init(struct uui_newaccount *a) {
    memset(a, 0, sizeof *a);
    uui_textbox_init(&a->full, "");
    uui_textbox_init(&a->name, "");
    uui_textbox_set_max(&a->name, 32);
    uui_segmented_init(&a->type, TYPES, 2, 0);
    uui_newpassword_init(&a->pw, "Password", "Confirm");
    a->kids[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &a->full, .flags = UUI_FILL_W,
                                    .name = "newacct_full" };
    a->kids[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &a->name, .flags = UUI_FILL_W,
                                    .name = "newacct_name" };
    a->kids[2] = (struct uui_item){ .ops = &uui_segmented_ops, .widget = &a->type, .name = "newacct_type" };
    a->kids[3] = (struct uui_item){ .ops = &uui_newpassword_ops, .widget = &a->pw, .flags = UUI_FILL_W,
                                    .name = "newacct_password" };
}

void uui_newaccount_changed(struct uui_newaccount *a) {
    const char *cur = uui_textbox_text(&a->name);
    // THE PERSON'S EDIT ENDS THE FOLLOWING, until a clear -- even one that
    // empties the field, or clearing it to type a name afresh would
    // refill it from the full name under their fingers.
    if (!a->edited && strcmp(cur, a->derived)) a->edited = 1;
    if (a->edited) return;
    char d[sizeof a->derived];
    uusers_name_from_full(uui_textbox_text(&a->full), d, sizeof d);
    if (!strcmp(d, cur)) { memcpy(a->derived, d, sizeof d); return; }
    memcpy(a->derived, d, sizeof d);
    uui_textbox_set_text(&a->name, d);
}

enum uui_newaccount_state uui_newaccount_state(const struct uui_newaccount *a) {
    const char *n = uui_textbox_text(&a->name);
    if (!n[0]) return UUI_NEWACCT_NO_NAME;
    if (!uusers_name_ok(n)) return UUI_NEWACCT_BAD_NAME;
    if (getpwnam(n) || getgrnam(n)) return UUI_NEWACCT_TAKEN;
    if (!uusers_full_ok(uui_textbox_text(&a->full))) return UUI_NEWACCT_BAD_FULL;
    enum uui_newpassword_state ps = uui_newpassword_state(&a->pw);
    if (ps == UUI_NEWPW_PARTIAL || ps == UUI_NEWPW_DIFFER) return UUI_NEWACCT_PW_UNSURE;
    return UUI_NEWACCT_READY;
}

const char *uui_newaccount_full(const struct uui_newaccount *a) { return uui_textbox_text(&a->full); }
const char *uui_newaccount_name(const struct uui_newaccount *a) { return uui_textbox_text(&a->name); }
int uui_newaccount_admin(const struct uui_newaccount *a) { return a->type.selected == 1; }

const char *uui_newaccount_password(const struct uui_newaccount *a) {
    enum uui_newpassword_state ps = uui_newpassword_state(&a->pw);
    return ps == UUI_NEWPW_EMPTY ? "" : uui_newpassword_text(&a->pw);
}

void uui_newaccount_clear(struct uui_newaccount *a) {
    uui_textbox_set_text(&a->full, "");
    uui_textbox_set_text(&a->name, "");
    a->derived[0] = '\0';
    a->edited = 0;
    a->type.selected = 0;
    uui_newpassword_clear(&a->pw);
}

void uui_newaccount_set_ids(struct uui_newaccount *a, int full_id, int name_id, int type_id, int pw_id,
                            int confirm_id) {
    a->kids[0].id = full_id;
    a->kids[1].id = name_id;
    a->kids[2].id = type_id;
    uui_newpassword_set_ids(&a->pw, pw_id, confirm_id);
}

int uui_newaccount_focusables(struct uui_newaccount *a, struct uui_focusable *out) {
    out[0] = (struct uui_focusable){ &a->full, &uui_textbox_ops };
    out[1] = (struct uui_focusable){ &a->name, &uui_textbox_ops };
    out[2] = (struct uui_focusable){ &a->type, &uui_segmented_ops };
    return 3 + uui_newpassword_focusables(&a->pw, out + 3);
}

// The label column: the widest label of this form and of the password's.
static int label_col(void) {
    static const char *const ALL[] = { "Full name", "Account name", "Account type", "Password", "Confirm" };
    int m = 0;
    for (int i = 0; i < 5; i++) {
        int w = ugfx_text_width(ALL[i]);
        if (w > m) m = w;
    }
    return m + gap();
}

// Rows, top to bottom: full name, account name, its note, type, its
// note, then the password pair (with its own message line) and a note.
static int y_name(void) { return row_h() + gap() / 2; }
static int y_type(void) { return y_name() + row_h() + note_h(); }
static int y_pw(void)   { return y_type() + row_h() + note_h(); }

static void natural(const void *w, int *ow, int *oh) {
    const struct uui_newaccount *a = w;
    int pw_w, pw_h;
    uui_newpassword_ops.natural_size(&a->pw, &pw_w, &pw_h);
    int sw, sh;
    uui_segmented_natural_size(&a->type, &sw, &sh);
    int fw = ugfx_char_w() * 26;
    if (sw > fw) fw = sw;
    *ow = label_col() + fw;
    *oh = y_pw() + pw_h;
}

static void geometry(void *w, int x, int y, int width, int height) {
    struct uui_newaccount *a = w;
    a->x = x; a->y = y; a->w = width; a->h = height;
    a->label_w = label_col();
    int fx = x + a->label_w, fw = width - a->label_w;
    if (fw < ugfx_char_w() * 8) fw = ugfx_char_w() * 8;
    uui_textbox_set_geometry(&a->full, fx, y, fw, row_h());
    uui_textbox_set_geometry(&a->name, fx, y + y_name(), fw, row_h());
    int sw, sh;
    uui_segmented_natural_size(&a->type, &sw, &sh);
    uui_segmented_set_geometry(&a->type, fx, y + y_type() + (row_h() - sh) / 2);
    a->pw.label_min = a->label_w;
    int pw_w, pw_h;
    uui_newpassword_ops.natural_size(&a->pw, &pw_w, &pw_h);
    uui_newpassword_ops.set_geometry(&a->pw, x, y + y_pw(), width, pw_h);
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_newaccount *a = w;
    *x = a->x; *y = a->y; *ow = a->w; *oh = a->h;
}

static struct uui_item *children(void *w, int *n) {
    struct uui_newaccount *a = w;
    *n = 4;
    return a->kids;
}

static void note(struct ugfx_surface *s, const struct uui_newaccount *a, int ry, const char *t, uint32_t c) {
    ugfx_draw_string_clipped(s, a->x + a->label_w, ry + ugfx_px(2), a->w - a->label_w, t, c, UGFX_TRANSPARENT);
}

// The labels and the notes under the fields -- before the router paints
// the fields themselves.
static void paint(struct ugfx_surface *s, void *w) {
    const struct uui_newaccount *a = w;
    uint32_t fg = UTHEME_TEXT, dim = utheme_current()->text_muted, bad = utheme_severity(UTHEME_SEV_ERROR);
    int ch = ugfx_char_h();
    const int rows[3] = { 0, y_name(), y_type() };
    for (int i = 0; i < 3; i++)
        ugfx_draw_string_clipped(s, a->x, a->y + rows[i] + (row_h() - ch) / 2, a->label_w - gap() / 2, LABEL[i],
                                 fg, UGFX_TRANSPARENT);
    char t[96];
    const char *n = uui_textbox_text(&a->name);
    switch (uui_newaccount_state(a)) {
    case UUI_NEWACCT_BAD_NAME:
        note(s, a, a->y + y_name() + row_h(), "Lower-case letters, digits, _ or -, a letter first", bad);
        break;
    case UUI_NEWACCT_TAKEN:
        note(s, a, a->y + y_name() + row_h(), "That name is taken", bad);
        break;
    case UUI_NEWACCT_BAD_FULL:
        note(s, a, a->y + y_name() + row_h(), "The full name is too long, or holds a ':'", bad);
        break;
    case UUI_NEWACCT_NO_NAME:
        break;
    default:
        snprintf(t, sizeof t, "Home: /home/%s", n);
        note(s, a, a->y + y_name() + row_h(), t, dim);
    }
    note(s, a, a->y + y_type() + row_h(), uui_newaccount_admin(a)
                                              ? "Can add people and change the machine"
                                              : "Can use apps and change their own settings", dim);
    // In the password's own message line, which says nothing until the
    // two differ.
    if (uui_newpassword_state(&a->pw) == UUI_NEWPW_EMPTY)
        note(s, a, a->y + y_pw() + row_h() * 2 + gap() / 2 + ugfx_px(2),
             "Left empty, the account signs in without one", dim);
}

// For a painter that does not walk children (uui_layout_draw()).
static void draw(struct ugfx_surface *s, const void *w) {
    struct uui_newaccount *a = (struct uui_newaccount *)w;
    paint(s, a);
    uui_textbox_draw(s, &a->full);
    uui_textbox_draw(s, &a->name);
    uui_segmented_draw(s, &a->type);
    uui_newpassword_ops.draw(s, &a->pw);
}

const struct uui_widget_ops uui_newaccount_ops = {
    .natural_size   = natural,
    .set_geometry   = geometry,
    .bounds         = bounds,
    .draw           = draw,
    .children       = children,
    .children_begin = paint,
};
