// See uui_nameask.h.
#include "ui/uui_nameask.h"
#include <string.h>

enum { NA_OK = 1, NA_CANCEL, NA_FIELD };

// One prompt at a time per process is all anything asks for, so the
// window's callbacks find their prompt here rather than through a
// context pointer the window API does not carry.
static struct uui_nameask *g_q;

static void na_close(struct uapp_window *w) {
    uapp_window_close(w);
    if (g_q) g_q->win = 0;
}

static void na_ok(struct uapp_window *w) {
    struct uui_nameask *q = g_q;
    if (!q) return;
    const char *name = uui_textbox_text(&q->field);
    const char *why = q->check ? q->check(q->ctx, name) : 0;
    if (why) {
        q->err.text = why;
        uapp_window_redraw(w);
        return;
    }
    // Copied first: closing the window takes the field with it.
    char keep[128];
    strlcpy(keep, name, sizeof keep);
    na_close(w);
    if (q->done) q->done(q->ctx, keep);
}

static void na_on_action(struct uapp_window *w, int code) {
    if (code == NA_OK) na_ok(w);
    else if (code == NA_CANCEL) na_close(w);
}

static void na_on_key(struct uapp_window *w, int key, unsigned mods) {
    (void)mods;
    if (key == '\n' || key == '\r') { na_ok(w); return; }
    if (g_q) g_q->err.text = "";
    uapp_window_redraw(w);
}

int uui_nameask_is_open(const struct uui_nameask *q) {
    return q->win && uapp_window_is_open(q->win);
}

void uui_nameask_open(struct uui_nameask *q, struct uapp *a, const char *title,
                      const char *caption, const char *initial, const char *ok_label,
                      const char *(*check)(void *ctx, const char *name),
                      void (*done)(void *ctx, const char *name), void *ctx) {
    if (uui_nameask_is_open(q)) return;
    g_q = q;
    q->check = check;
    q->done = done;
    q->ctx = ctx;
    uui_label_init(&q->caption, caption);
    uui_textbox_init(&q->field, initial);
    uui_textbox_select(&q->field, 0, (int)strlen(initial));
    uui_label_init(&q->err, "");
    q->err.fg = utheme_severity(UTHEME_SEV_ERROR);
    uui_label_init(&q->gap, "");
    uui_button_init(&q->ok, 0, 0, 0, 0, ok_label, UTHEME_ACCENT, UTHEME_ACCENT_TEXT, NA_OK);
    uui_button_init(&q->cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, NA_CANCEL);
    q->btns[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &q->gap, .flags = UUI_FILL_W };
    q->btns[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &q->cancel, .id = NA_CANCEL, .name = "nameask_cancel" };
    q->btns[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &q->ok, .id = NA_OK, .name = "nameask_ok" };
    q->btn_row = (struct uui_layout){ .dir = UUI_ROW, .items = q->btns, .count = 3 };
    q->items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &q->caption };
    q->items[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &q->field, .id = NA_FIELD,
                                     .flags = UUI_FILL_W, .name = "nameask_field" };
    q->items[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &q->err, .flags = UUI_FILL_W,
                                     .name = "nameask_error" };
    q->items[3] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &q->btn_row, .flags = UUI_FILL_W };
    q->layout = (struct uui_layout){ .dir = UUI_COLUMN, .items = q->items, .count = 4 };
    q->focus_items[0] = (struct uui_focusable){ &q->field, &uui_textbox_focus_ops };
    q->focus_items[1] = (struct uui_focusable){ &q->cancel, &uui_button_ops };
    q->focus_items[2] = (struct uui_focusable){ &q->ok, &uui_button_ops };
    uui_focus_init(&q->focus, q->focus_items, 3);
    uui_focus_set(&q->focus, 0);
    int w = 0, h = 0;
    uui_layout_natural_size(&q->layout, &w, &h);
    if (w < ugfx_char_w() * 30) w = ugfx_char_w() * 30;
    q->win = uapp_window_open(a, &(struct uapp_window_desc){
        .title = title, .w = w, .h = h, .flags = UAPP_WIN_MODAL,
        .widgets = q->items, .widget_count = 4, .layout = &q->layout, .focus = &q->focus,
        .on_action = na_on_action, .on_key = na_on_key, .on_close = na_close,
        .log_prefix = "nameask",
    });
}
