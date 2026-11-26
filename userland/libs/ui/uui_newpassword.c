// See ui/uui_newpassword.h.
#include "ui/uui_newpassword.h"
#include "ui/utheme.h"
#include <string.h>

static int gap(void)   { return ugfx_char_w(); }
static int row_h(void) { return ugfx_char_h() + ugfx_px(10); }
static int msg_h(void) { return ugfx_char_h() + ugfx_px(4); }

static const char *lab(const struct uui_newpassword *p, int i) {
    return i ? (p->confirm_label ? p->confirm_label : "Confirm")
             : (p->label ? p->label : "Password");
}

static void masked(struct uui_textbox *f) {
    uui_textbox_init(f, "");
    f->masked = 1;
    f->ed.flags |= UUI_EDIT_MASKED;
}

void uui_newpassword_init(struct uui_newpassword *p, const char *label, const char *confirm_label) {
    memset(p, 0, sizeof *p);
    p->label = label;
    p->confirm_label = confirm_label;
    masked(&p->field);
    masked(&p->confirm);
    p->kids[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &p->field, .flags = UUI_FILL_W,
                                    .name = "newpw_field" };
    p->kids[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &p->confirm, .flags = UUI_FILL_W,
                                    .name = "newpw_confirm" };
}

enum uui_newpassword_state uui_newpassword_state(const struct uui_newpassword *p) {
    const char *a = uui_textbox_text(&p->field), *b = uui_textbox_text(&p->confirm);
    if (!a[0] && !b[0]) return UUI_NEWPW_EMPTY;
    if (!b[0]) return UUI_NEWPW_PARTIAL;
    return strcmp(a, b) ? UUI_NEWPW_DIFFER : UUI_NEWPW_MATCH;
}

const char *uui_newpassword_text(const struct uui_newpassword *p) {
    return uui_newpassword_state(p) == UUI_NEWPW_MATCH ? uui_textbox_text(&p->field) : 0;
}

void uui_newpassword_clear(struct uui_newpassword *p) {
    uui_textbox_set_text(&p->field, "");
    uui_textbox_set_text(&p->confirm, "");
}

int uui_newpassword_focusables(struct uui_newpassword *p, struct uui_focusable *out) {
    out[0] = (struct uui_focusable){ &p->field, &uui_textbox_ops };
    out[1] = (struct uui_focusable){ &p->confirm, &uui_textbox_ops };
    return 2;
}

// The label column: the wider label and a gap, or label_min.
static int label_col(const struct uui_newpassword *p) {
    int lw = ugfx_text_width(lab(p, 0)), cw = ugfx_text_width(lab(p, 1));
    int c = (lw > cw ? lw : cw) + gap();
    return c > p->label_min ? c : p->label_min;
}

static void natural(const void *w, int *ow, int *oh) {
    const struct uui_newpassword *p = w;
    *ow = label_col(p) + ugfx_char_w() * 24;
    *oh = row_h() * 2 + gap() / 2 + msg_h();
}

static void geometry(void *w, int x, int y, int width, int height) {
    struct uui_newpassword *p = w;
    p->x = x; p->y = y; p->w = width; p->h = height;
    p->label_w = label_col(p);
    int fx = x + p->label_w, fw = width - p->label_w;
    if (fw < ugfx_char_w() * 8) fw = ugfx_char_w() * 8;
    uui_textbox_set_geometry(&p->field, fx, y, fw, row_h());
    uui_textbox_set_geometry(&p->confirm, fx, y + row_h() + gap() / 2, fw, row_h());
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_newpassword *p = w;
    *x = p->x; *y = p->y; *ow = p->w; *oh = p->h;
}

static struct uui_item *children(void *w, int *n) {
    struct uui_newpassword *p = w;
    *n = 2;
    return p->kids;
}

// The labels, centred on their fields, and the message under them --
// drawn before the router paints the fields themselves.
static void paint(struct ugfx_surface *s, void *w) {
    const struct uui_newpassword *p = w;
    uint32_t fg = UTHEME_TEXT;
    int ch = ugfx_char_h();
    for (int i = 0; i < 2; i++) {
        int ry = p->y + i * (row_h() + gap() / 2);
        ugfx_draw_string_clipped(s, p->x, ry + (row_h() - ch) / 2, p->label_w - gap() / 2, lab(p, i), fg,
                                 UGFX_TRANSPARENT);
    }
    if (uui_newpassword_state(p) != UUI_NEWPW_DIFFER) return;
    int my = p->y + row_h() * 2 + gap() / 2 + ugfx_px(4);
    ugfx_draw_string_clipped(s, p->x + p->label_w, my, p->w - p->label_w, "The two passwords differ.",
                             utheme_severity(UTHEME_SEV_ERROR), UGFX_TRANSPARENT);
}

// For a painter that does not walk children -- uui_layout_draw(), which a
// dialog window's layout is drawn with (uapp_window.c): the labels, then
// the fields themselves. The router never calls this; it walks children.
static void draw(struct ugfx_surface *s, const void *w) {
    struct uui_newpassword *p = (struct uui_newpassword *)w;
    paint(s, p);
    uui_textbox_draw(s, &p->field);
    uui_textbox_draw(s, &p->confirm);
}

void uui_newpassword_set_ids(struct uui_newpassword *p, int field_id, int confirm_id) {
    p->kids[0].id = field_id;
    p->kids[1].id = confirm_id;
}

const struct uui_widget_ops uui_newpassword_ops = {
    .natural_size   = natural,
    .set_geometry   = geometry,
    .bounds         = bounds,
    .draw           = draw,
    .children       = children,
    .children_begin = paint,
};
