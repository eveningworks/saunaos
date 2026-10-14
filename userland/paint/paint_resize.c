// Paint's Image > Resize: one dialog, two tabs -- RESIZE PICTURE (scale
// it, by percent or pixels, the aspect kept or not) and CANVAS SIZE (the
// same pixels in a bigger or smaller frame, placed by a 3 x 3 anchor,
// the new room in Colour 2). R1 from the mockups (2026-10-14); the drag
// handles on the picture's edges (R2) are the canvas widget's.
// One of Paint's units -- see paint.h.
#include "paint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uui.h"
#include "ui/uui_anchor.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_dialog.h"
#include "ui/uui_infonote.h"
#include "ui/uui_segmented.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_tabs.h"

enum {
    RS_TABS = PAINT_RESIZE_ID, RS_BY, RS_W, RS_H, RS_KEEP, CS_W, CS_H, CS_ANCHOR,
};
enum { ANS_OK = 400, ANS_CANCEL };
#define MAX_PX 8192
#define MAX_PCT 1000

static struct uui_dialog g_dlg;
static struct uui_tab g_tab_list[2] = { { "Resize picture", 0, 0 }, { "Canvas size", 0, 0 } };
static struct uui_tabs g_tabs;
static const char *const BY[] = { "Percent", "Pixels" };
static struct uui_segmented g_by;
static struct uui_spinbox g_rw, g_rh, g_cw, g_ch;
static struct uui_checkbox g_keep;
static struct uui_anchor g_anchor;
static struct uui_custom g_l_by, g_l_rw, g_l_rh, g_l_cw, g_l_ch, g_l_anchor, g_rnote;
static struct uui_infonote g_cnote;
static char g_rnote_text[64], g_cnote_text[80];
static struct uui_item g_by_it[2], g_rw_it[2], g_rh_it[2], g_cw_it[2], g_ch_it[2], g_an_it[2];
static struct uui_layout g_by_row, g_rw_row, g_rh_row, g_cw_row, g_ch_row, g_an_row;
static struct uui_item g_p1_it[5], g_p2_it[4], g_col_it[2], g_body;
static struct uui_layout g_p1, g_p2, g_col;
static int g_ow, g_oh;   // the picture's size when the dialog opened

struct uui_dialog *resize_dialog(void) { return &g_dlg; }
int resize_is_open(void) { return uui_dialog_is_open(&g_dlg); }

static void draw_text(struct ugfx_surface *s, const struct uui_custom *cu) {
    ugfx_draw_string_clipped(s, cu->x, cu->y + (cu->h - ugfx_char_h()) / 2, cu->w,
                             (const char *)cu->state, UTHEME_TEXT, UGFX_TRANSPARENT);
}

static struct uui_custom label(const char *t, int w) {
    return (struct uui_custom){ .w = w, .h = utheme_control_h(), .draw = draw_text, .state = (void *)t };
}

static void row(struct uui_layout *l, struct uui_item *it, struct uui_custom *lab,
                const struct uui_widget_ops *ops, void *w, int id, const char *name) {
    it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = lab };
    it[1] = (struct uui_item){ .ops = ops, .widget = w, .id = id, .name = name };
    *l = (struct uui_layout){ .dir = UUI_ROW, .items = it, .count = 2 };
}

// A field's value while it is typed in -- uui_spinbox adopts it on Enter.
static int typed(const struct uui_spinbox *s) {
    const char *t = uui_textbox_text(&s->field);
    int v = 0, n = 0;
    for (; t[n] >= '0' && t[n] <= '9' && n < 6; n++) v = v * 10 + (t[n] - '0');
    if (!n || t[n] || v < s->min || v > s->max) return uui_spinbox_value(s);
    return v;
}

static int pct(void) { return g_by.selected == 0; }

static void sync_notes(void) {
    int w = typed(&g_rw), h = typed(&g_rh);
    if (pct()) { w = (int)((long long)g_ow * w / 100); h = (int)((long long)g_oh * h / 100); }
    snprintf(g_rnote_text, sizeof g_rnote_text, "%d \xd7 %d  \xbb  %d \xd7 %d px", g_ow, g_oh,
             w < 1 ? 1 : w, h < 1 ? 1 : h);
    snprintf(g_cnote_text, sizeof g_cnote_text, "The new room is filled with Colour 2 (#%06X).",
             (unsigned)(g_colour[1] & 0xFFFFFF));
}

// The units changed: the same size, said the other way.
static void set_units(int percent) {
    int w = typed(&g_rw), h = typed(&g_rh);
    if (percent) {
        int pw = g_ow ? (int)((long long)w * 100 / g_ow) : 100, ph = g_oh ? (int)((long long)h * 100 / g_oh) : 100;
        uui_spinbox_init(&g_rw, pw < 1 ? 1 : pw, 1, MAX_PCT, 1, "%");
        uui_spinbox_init(&g_rh, ph < 1 ? 1 : ph, 1, MAX_PCT, 1, "%");
    } else {
        int pw = (int)((long long)g_ow * w / 100), ph = (int)((long long)g_oh * h / 100);
        uui_spinbox_init(&g_rw, pw < 1 ? 1 : pw, 1, MAX_PX, 1, "px");
        uui_spinbox_init(&g_rh, ph < 1 ? 1 : ph, 1, MAX_PX, 1, "px");
    }
    // REBUILT, SO TOLD AGAIN: init cleared the focused field's caret, and
    // the dialog still names it -- typing would go into a field that
    // does not know it has the keyboard.
    struct uui_item *f = g_dlg.focus;
    if (f == &g_rw_it[1] || f == &g_rh_it[1]) {
        uui_dialog_focus(&g_dlg, 0);
        uui_dialog_focus(&g_dlg, f);
    }
}

// KEEP THE ASPECT: the other field follows the one just typed in.
static void follow(int from_w) {
    if (!g_keep.checked) return;
    if (pct()) {
        uui_spinbox_set_value(from_w ? &g_rh : &g_rw, typed(from_w ? &g_rw : &g_rh));
        return;
    }
    if (!g_ow || !g_oh) return;
    if (from_w) uui_spinbox_set_value(&g_rh, (int)(((long long)typed(&g_rw) * g_oh + g_ow / 2) / g_ow));
    else uui_spinbox_set_value(&g_rw, (int)(((long long)typed(&g_rh) * g_ow + g_oh / 2) / g_oh));
}

static void show_tab(int t) {
    uui_tabs_select(&g_tabs, t);
    // ONE PAGE SLOT, its page swapped: a hidden item still holds its
    // room in a layout, so two pages stacked showed the second below a
    // blank first.
    g_col_it[1].widget = t == 0 ? (void *)&g_p1 : (void *)&g_p2;
    uui_dialog_set_body(&g_dlg, &g_body, g_dlg.body_w, g_dlg.body_h);   // re-placed
    struct uui_item *r[] = { &g_rw_it[1], &g_rh_it[1] };
    struct uui_item *c[] = { &g_cw_it[1], &g_ch_it[1], &g_an_it[1] };
    if (t == 0) uui_dialog_set_tabs(&g_dlg, r, 2);
    else uui_dialog_set_tabs(&g_dlg, c, 3);
    uui_dialog_focus(&g_dlg, t == 0 ? &g_rw_it[1] : &g_cw_it[1]);
}

void resize_init(void) { uui_dialog_init(&g_dlg); }

void resize_open(struct uapp *a, int canvas_tab) {
    if (uui_dialog_is_open(&g_dlg) || !g_img.px) return;
    g_ow = g_img.w;
    g_oh = g_img.h;
    int lw = ugfx_text_width("Anchor") + ugfx_char_w() * 3;
    uui_tabs_init(&g_tabs, g_tab_list, 2, 0);
    g_tabs.ground = UTHEME_WHITE;   // the dialog's card: no bar beside the tabs
    uui_segmented_init(&g_by, BY, 2, 1);
    uui_spinbox_init(&g_rw, g_ow, 1, MAX_PX, 1, "px");
    uui_spinbox_init(&g_rh, g_oh, 1, MAX_PX, 1, "px");
    uui_spinbox_init(&g_cw, g_ow, 1, MAX_PX, 1, "px");
    uui_spinbox_init(&g_ch, g_oh, 1, MAX_PX, 1, "px");
    uui_checkbox_init(&g_keep, 0, 0, 0, "Keep the aspect ratio", UUI_COLOR_UNSET, UUI_COLOR_UNSET);
    g_keep.checked = 1;
    g_keep.accent = 1;
    uui_anchor_init(&g_anchor, 4);
    g_l_by = label("By", lw); g_l_rw = label("Width", lw); g_l_rh = label("Height", lw);
    g_l_cw = label("Width", lw); g_l_ch = label("Height", lw); g_l_anchor = label("Anchor", lw);
    sync_notes();
    g_rnote = label(g_rnote_text, ugfx_char_w() * 34);
    uui_infonote_init(&g_cnote, g_cnote_text);
    row(&g_by_row, g_by_it, &g_l_by, &uui_segmented_ops, &g_by, RS_BY, "by");
    row(&g_rw_row, g_rw_it, &g_l_rw, &uui_spinbox_ops, &g_rw, RS_W, "rwidth");
    row(&g_rh_row, g_rh_it, &g_l_rh, &uui_spinbox_ops, &g_rh, RS_H, "rheight");
    row(&g_cw_row, g_cw_it, &g_l_cw, &uui_spinbox_ops, &g_cw, CS_W, "cwidth");
    row(&g_ch_row, g_ch_it, &g_l_ch, &uui_spinbox_ops, &g_ch, CS_H, "cheight");
    row(&g_an_row, g_an_it, &g_l_anchor, &uui_anchor_ops, &g_anchor, CS_ANCHOR, "anchor");
    g_p1_it[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_by_row };
    g_p1_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_rw_row };
    g_p1_it[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_rh_row };
    g_p1_it[3] = (struct uui_item){ .ops = &uui_checkbox_ops, .widget = &g_keep, .id = RS_KEEP, .name = "keep" };
    g_p1_it[4] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_rnote };
    g_p1 = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_p1_it, .count = 5 };
    g_p2_it[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_cw_row };
    g_p2_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_ch_row };
    g_p2_it[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_an_row };
    g_p2_it[3] = (struct uui_item){ .ops = &uui_infonote_ops, .widget = &g_cnote, .name = "cnote" };
    g_p2 = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_p2_it, .count = 4 };
    g_col_it[0] = (struct uui_item){ .ops = &uui_tabs_ops, .widget = &g_tabs, .id = RS_TABS,
                                     .flags = UUI_FILL_W, .name = "resizetabs" };
    g_col_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_p1, .flags = UUI_FILL_H };
    g_col = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_col_it, .count = 2 };
    g_body = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_col, .name = "resizebody" };

    // Sized for the TALLER page, so switching tabs moves nothing.
    int w1, h1, w2, h2, tw, th;
    uui_layout_ops.natural_size(&g_p1, &w1, &h1);
    uui_layout_ops.natural_size(&g_p2, &w2, &h2);
    uui_tabs_ops.natural_size(&g_tabs, &tw, &th);
    int bw = w1 > w2 ? w1 : w2, bh = (h1 > h2 ? h1 : h2) + th + utheme_gap();
    if (bw < tw) bw = tw;
    static const struct uui_dialog_button btns[] = {
        { "Cancel", ANS_CANCEL, 0 }, { "OK", ANS_OK, UUI_DLG_PRIMARY },
    };
    uui_dialog_set_bounds(&g_dlg, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_dlg, "Resize", 0, 0, btns, 2, 1, ANS_CANCEL);
    uui_dialog_set_body(&g_dlg, &g_body, bw, bh);
    show_tab(canvas_tab ? 1 : 0);
}

// Scale, or re-frame, into a new picture handed to the window.
static void apply(void) {
    int tab = g_tabs.selected;
    struct uimg out = { 0 };
    if (tab == 0) {
        uui_spinbox_commit(&g_rw);
        uui_spinbox_commit(&g_rh);
        int w = uui_spinbox_value(&g_rw), h = uui_spinbox_value(&g_rh);
        if (pct()) { w = (int)((long long)g_ow * w / 100); h = (int)((long long)g_oh * h / 100); }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (w == g_img.w && h == g_img.h) return;
        if (uimg_scale(&g_img, w, h, &out) != 0) { paint_note("Not enough memory to resize"); return; }
        ulogf("paint: resized to %dx%d\n", w, h);
    } else {
        uui_spinbox_commit(&g_cw);
        uui_spinbox_commit(&g_ch);
        int w = uui_spinbox_value(&g_cw), h = uui_spinbox_value(&g_ch), col, rw;
        if (w == g_img.w && h == g_img.h) return;
        uui_anchor_xy(&g_anchor, &col, &rw);
        out.px = malloc((size_t)w * h * 4);
        if (!out.px) { paint_note("Not enough memory for that canvas"); return; }
        out.w = w;
        out.h = h;
        for (size_t i = 0; i < (size_t)w * h; i++) out.px[i] = g_colour[1];
        // Where the old picture's top-left lands: against the anchored
        // edge, or centred between the two.
        int dx = col == 0 ? 0 : col == 1 ? (w - g_img.w) / 2 : w - g_img.w;
        int dy = rw == 0 ? 0 : rw == 1 ? (h - g_img.h) / 2 : h - g_img.h;
        for (int y = 0; y < g_img.h; y++) {
            int ty = y + dy;
            if (ty < 0 || ty >= h) continue;
            for (int x = 0; x < g_img.w; x++) {
                int tx = x + dx;
                if (tx >= 0 && tx < w) out.px[(size_t)ty * w + tx] = g_img.px[(size_t)y * g_img.w + x];
            }
        }
        out.has_alpha = g_img.has_alpha || (g_colour[1] >> 24) != 0xFF;
        ulogf("paint: canvas %dx%d anchor %d\n", w, h, g_anchor.selected);
    }
    paint_replace_picture(&out);
}

static void answer(int code) {
    uui_dialog_close(&g_dlg);
    uui_dialog_set_body(&g_dlg, 0, 0, 0);
    if (code == ANS_OK) apply();
}

int resize_on_widget(int id, int reason) {
    if (id < PAINT_RESIZE_ID || id >= PAINT_RESIZE_ID + PAINT_RESIZE_IDS) return 0;
    switch (id) {
    case RS_TABS:
        if (g_tabs.selected == 0 || g_tabs.selected == 1) show_tab(g_tabs.selected);
        break;
    case RS_BY:
        set_units(pct());
        break;
    case RS_W: case RS_H: case CS_W: case CS_H: case CS_ANCHOR: {
        struct uui_item *it = id == RS_W ? &g_rw_it[1] : id == RS_H ? &g_rh_it[1]
                            : id == CS_W ? &g_cw_it[1] : id == CS_H ? &g_ch_it[1] : &g_an_it[1];
        if (reason == UUI_REASON_PRESS) uui_dialog_focus(&g_dlg, it);
        if (id == RS_W || id == RS_H) follow(id == RS_W);
        break;
    }
    default:
        break;
    }
    sync_notes();
    return 1;
}

// A key through the dialog: the field gets it, the aspect follows.
int resize_key(int key) {
    if (!uui_dialog_is_open(&g_dlg)) return 0;
    struct uui_item *was = g_dlg.focus;
    uui_dialog_key(&g_dlg, key);
    if (was == &g_rw_it[1] || was == &g_rh_it[1]) follow(was == &g_rw_it[1]);
    sync_notes();
    int code = uui_dialog_take_code(&g_dlg);
    if (code > 0) answer(code);
    return 1;
}

// The dialog's own id: a button or a key the dialog answered.
int resize_take(void) {
    int code = uui_dialog_take_code(&g_dlg);
    if (code > 0) { answer(code); return 1; }
    struct uui_item *f = g_dlg.focus;
    if (f == &g_rw_it[1] || f == &g_rh_it[1]) follow(f == &g_rw_it[1]);
    sync_notes();
    return 0;
}
