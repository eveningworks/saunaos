// uui_countdown -- see uui_countdown.h.
#include "ui/uui_countdown.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uui_button.h"
#include "ui/uui_focus.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_progress.h"
#include "ui/uui_route.h"
#include "rt/sys.h"
#include <stdio.h>

enum { ID_PRIMARY = 1, ID_OTHER };

static struct uui_countdown_spec g_spec;
static uui_countdown_fn g_done;
static void *g_ctx;
static unsigned long long g_start_ms;
static int g_left = -1;     // whole seconds shown, -1 before the first tick
static int g_key_took;

static char g_left_text[96];
static struct uui_label g_l_title, g_l_text, g_l_left, g_spacer, g_gap;
static struct uui_progress g_bar;
static struct uui_button g_b_primary, g_b_other;
static struct uui_item g_text_items[4], g_btn_items[3], g_root_items[3];
static struct uui_layout g_text_l, g_btn_l, g_root_l;
static struct uui_focusable g_focusables[2];
static struct uui_focus g_focus;

static unsigned long long now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

static void finish(int code) {
    uui_countdown_fn done = g_done;
    g_done = 0;
    ulogf("countdown: answered %d\n", code);
    if (done) done(g_ctx, code);
}

// Bar and line from the clock; 1 when either changed. The bar runs DOWN,
// the time that is left.
static int update(void) {
    if (g_spec.seconds <= 0) return 0;
    unsigned long long total = (unsigned long long)g_spec.seconds * 1000ull;
    unsigned long long gone = now_ms() - g_start_ms;
    if (gone >= total) { finish(g_spec.timeout_code); return 1; }
    int per_mille = (int)((total - gone) * 1000ull / total);
    int left = (int)((total - gone + 999) / 1000);
    int changed = per_mille != g_bar.value;
    uui_progress_set(&g_bar, per_mille);
    if (left != g_left) {
        g_left = left;
        snprintf(g_left_text, sizeof g_left_text, g_spec.remaining, left);
        uui_label_set_text(&g_l_left, g_left_text);
        changed = 1;
    }
    return changed;
}

static void act_code(int code) {
    if (code == ID_PRIMARY) finish(g_spec.primary_code);
    else if (code == ID_OTHER) finish(g_spec.other_code);
}

static void a_action(struct uapp *a, int code) {
    act_code(code);
    if (!g_done) uapp_quit(a, 0);
}
static void a_widget(struct uapp *a, int id, int reason) {
    if (reason == UUI_REASON_KEY) {
        g_key_took = 1;
        act_code(id);
    }
    if (!g_done) uapp_quit(a, 0);
    else uapp_redraw(a);
}
static void a_key(struct uapp *a, int k, unsigned mods) {
    (void)mods;
    if (k == 0x1B) finish(g_spec.close_code);
    else if ((k == '\n' || k == '\r') && !g_key_took) finish(g_spec.primary_code);
    g_key_took = 0;
    if (!g_done) uapp_quit(a, 0);
}
static int a_close(struct uapp *a) {
    if (g_done) finish(g_spec.close_code);
    uapp_quit(a, 0);
    return 1;
}
static int a_tick(struct uapp *a) {
    int changed = update();
    if (!g_done) uapp_quit(a, 0);
    return changed;
}
static void a_open(struct uapp *a) {
    (void)a;
    g_start_ms = now_ms();
    update();
}
static void a_draw(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    uapp_log_layout(a, "countdown");
}

static void build(void) {
    uint32_t fg = UUI_ROLE(text);
    uui_label_init(&g_l_title, g_spec.title);
    uui_label_init(&g_l_text, g_spec.text);
    uui_label_set_wrap(&g_l_text, 3);
    uui_label_init(&g_l_left, "");
    uui_label_init(&g_spacer, "");
    uui_label_init(&g_gap, "");
    uui_progress_init(&g_bar);
    uui_progress_set(&g_bar, 1000);

    int nt = 0;
    g_text_items[nt++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_l_title, .flags = UUI_FILL_W, .name = "title" };
    g_text_items[nt++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_l_text, .flags = UUI_FILL_W, .name = "text" };
    if (g_spec.seconds > 0) {
        g_text_items[nt++] = (struct uui_item){ .ops = &uui_progress_ops, .widget = &g_bar, .flags = UUI_FILL_W, .name = "bar" };
        g_text_items[nt++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_l_left, .flags = UUI_FILL_W, .name = "left" };
    }
    g_text_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_text_items, .count = nt, .gap = 8 };

    uui_button_init(&g_b_primary, 0, 0, 0, 0, g_spec.primary_label, UUI_ROLE(accent), UUI_ROLE(accent_text), ID_PRIMARY);
    int nb = 0, nf = 0;
    g_btn_items[nb++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_spacer, .flags = UUI_FILL_W };
    if (g_spec.other_label) {
        uui_button_init(&g_b_other, 0, 0, 0, 0, g_spec.other_label, UUI_ROLE(control_bg), fg, ID_OTHER);
        g_btn_items[nb++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_b_other, .id = ID_OTHER, .name = "other" };
    }
    g_btn_items[nb++] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_b_primary, .id = ID_PRIMARY, .name = "primary" };
    g_focusables[nf++] = (struct uui_focusable){ &g_b_primary, &uui_button_ops };
    if (g_spec.other_label) g_focusables[nf++] = (struct uui_focusable){ &g_b_other, &uui_button_ops };
    g_btn_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_btn_items, .count = nb, .gap = 8 };

    g_root_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_text_l, .flags = UUI_FILL_W };
    g_root_items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_gap, .flags = UUI_FILL_H };
    g_root_items[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_btn_l, .flags = UUI_FILL_W };
    g_root_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_root_items, .count = 3, .margin = 16, .gap = 14 };
    uui_focus_init(&g_focus, g_focusables, nf);
    uui_focus_set(&g_focus, 0);
}

// About 46 average characters wide -- the mockup's card -- and as tall as
// the layout needs there.
static void a_size(int *w, int *h) {
    int nw;
    // Here, not in build(): the session's faces exist once uapp_run() has
    // started the font, which is just before it asks for the size.
    g_l_title.font = ugfx_font_session(UGFX_FONT_BOLD);
    *w = ugfx_char_advance('n') * 46;
    uui_layout_natural_size(&g_root_l, &nw, h);
}

void uui_countdown_app_desc(struct uapp_desc *d, const struct uui_countdown_spec *s,
                            uui_countdown_fn done, void *ctx) {
    g_spec = *s;
    g_done = done;
    g_ctx = ctx;
    g_left = -1;
    g_key_took = 0;
    build();
    d->title = g_spec.window_title;
    d->app_id = "countdown";
    d->on_size = a_size;
    d->layout = &g_root_l;
    d->widgets = g_root_items;
    d->widget_count = 3;
    d->focus = &g_focus;
    d->on_open = a_open;
    d->on_draw = a_draw;
    d->on_widget = a_widget;
    d->on_action = a_action;
    d->on_key = a_key;
    d->on_close = a_close;
    d->flags |= UAPP_CENTRED;   // asked with nothing else on screen to stand beside
    if (g_spec.seconds > 0) {
        d->on_tick = a_tick;
        d->tick_ms = 100;
    }
}
