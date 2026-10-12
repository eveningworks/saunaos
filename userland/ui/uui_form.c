// See uui_form.h. Rows from the font: a label line, the control, an
// optional hint line, and a gap; the error, when there is one, last.
#include "ui/uui_form.h"
#include <string.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_label.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

#define NOTE_LINES 2

static int gap(void) { return ugfx_char_h() / 2; }
static int line_h(void) { return ugfx_char_h() + 2; }

// The card a form sits on (uui_dialog's), for anti-aliased text.
static uint32_t ground(void) { return UTHEME_WHITE; }
static uint32_t dim(void) { return ugfx_blend(ground(), UTHEME_TEXT, 165); }

static int field_h(const struct uui_form_row *r) {
    int w, h;
    uui_textbox_natural_size(&r->field, &w, &h);
    return h;
}

static int check_h(const struct uui_form_row *r) {
    int w, h;
    uui_checkbox_natural_size(&r->check, &w, &h);
    return h > line_h() ? h : line_h();
}

static int row_h(const struct uui_form_row *r) {
    int h;
    switch (r->kind) {
    case UUI_FORM_TEXT: case UUI_FORM_PASSWORD: h = line_h() + field_h(r); break;
    case UUI_FORM_CHECK: h = check_h(r); break;
    default: h = NOTE_LINES * line_h(); break;
    }
    return h + (r->hint ? line_h() : 0);
}

void uui_form_init(struct uui_form *f) {
    memset(f, 0, sizeof *f);
    f->focus = -1;
    f->pressed = -1;
}

static struct uui_form_row *add(struct uui_form *f, int kind, const char *label, const char *hint) {
    if (f->count >= UUI_FORM_ROWS) return NULL;
    struct uui_form_row *r = &f->rows[f->count++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->label = label;
    r->hint = hint;
    return r;
}

int uui_form_add_text(struct uui_form *f, const char *label, const char *initial, const char *hint) {
    struct uui_form_row *r = add(f, UUI_FORM_TEXT, label, hint);
    if (!r) return -1;
    uui_textbox_init(&r->field, initial ? initial : "");
    if (f->focus < 0) f->focus = f->count - 1;
    return f->count - 1;
}

int uui_form_add_password(struct uui_form *f, const char *label, const char *hint) {
    struct uui_form_row *r = add(f, UUI_FORM_PASSWORD, label, hint);
    if (!r) return -1;
    uui_textbox_init(&r->field, "");
    r->field.masked = 1;
    if (f->focus < 0) f->focus = f->count - 1;
    return f->count - 1;
}

int uui_form_add_check(struct uui_form *f, const char *label, int checked, const char *hint) {
    struct uui_form_row *r = add(f, UUI_FORM_CHECK, label, hint);
    if (!r) return -1;
    uui_checkbox_init(&r->check, 0, 0, 0, label, ground(), UUI_ROLE(text));
    r->check.checked = checked;
    r->check.accent = 1;
    return f->count - 1;
}

int uui_form_add_note(struct uui_form *f, const char *text) {
    return add(f, UUI_FORM_NOTE, text, NULL) ? f->count - 1 : -1;
}

static int is_field(const struct uui_form_row *r) {
    return r->kind == UUI_FORM_TEXT || r->kind == UUI_FORM_PASSWORD;
}

static int takes_focus(const struct uui_form_row *r) { return is_field(r) || r->kind == UUI_FORM_CHECK; }

const char *uui_form_text(const struct uui_form *f, int row) {
    return row >= 0 && row < f->count && is_field(&f->rows[row]) ? uui_textbox_text(&f->rows[row].field) : "";
}

void uui_form_set_text(struct uui_form *f, int row, const char *text) {
    if (row >= 0 && row < f->count && is_field(&f->rows[row])) uui_textbox_set_text(&f->rows[row].field, text);
}

int uui_form_checked(const struct uui_form *f, int row) {
    return row >= 0 && row < f->count && f->rows[row].kind == UUI_FORM_CHECK && f->rows[row].check.checked;
}

void uui_form_set_error(struct uui_form *f, const char *error) { f->error = error; }

static void show_focus(struct uui_form *f) {
    for (int i = 0; i < f->count; i++) {
        struct uui_form_row *r = &f->rows[i];
        int on = f->focused && i == f->focus;
        if (is_field(r)) uui_textbox_set_active(&r->field, on);
        else if (r->kind == UUI_FORM_CHECK) r->check.focused = on;
    }
}

void uui_form_focus(struct uui_form *f, int row) {
    if (row >= 0 && row < f->count && takes_focus(&f->rows[row])) f->focus = row;
    show_focus(f);
}

void uui_form_wipe_passwords(struct uui_form *f) {
    for (int i = 0; i < f->count; i++)
        if (f->rows[i].kind == UUI_FORM_PASSWORD) {
            memset(f->rows[i].field.buf, 0, sizeof f->rows[i].field.buf);
            uui_textbox_set_text(&f->rows[i].field, "");
        }
}

// --- ops -----------------------------------------------------------------

static void ops_natural_size(const void *w, int *ow, int *oh) {
    const struct uui_form *f = w;
    int h = 0, wide = 0;
    for (int i = 0; i < f->count; i++) {
        const struct uui_form_row *r = &f->rows[i];
        h += row_h(r) + (i ? gap() : 0);
        int lw = ugfx_text_width(r->label ? r->label : "");
        if (r->kind != UUI_FORM_NOTE && lw > wide) wide = lw;
    }
    if (f->error) h += gap() + NOTE_LINES * line_h();
    *ow = wide;
    *oh = h;
}

static void ops_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_form *f = w;
    f->x = x; f->y = y; f->w = width; f->h = height;
    int cy = y;
    for (int i = 0; i < f->count; i++) {
        struct uui_form_row *r = &f->rows[i];
        r->y = cy;
        if (is_field(r)) uui_textbox_set_geometry(&r->field, x, cy + line_h(), width, field_h(r));
        else if (r->kind == UUI_FORM_CHECK) {
            int cw, ch;
            uui_checkbox_natural_size(&r->check, &cw, &ch);
            uui_checkbox_set_geometry(&r->check, x, cy + (check_h(r) - ch) / 2);
        }
        cy += row_h(r) + gap();
    }
}

static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_form *f = w;
    *x = f->x; *y = f->y; *ow = f->w; *oh = f->h;
}

// Up to NOTE_LINES lines of `text`, word-wrapped to the form's width.
static void wrapped(struct ugfx_surface *s, const struct uui_form *f, int y, const char *text, uint32_t ink) {
    char line[160];
    for (int n = 0; n < NOTE_LINES && text && *text; n++) {
        text = uui_label_wrap_next(text, f->w, line, sizeof line);
        ugfx_draw_string_clipped(s, f->x, y + n * line_h(), f->w, line, ink, ground());
    }
}

static void ops_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_form *f = w;
    for (int i = 0; i < f->count; i++) {
        const struct uui_form_row *r = &f->rows[i];
        int y = r->y;
        if (is_field(r)) {
            ugfx_draw_string_clipped(s, f->x, y, f->w, r->label ? r->label : "", UTHEME_TEXT, ground());
            uui_textbox_draw(s, &r->field);
            y += line_h() + field_h(r);
        } else if (r->kind == UUI_FORM_CHECK) {
            uui_checkbox_draw(s, &r->check);
            y += check_h(r);
        } else {
            wrapped(s, f, y, r->label, dim());
            y += NOTE_LINES * line_h();
        }
        // A hint under a checkbox lines up with its label, not its box.
        if (r->hint) {
            int indent = r->kind == UUI_FORM_CHECK ? r->check.size + ugfx_char_w() : 0;
            if (r->kind == UUI_FORM_CHECK && r->check.size <= 0) indent = utheme_indicator() + ugfx_char_w();
            ugfx_draw_string_clipped(s, f->x + indent, y + 2, f->w - indent, r->hint, dim(), ground());
        }
    }
    if (f->error) {
        int y = f->y + f->h - NOTE_LINES * line_h();
        wrapped(s, f, y, f->error, utheme_action(UTHEME_ACT_DANGER));
    }
}

static int row_at(const struct uui_form *f, int cx, int cy) {
    for (int i = 0; i < f->count; i++) {
        const struct uui_form_row *r = &f->rows[i];
        if (is_field(r) && uui_textbox_hit(&r->field, cx, cy)) return i;
        if (r->kind == UUI_FORM_CHECK && uui_checkbox_hit(&r->check, cx, cy)) return i;
    }
    return -1;
}

static int ops_hit(const void *w, int cx, int cy) { return row_at(w, cx, cy) >= 0; }

static int ops_press(void *w, int cx, int cy, unsigned mods) {
    struct uui_form *f = w;
    int i = row_at(f, cx, cy);
    f->pressed = i;
    if (i < 0) return 0;
    f->focus = i;
    f->focused = 1;
    show_focus(f);
    if (is_field(&f->rows[i])) return uui_textbox_ops.press(&f->rows[i].field, cx, cy, mods);
    return 1;
}

static int ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_form *f = w;
    if (f->pressed >= 0 && is_field(&f->rows[f->pressed]))
        return uui_textbox_ops.motion(&f->rows[f->pressed].field, cx, cy, buttons);
    return 0;
}

// A checkbox toggles on RELEASE over itself, so a press can be dragged
// off and cancelled (docs/gui-guidelines.md).
static int ops_release(void *w, int cx, int cy) {
    struct uui_form *f = w;
    int i = f->pressed;
    f->pressed = -1;
    if (i >= 0 && f->rows[i].kind == UUI_FORM_CHECK && uui_checkbox_hit(&f->rows[i].check, cx, cy))
        uui_checkbox_toggle(&f->rows[i].check);
    return 1;
}

static int ops_key(void *w, int key, unsigned mods) {
    struct uui_form *f = w;
    if (key == '\t') {
        // The next row that takes the keyboard, round to the first.
        for (int k = 1; k <= f->count; k++) {
            int i = (f->focus + k) % f->count;
            if (takes_focus(&f->rows[i])) { f->focus = i; break; }
        }
        show_focus(f);
        return 1;
    }
    if (f->focus < 0 || f->focus >= f->count) return 0;
    struct uui_form_row *r = &f->rows[f->focus];
    if (r->kind == UUI_FORM_CHECK) return key == ' ' ? (uui_checkbox_toggle(&r->check), 1) : 0;
    if (!is_field(r)) return 0;
    int used = uui_textbox_key_mods(&r->field, key, mods);
    if (used) f->edits++;
    return used;
}

static void ops_set_focused(void *w, int focused) {
    struct uui_form *f = w;
    f->focused = focused;
    show_focus(f);
}

static int ops_accepts_focus(const void *w) { (void)w; return 1; }

static int ops_cursor(const void *w, int cx, int cy) {
    const struct uui_form *f = w;
    int i = row_at(f, cx, cy);
    return i >= 0 && is_field(&f->rows[i]) ? WIN_CURSOR_TEXT : WIN_CURSOR_DEFAULT;
}

// The field under the pointer (or the focused one), for the shared
// right-click edit menu -- never a password's, which is not to be copied.
static int ops_edit_target(void *w, int cx, int cy, struct uui_edit_target *out) {
    struct uui_form *f = w;
    for (int i = 0; i < f->count; i++) {
        struct uui_form_row *r = &f->rows[i];
        if (r->kind == UUI_FORM_TEXT && uui_textbox_edit_target(&r->field, cx, cy, out)) return 1;
    }
    return 0;
}

const struct uui_widget_ops uui_form_ops = {
    .natural_size  = ops_natural_size,
    .set_geometry  = ops_set_geometry,
    .bounds        = ops_bounds,
    .draw          = ops_draw,
    .hit           = ops_hit,
    .press         = ops_press,
    .motion        = ops_motion,
    .release       = ops_release,
    .key           = ops_key,
    .set_focused   = ops_set_focused,
    .accepts_focus = ops_accepts_focus,
    .cursor        = ops_cursor,
    .edit_target   = ops_edit_target,
};
