// System Settings: Input > Mouse's Test card -- a folder that opens and
// closes on a double click, under Double-click speed (Windows' Mouse
// Properties has the same folder). It judges the double click by the
// STAGED speed, so a speed can be tried before it is applied.
// Split out as set_sound.c is: a page that needs more than its settings'
// generic rows.
#include "settings/settings_internal.h"
#include "lib/udclick.h"
#include "lib/icon_cache.h"
#include "ui/uui_primitives.h"

struct dclick_test {
    int x, y, w, h;
    int open;
    uint64_t last_ns;   // the last press that did not complete a double; 0 none
};

static struct dclick_test g_test;
static struct uui_setting_row g_row;

static int icon_px(void) { return ugfx_char_h() * 3; }

static struct slot *speed_slot(void) {
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && !strcmp(g_name[g_slot[i].setting], UDCLICK_SETTING))
            return &g_slot[i];
    return 0;
}

static void dt_natural_size(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = icon_px() * 2;
    *oh = icon_px() + ugfx_char_h();
}

static void dt_set_geometry(void *w, int x, int y, int width, int height) {
    struct dclick_test *t = w;
    t->x = x; t->y = y; t->w = width; t->h = height;
}

static void dt_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct dclick_test *t = w;
    *x = t->x; *y = t->y; *ow = t->w; *oh = t->h;
}

// The folder, and when it is open a sheet of paper standing out of it.
static void dt_draw(struct ugfx_surface *s, const void *w) {
    const struct dclick_test *t = w;
    int px = icon_px(), ix = t->x + (t->w - px) / 2, iy = t->y + t->h - px;
    if (t->open) {
        int pw = px * 3 / 5, ph = px * 3 / 4, pxx = ix + (px - pw) / 2, py = iy - px / 4;
        ugfx_fill_rect(s, pxx - 1, py - 1, pw + 2, ph + 2, UTHEME_BORDER);
        ugfx_fill_rect(s, pxx, py, pw, ph, UTHEME_WHITE);
        for (int k = 1; k <= 3; k++)
            ugfx_fill_rect(s, pxx + pw / 6, py + k * ph / 6, pw * 2 / 3, 2, UTHEME_ACCENT);
    }
    const struct uimg *ic = icon_get("folder", px);
    if (ic) ugfx_blit_alpha(s, ix, iy, ic->w, ic->h, ic->px, ic->w);
}

static int dt_hit(const void *w, int cx, int cy) {
    const struct dclick_test *t = w;
    return uui_hit(t->x, t->y, t->w, t->h, cx, cy);
}

// On the PRESS, as the desktop's icons judge a double click.
static int dt_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct dclick_test *t = w;
    if (!dt_hit(t, cx, cy)) return 0;
    struct slot *sl = speed_slot();
    uint64_t now = sys_monotonic_ns();
    uint64_t window = (uint64_t)udclick_word_ms(sl ? staged_value(sl) : 0) * 1000000ull;
    if (t->last_ns && now - t->last_ns < window) {
        t->open = !t->open;
        t->last_ns = 0;   // a third click starts a new pair
        ulogf("settings: dclick test %s\n", t->open ? "opened" : "closed");
    } else {
        t->last_ns = now;
    }
    return 1;
}

static int dt_release(void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return 1;
}

static const struct uui_widget_ops dclick_test_ops = {
    .natural_size = dt_natural_size,
    .set_geometry = dt_set_geometry,
    .bounds = dt_bounds,
    .draw = dt_draw,
    .hit = dt_hit,
    .press = dt_press,
    .release = dt_release,
};

int mouse_emit_after(struct uui_item *out, int n, int i) {
    if (g_slot[i].setting < 0 || strcmp(g_name[g_slot[i].setting], UDCLICK_SETTING)) return n;
    g_row.title = "Test";
    g_row.desc = "Double-click the folder to open or close it, at the speed chosen above.";
    g_row.control = (struct uui_item){ .ops = &dclick_test_ops, .widget = &g_test,
                                       .name = "dclick_test" };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_row,
                                  .flags = UUI_FILL_W, .name = "dclick_card" };
    return n;
}

int mouse_fit(void) { return speed_slot() ? uui_setting_row_fit(&g_row) : 0; }

void mouse_page_opened(void) { g_test.open = 0; g_test.last_ns = 0; }
