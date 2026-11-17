#include "ui/uui_msgbox.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_button.h"
#include "ui/uui_canvas.h"
#include "ui/uui_focus.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_para.h"
#include "ui/uui_route.h"
#include "base/ulog.h"
#include <stdlib.h>
#include <string.h>

#define ID_BUTTON 100   // + the button's index

static struct uapp_window *g_win;
static uui_msgbox_fn g_done;
static void *g_ctx;
static int g_kind, g_count;
static char *g_title;

// The buttons, as many as asked for: allocated with the box, freed with it.
struct mb_button {
    char *label;
    int code;
    unsigned flags;
    struct uui_button b;
};
static struct mb_button *g_b;
static struct uui_item *g_foot_items;          // the spacer, then each button
static struct uui_focusable *g_focusables;

static struct uui_para g_para;
static struct uui_label g_spacer;
static struct uui_item g_body_items[2], g_root_items[2];
static struct uui_layout g_body_l, g_foot_l, g_root_l;
static struct uui_focus g_focus;

// --- the severity badge: a disc in the severity's colour, a glyph on it ----

static struct uui_canvas g_badge;

static int badge_d(void) { return ugfx_char_h() * 2; }

static void badge_natural(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = *oh = badge_d();
}
static void badge_geometry(void *w, int x, int y, int ww, int hh) {
    (void)ww; (void)hh;
    uui_canvas_set_geometry(w, x, y, badge_d(), badge_d());
}
static void badge_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_canvas *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}
static void badge_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_canvas *c = w;
    const struct utheme *th = utheme_current();
    uint32_t disc = g_kind == UUI_MSGBOX_ERROR   ? th->severity[UTHEME_SEV_ERROR]
                  : g_kind == UUI_MSGBOX_WARNING ? th->severity[UTHEME_SEV_WARNING]
                  : th->accent;
    int d = c->w, r = d / 2;
    uui_canvas_fill_ellipse(s, c, r, r, r, r, disc);
    // The glyph is DRAWN, sized to the disc: a font's x or ! is a text
    // size, and at two lines high the disc dwarfed it.
    int t = d / 8 > 2 ? d / 8 : 2;   // stroke
    uint32_t ink = th->accent_text;
    if (g_kind == UUI_MSGBOX_ERROR) {
        int a = d * 18 / 100, h = t * 707 / 2000 + 1;   // half a bar's span, and its half-width per axis
        int x1[] = { r - a + h, r + a + h, r + a - h, r - a - h }, y1[] = { r - a - h, r + a - h, r + a + h, r - a + h };
        int x2[] = { r + a - h, r - a - h, r - a + h, r + a + h }, y2[] = { r - a - h, r + a - h, r + a + h, r - a + h };
        uui_canvas_fill_polygon(s, c, x1, y1, 4, ink);
        uui_canvas_fill_polygon(s, c, x2, y2, 4, ink);
    } else {
        int warn = g_kind == UUI_MSGBOX_WARNING;
        // ! is a bar over a dot, i a dot over a bar.
        int bar0 = warn ? r - d * 30 / 100 : r - d * 10 / 100, bar1 = warn ? r + d * 8 / 100 : r + d * 30 / 100;
        int dot = warn ? r + d * 18 / 100 : r - d * 30 / 100;
        int bx[] = { r - t / 2, r - t / 2 + t, r - t / 2 + t, r - t / 2 }, by[] = { bar0, bar0, bar1, bar1 };
        uui_canvas_fill_polygon(s, c, bx, by, 4, ink);
        uui_canvas_fill_ellipse(s, c, r, dot + t / 2, t * 6 / 10 + 1, t * 6 / 10 + 1, ink);
    }
}
static const struct uui_widget_ops g_badge_ops = {
    .natural_size = badge_natural,
    .set_geometry = badge_geometry,
    .bounds = badge_bounds,
    .draw = badge_draw,
};

// --- answering ---------------------------------------------------------------

static void finish(int code) {
    uui_msgbox_fn done = g_done;
    void *ctx = g_ctx;
    g_done = 0;
    if (g_win) {
        uapp_window_close(g_win);
        g_win = 0;
    }
    uui_para_free(&g_para);
    for (int i = 0; g_b && i < g_count; i++) free(g_b[i].label);
    free(g_b);
    free(g_foot_items);
    free(g_focusables);
    g_b = 0;
    g_foot_items = 0;
    g_focusables = 0;
    g_count = 0;
    free(g_title);
    g_title = 0;
    ulogf("msgbox: answered %d\n", code);
    if (done) done(ctx, code);
}

static int with_flag(unsigned flag) {
    for (int i = 0; i < g_count; i++)
        if (g_b[i].flags & flag) return i;
    return -1;
}

// A click, or Enter or Space on the focused button -- the focus ring
// commits those as a click. RETURN works by FOCUS: the return button has it
// when the box opens, and Enter is the focused button's, as in MessageBox.
// So Enter is never answered here as well: the window hears the same key
// after the ring, and a second answer could reach a box `done` just opened.
static void w_action(struct uapp_window *w, int code) {
    (void)w;
    if (code >= ID_BUTTON && code < ID_BUTTON + g_count) finish(g_b[code - ID_BUTTON].code);
}

static void w_key(struct uapp_window *w, int key, unsigned mods) {
    if (uui_para_key(&g_para, key, mods)) {
        uapp_window_redraw(w);
        return;
    }
    if (key == 0x1B) {
        int i = with_flag(UUI_MSGBOX_ESCAPE);
        if (i >= 0) finish(g_b[i].code);
    }
}

static void w_close(struct uapp_window *w) {
    (void)w;
    int i = with_flag(UUI_MSGBOX_ESCAPE);
    finish(i >= 0 ? g_b[i].code : UUI_MSGBOX_CLOSED);
}

// The message on the field colour, the buttons on a strip of the panel
// colour under a separator -- MessageBox's and KMessageBox's two bands.
static int margin(void) { return ugfx_px(14); }

static void w_draw(struct uapp_window *w, struct uapp_draw *d) {
    int ww = uapp_window_width(w), wh = uapp_window_height(w);
    // The band starts one margin above the buttons: the root's gap is two.
    int foot = g_count ? g_b[0].b.y - margin() : wh;
    ugfx_fill_rect(d->surface, 0, 0, ww, foot, UTHEME_WHITE);
    ugfx_fill_rect(d->surface, 0, foot, ww, wh - foot, UTHEME_PANEL_BG);
    ugfx_fill_rect(d->surface, 0, foot, ww, 1, UTHEME_SEPARATOR);
}

// --- building it ---------------------------------------------------------------

static int take(const struct uui_msgbox *m) {
    if (!m || m->count < 1 || !m->buttons) return 0;
    g_kind = m->kind;
    g_count = m->count;
    g_title = strdup(m->title ? m->title : "");
    g_b = calloc((size_t)g_count, sizeof *g_b);
    g_foot_items = calloc((size_t)g_count + 1, sizeof *g_foot_items);
    g_focusables = calloc((size_t)g_count, sizeof *g_focusables);
    int ok = g_title && g_b && g_foot_items && g_focusables && uui_para_init(&g_para, m->message);
    for (int i = 0; ok && i < g_count; i++) {
        g_b[i].label = strdup(m->buttons[i].label ? m->buttons[i].label : "");
        g_b[i].code = m->buttons[i].code;
        g_b[i].flags = m->buttons[i].flags;
        ok = g_b[i].label != 0;
    }
    if (!ok) ulogf("msgbox: no memory for a box of %d button(s)\n", m->count);
    return ok;
}

static void build(void) {
    int nb = 0;
    if (g_kind != UUI_MSGBOX_PLAIN) {
        uui_canvas_init(&g_badge, 0, 0, badge_d(), badge_d(), UUI_COLOR_UNSET, 0);
        g_body_items[nb++] = (struct uui_item){ .ops = &g_badge_ops, .widget = &g_badge, .name = "badge" };
    }
    g_para.wrap_chars = 52;
    g_para.bg = UUI_ROLE(field_bg);
    g_body_items[nb++] = (struct uui_item){ .ops = &uui_para_ops, .widget = &g_para,
                                            .flags = UUI_FILL_W, .name = "message" };
    g_body_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_body_items, .count = nb, .gap = ugfx_px(14) };

    int nf = 0, first_focus = 0;
    uui_label_init(&g_spacer, "");
    g_foot_items[nf++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_spacer, .flags = UUI_FILL_W };
    for (int i = 0; i < g_count; i++) {
        struct uui_button *b = &g_b[i].b;
        uui_button_init(b, 0, 0, 0, 0, g_b[i].label, UUI_COLOR_UNSET, UUI_COLOR_UNSET, ID_BUTTON + i);
        b->outlined = 1;
        uui_button_set_default(b, (g_b[i].flags & UUI_MSGBOX_RETURN) != 0);
        if (g_b[i].flags & UUI_MSGBOX_RETURN) first_focus = i;
        g_foot_items[nf++] = (struct uui_item){ .ops = &uui_button_ops, .widget = b,
                                                .id = ID_BUTTON + i, .name = g_b[i].label };
        g_focusables[i] = (struct uui_focusable){ b, &uui_button_ops };
    }
    g_foot_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_foot_items, .count = nf, .gap = ugfx_px(8) };

    g_root_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_body_l, .flags = UUI_FILL_W | UUI_FILL_H };
    g_root_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_foot_l, .flags = UUI_FILL_W };
    // A root layout has no "none" margin (<= 0 is the default), so the two
    // bands are one margin round everything and a gap of two between --
    // w_draw() splits the gap.
    g_root_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_root_items, .count = 2,
                                    .margin = margin(), .gap = 2 * margin() };
    uui_focus_init(&g_focus, g_focusables, g_count);
    uui_focus_set(&g_focus, first_focus);
}

int uui_msgbox_is_open(void) { return g_win != 0; }

int uui_msgbox_open(struct uapp *a, const struct uui_msgbox *m, uui_msgbox_fn done, void *ctx) {
    if (g_win || !a) return 0;
    if (!take(m)) {
        finish(UUI_MSGBOX_CLOSED);   // frees what take() got; `done` is not set yet
        return 0;
    }
    build();
    int w, h, minw = ugfx_char_advance('n') * 30;
    uui_layout_natural_size(&g_root_l, &w, &h);
    if (w < minw) w = minw;
    struct uapp_window_desc d = {
        .title = g_title,
        .w = w, .h = h,
        .flags = UAPP_WIN_MODAL,
        .owner = m->owner,
        .widgets = g_root_items,
        .widget_count = 2,
        .layout = &g_root_l,
        .focus = &g_focus,
        .on_action = w_action,
        .on_key = w_key,
        .on_close = w_close,
        .on_draw = w_draw,
        .log_prefix = "msgbox",
    };
    g_win = uapp_window_open(a, &d);
    if (!g_win) {
        finish(UUI_MSGBOX_CLOSED);
        return 0;
    }
    g_done = done;
    g_ctx = ctx;
    ulogf("msgbox: '%s' with %d button(s)\n", g_title, g_count);
    return 1;
}

static int g_answered, g_answer;
static void run_done(void *ctx, int code) {
    (void)ctx;
    g_answered = 1;
    g_answer = code;
}

int uui_msgbox_run(struct uapp *a, const struct uui_msgbox *m, int *code) {
    g_answered = 0;
    if (!uui_msgbox_open(a, m, run_done, 0)) return 0;
    while (!g_answered) {
        if (uapp_pump(a, -1) < 0) {
            g_done = 0;
            finish(UUI_MSGBOX_CLOSED);
            return 0;
        }
    }
    if (code) *code = g_answer;
    return 1;
}
