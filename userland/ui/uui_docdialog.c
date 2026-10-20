// uui_docdialog.c -- a Markdown document in a window of its own; see the header.
#include "ui/uui_docdialog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/uclip.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uui_button.h"
#include "ui/uui_focus.h"
#include "ui/uui_label.h"
#include "ui/uui_markdown.h"

enum { DD_COPY = 1, DD_CLOSE };

static struct uapp_window *g_win;
static char *g_text, *g_title;
static int g_len;
static struct uui_markdown g_md;
static struct uui_button g_copy, g_close;
static struct uui_label g_note, g_spacer;
static char g_note_text[48];
static struct uui_item g_btn_items[4], g_root_items[2];
static struct uui_layout g_btn_l, g_root_l;
static struct uui_focusable g_focusables[2];
static struct uui_focus g_focus;
static int g_ready;

static struct uui_item item(const struct uui_widget_ops *ops, void *w, int id, const char *name,
                            unsigned flags) {
    return (struct uui_item){ .ops = ops, .widget = w, .id = id, .name = name, .flags = flags };
}

void uui_docdialog_close(void) {
    if (g_win && uapp_window_is_open(g_win)) uapp_window_close(g_win);
    g_win = 0;
}

int uui_docdialog_is_open(void) { return g_win && uapp_window_is_open(g_win); }

static void on_action(struct uapp_window *w, int code) {
    if (code == DD_CLOSE) { uui_docdialog_close(); return; }
    if (code == DD_COPY) {
        snprintf(g_note_text, sizeof g_note_text, "%s",
                 uclip_set_text(g_text, g_len) ? "Copied" : "Could not copy");
        uui_label_init(&g_note, g_note_text);
    }
    uapp_window_redraw(w);
}

static void on_key(struct uapp_window *w, int key, unsigned mods) {
    (void)w; (void)mods;
    if (key == 0x1B) uui_docdialog_close();
}

static void on_close(struct uapp_window *w) { (void)w; uui_docdialog_close(); }

static void build(void) {
    uui_markdown_init(&g_md);
    uui_button_init(&g_copy, 0, 0, 0, 0, "Copy", UUI_COLOR_UNSET, UUI_COLOR_UNSET, DD_COPY);
    uui_button_init(&g_close, 0, 0, 0, 0, "Close", UUI_ROLE(accent), UUI_ROLE(accent_text), DD_CLOSE);
    uui_label_init(&g_spacer, "");
    g_btn_items[0] = item(&uui_label_ops, &g_note, 0, "note", 0);
    g_btn_items[1] = item(&uui_label_ops, &g_spacer, 0, 0, UUI_FILL_W);
    g_btn_items[2] = item(&uui_button_ops, &g_copy, DD_COPY, "copy", 0);
    g_btn_items[3] = item(&uui_button_ops, &g_close, DD_CLOSE, "close", 0);
    g_btn_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_btn_items, .count = 4, .gap = 8 };
    g_root_items[0] = item(&uui_markdown_ops, &g_md, 0, "document", UUI_FILL_W | UUI_FILL_H);
    g_root_items[1] = item(&uui_layout_ops, &g_btn_l, 0, 0, UUI_FILL_W);
    g_root_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_root_items, .count = 2,
                                    .margin = 12, .gap = 10 };
    g_focusables[0] = (struct uui_focusable){ &g_copy, &uui_button_ops };
    g_focusables[1] = (struct uui_focusable){ &g_close, &uui_button_ops };
    uui_focus_init(&g_focus, g_focusables, 2);
    g_ready = 1;
}

void uui_docdialog_open(struct uapp *a, const char *title, const char *markdown, int len) {
    char *t = malloc((size_t)len + 1), *ti = strdup(title);
    if (!t || !ti) { free(t); free(ti); return; }
    memcpy(t, markdown, (size_t)len);
    t[len] = 0;
    if (!g_ready) build();
    // The old text goes only once the widget points at the new.
    uui_markdown_set_text(&g_md, t, len);
    free(g_text);
    free(g_title);
    g_text = t;
    g_len = len;
    g_title = ti;
    g_note_text[0] = 0;
    uui_label_init(&g_note, g_note_text);
    uui_focus_set(&g_focus, 1);
    if (uui_docdialog_is_open()) {
        uapp_window_set_title(g_win, g_title);
        uapp_window_redraw(g_win);
        return;
    }
    struct uapp_window_desc d = {
        .title = g_title,
        .w = ugfx_char_advance('n') * 72,
        .h = ugfx_char_h() * 26,
        .flags = UAPP_WIN_RESIZABLE,
        .widgets = g_root_items,
        .widget_count = 2,
        .layout = &g_root_l,
        .focus = &g_focus,
        .on_action = on_action,
        .on_key = on_key,
        .on_close = on_close,
        .log_prefix = "docdialog",
    };
    g_win = uapp_window_open(a, &d);
}
