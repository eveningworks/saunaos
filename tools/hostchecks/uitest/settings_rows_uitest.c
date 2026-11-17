// System Settings' rows as widgets: a page shaped like set_page.c's -- a
// uui_scrollview over a column of uui_setting_row cards, each carrying a
// switch, a segmented control, a dropdown or a radio list -- driven by
// NAME through the router. What tools/tests/gui/settings_test.py asserts
// about staging, the registry and the bytes on disk is the APP's and
// stays there (stage 2 is an offscreen uapp); this is the part that is
// each control's own: a click changes the value it should and paints it
// selected, a press dragged off commits nothing, a popup takes the keys,
// and the scroll view routes a click only to what it SHOWS.
//
// "Selected" is asserted in pixels as well as state: the accent fill is
// what a person reads (docs/gui-guidelines.md), and a widget whose value
// moved while its paint did not passes every state check.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uitest.h"
#include "keyboard.h"
#include "ui/utheme.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_radio_list.h"
#include "ui/uui_scrollview.h"
#include "ui/uui_segmented.h"
#include "ui/uui_setting_row.h"
#include "ui/uui_switch.h"

#define W 560
#define H 360
#define FILLERS 12   // rows past the four, so the page overflows

static const char *const CLOCK[] = { "12-hour", "24-hour" };
static const char *const THEME[] = { "Light", "Dark", "High contrast" };
static const char *const POINTER[] = { "Small", "Medium", "Large" };

static struct uui_switch g_anim, g_fill[FILLERS];
static struct uui_segmented g_clock;
static struct uui_dropdown g_theme;
static struct uui_radio_list g_pointer;
static struct uui_setting_row g_rows[4 + FILLERS];
static char g_fill_title[FILLERS][32], g_fill_name[FILLERS][16];
static struct uui_item g_page[4 + FILLERS];
static struct uui_layout g_column;
static struct uui_scrollview g_scroll;
static struct uui_item g_items[1];
static struct uui_focusable g_ring[4 + FILLERS];
static struct uui_focus g_focus;

// How close a pixel is to a colour, summed over the channels.
static int dist(uint32_t a, uint32_t b) {
    int d = 0;
    for (int s = 0; s <= 16; s += 8) d += abs((int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF));
    return d;
}

// Is the rect FILLED with the accent -- a tenth of it or more? A count,
// not "any accent pixel": the focus ring is accent too, and a click
// focuses what it lands on, so a ring round an Off switch must not read
// as the switch being on.
static int accent_in(struct uitest_win *win, int x, int y, int w, int h) {
    int hits = 0;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            hits += dist(uitest_pixel_at(win, xx, yy), UTHEME_ACCENT) < 24;
    return w > 0 && h > 0 && hits * 10 >= w * h;
}

static void row(int i, const char *title, const char *desc, struct uui_item control) {
    uui_setting_row_init(&g_rows[i], title, desc, control);
    g_rows[i].stacked = control.ops == &uui_radio_list_ops;   // set_page.c's rule
    g_page[i] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_rows[i], .flags = UUI_FILL_W };
    g_ring[i] = (struct uui_focusable){ control.widget, control.ops };
}

static void build(void) {
    uui_switch_init(&g_anim, 0);
    uui_segmented_init(&g_clock, CLOCK, 2, 0);
    uui_dropdown_init(&g_theme, 0, 0, 0, 0, THEME, 3);
    g_pointer.options = POINTER;
    g_pointer.count = 3;
    g_pointer.cols = 1;
    g_pointer.selected = 0;
    row(0, "Animations", "Windows and menus open with a short motion",
        (struct uui_item){ .ops = &uui_switch_ops, .widget = &g_anim, .id = 10, .name = "anim" });
    row(1, "Clock", "How the taskbar shows the time",
        (struct uui_item){ .ops = &uui_segmented_ops, .widget = &g_clock, .id = 11, .name = "clock" });
    row(2, "Theme", "The colours every window draws with",
        (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_theme, .id = 12, .name = "theme" });
    row(3, "Pointer size", NULL,
        (struct uui_item){ .ops = &uui_radio_list_ops, .widget = &g_pointer, .id = 13, .name = "pointer" });
    for (int i = 0; i < FILLERS; i++) {
        snprintf(g_fill_title[i], sizeof g_fill_title[i], "Filler setting %02d", i);
        snprintf(g_fill_name[i], sizeof g_fill_name[i], "fill%02d", i);
        uui_switch_init(&g_fill[i], 0);
        row(4 + i, g_fill_title[i], "A row to scroll past",
            (struct uui_item){ .ops = &uui_switch_ops, .widget = &g_fill[i], .id = 20 + i, .name = g_fill_name[i] });
    }
    g_column = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_page, .count = 4 + FILLERS };
    uui_scrollview_init(&g_scroll, &g_column);
    g_items[0] = (struct uui_item){ .ops = &uui_scrollview_ops, .widget = &g_scroll, .id = 1, .name = "page",
                                    .flags = UUI_FILL_W | UUI_FILL_H };
    uui_focus_init(&g_focus, g_ring, 4 + FILLERS);
}

// The window: the scroll view placed over all of it, as the app's body
// layout leaves it the page's whole area.
static void open_page(struct uitest_win *win) {
    uui_scrollview_ops.set_geometry(&g_scroll, 0, 0, W, H);
    uitest_window(win, g_items, 1, NULL, W, H);
    uitest_set_focus(win, &g_focus);
}

static void relayout(struct uitest_win *win) {
    uui_scrollview_ops.set_geometry(&g_scroll, 0, 0, W, H);
    uitest_settle(win);
}

int main(void) {
    uitest_begin("settings_rows", "System Settings' rows: each control, by name, in a scrolling page");
    build();
    struct uitest_win win;
    open_page(&win);
    int x, y, w, h;
    char what[160];

    // --- a switch flips on a click, says so, and paints its track ---------------
    uitest_rect(&win, "anim", &x, &y, &w, &h);
    utest_check(!accent_in(&win, x, y, w, h), "an Off switch draws no accent");
    int id = uitest_click(&win, "anim");
    uitest_settle(&win);
    uitest_check_int("a click turns the switch on", g_anim.on, 1);
    uitest_check_int("...and the router names it to the app", id, 10);
    utest_check(accent_in(&win, x, y, w, h), "...and its track is drawn in the accent");
    uitest_click(&win, "anim");
    uitest_settle(&win);
    uitest_check_int("a second click turns it off", g_anim.on, 0);
    utest_check(!accent_in(&win, x, y, w, h), "...and the accent is gone");

    // --- a segmented control selects the segment under the click ------------------
    uitest_rect(&win, "clock", &x, &y, &w, &h);
    uitest_click_in(&win, "clock", w * 3 / 4, h / 2);
    uitest_settle(&win);
    uitest_check_int("a click on the second segment selects it", g_clock.selected, 1);
    utest_check(accent_in(&win, x + w / 2 + 2, y + 2, w / 2 - 4, h - 4), "...which is filled in the accent");
    utest_check(!accent_in(&win, x + 2, y + 2, w / 2 - 4, h - 4), "...and the first is not");

    // --- a dropdown opens a popup that takes the keys, and Esc leaves it alone --------
    uitest_click(&win, "theme");
    uitest_settle(&win);
    utest_check(uui_router_overlay_open(&win.router), "a click opens the dropdown's list");
    uitest_key(&win, KEY_ARROW_DOWN, 0);
    uitest_key(&win, '\n', 0);
    uitest_settle(&win);
    uitest_check_int("Down then Enter in the open list picks the next choice", uui_dropdown_selected(&g_theme), 1);
    utest_check(!uui_router_overlay_open(&win.router), "...and closes it");
    uitest_click(&win, "theme");
    uitest_key(&win, KEY_ARROW_DOWN, 0);
    uitest_key(&win, 27, 0);   // Esc
    uitest_settle(&win);
    uitest_check_int("Esc in the open list keeps the choice it had", uui_dropdown_selected(&g_theme), 1);
    utest_check(!uui_router_overlay_open(&win.router), "...and closes it");

    // --- a radio list commits on RELEASE, and a press dragged off commits nothing ------
    uitest_rect(&win, "pointer", &x, &y, &w, &h);
    // Rows from the list's own height: row_h 0 means the widget's default.
    int row_h = h / g_pointer.count;
    int third = y + row_h * 2 + row_h / 2;
    uitest_click_at(&win, x + 8, third);
    uitest_check_int("a click on the third option selects it", g_pointer.selected, 2);
    int second = y + row_h + row_h / 2;
    uitest_press_at(&win, x + 8, second);
    uitest_motion_at(&win, x + w + 200, second, 1);
    uitest_release_at(&win, x + w + 200, second);
    uitest_check_int("a press on the second dragged off the list commits nothing", g_pointer.selected, 2);

    // --- the page overflows, and the scroll view routes only what it SHOWS ----------
    utest_check(uui_scrollview_scrollable(&g_scroll), "the page has more rows than fit, so it scrolls");
    const char *last = g_fill_name[FILLERS - 1];
    uitest_rect(&win, last, &x, &y, &w, &h);
    snprintf(what, sizeof what, "the last row's switch starts below the view (y %d of %d)", y, H);
    utest_check(y >= H, what);
    for (int i = 0; i < 20 && uui_scrollview_offset(&g_scroll) < uui_scrollview_max_offset(&g_scroll); i++)
        uitest_wheel(&win, "page", -3);
    uitest_settle(&win);
    uitest_check_int("the wheel down reaches the page's end", uui_scrollview_offset(&g_scroll),
                     uui_scrollview_max_offset(&g_scroll));
    uitest_rect(&win, last, &x, &y, &w, &h);
    snprintf(what, sizeof what, "...where the last switch is inside the view (%d..%d of %d)", y, y + h, H);
    utest_check(y >= 0 && y + h <= H, what);
    uitest_click(&win, last);
    uitest_check_int("...and a click there turns it on", g_fill[FILLERS - 1].on, 1);

    // A switch STRADDLING the view's foot: its shown half takes a click, its
    // clipped half does not -- the clip is the viewport's edge.
    uitest_wheel(&win, "page", 30);   // back to the top
    uitest_settle(&win);
    int pick = -1;
    for (int i = 0; i < FILLERS && pick < 0; i++) {
        uitest_rect(&win, g_fill_name[i], &x, &y, &w, &h);
        if (y < H && y + h > H) pick = i;
    }
    if (pick < 0) {
        // None happens to straddle at offset 0: scroll until one does.
        for (int off = 1; off < uui_scrollview_max_offset(&g_scroll) && pick < 0; off++) {
            uui_scrollview_set_offset(&g_scroll, off);
            relayout(&win);
            for (int i = 0; i < FILLERS && pick < 0; i++) {
                uitest_rect(&win, g_fill_name[i], &x, &y, &w, &h);
                if (y < H - 4 && y + h > H + 4) pick = i;
            }
        }
    }
    utest_check(pick >= 0, "a filler switch straddles the view's foot");
    if (pick >= 0) {
        uitest_rect(&win, g_fill_name[pick], &x, &y, &w, &h);
        int before = g_fill[pick].on;
        uitest_click_at(&win, x + 4, H + 2);   // below the view: clipped away
        uitest_check_int("a click on the clipped half of a straddling switch does nothing", g_fill[pick].on, before);
        uitest_click_at(&win, x + 4, H - 3);
        uitest_check_int("...and one on its shown half flips it", g_fill[pick].on, !before);
    }

    // --- the keyboard: Tab walks the controls, Space acts on the focused one ----------
    uui_focus_set(&g_focus, 0);
    int was = g_anim.on;
    uitest_key(&win, ' ', 0);
    uitest_check_int("Space toggles the focused switch", g_anim.on, !was);
    uitest_key(&win, '\t', 0);
    uitest_check_int("Tab moves focus to the next control", g_focus.current, 1);
    uitest_key(&win, KEY_ARROW_LEFT, 0);
    uitest_check_int("Left on the focused segmented control selects the first segment", g_clock.selected, 0);

    uitest_keep(&win, "page");
    uitest_window_free(&win);
    return uitest_end();
}
