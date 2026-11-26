// uui_textbox's eye: a masked field shows its text when the eye inside
// its right edge is clicked, and hides it again (2026-11-26, mockup P1).
// Driven through the ROUTER, with a font loaded so the pixels are real.
//
// What a broken version would still pass, and what stops it: an eye that
// did nothing would leave the field as it was, so the field's own pixels
// must change on the click and come back on the second; a toggle that
// moved the caret too passes the pixels, so the caret must stay put; a
// field that kept showing after new text passes the toggle, so set_text
// and emptying it must hide it; an eye on an EMPTY field would show for
// nothing, so it must be absent until something is typed.
#include <stdint.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/uui_layout.h"
#include "ui/uui_textbox.h"
#include "keyboard.h"
#include "uitest.h"

#define W 300
#define H 60

static struct uui_textbox g_tb;
static struct uui_item g_page[1];
static struct uui_layout g_column;
static struct uui_item g_items[1];
static struct uui_focusable g_ring[1];
static struct uui_focus g_focus;

// The field's pixels left of the eye, as one checksum: what the text shows.
static uint32_t text_sum(struct uitest_win *win) {
    uitest_frame(win);
    uint32_t sum = 0;
    for (int y = g_tb.y + 2; y < g_tb.y + g_tb.h - 2; y++)
        for (int x = g_tb.x + 2; x < g_tb.x + g_tb.w - g_tb.h; x++)
            sum = sum * 31 + uitest_pixel_at(win, x, y);
    return sum;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uitest_begin("textbox_reveal", "uui_textbox's show-password eye");

    uui_textbox_init(&g_tb, "");
    g_tb.masked = 1;
    g_page[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_tb, .flags = UUI_FILL_W, .name = "pw" };
    g_column = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_page, .count = 1 };
    g_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_column, .name = "page" };
    static struct uitest_win win;
    uui_layout_ops.set_geometry(&g_column, 4, 4, W - 8, H - 8);
    uitest_window(&win, g_items, 1, NULL, W, H);
    g_ring[0] = (struct uui_focusable){ &g_tb, &uui_textbox_ops };
    uui_focus_init(&g_focus, g_ring, 1);
    uitest_set_focus(&win, &g_focus);

    // The eye's own pixels need the theme's icons, which the host has not:
    // login_test samples them on the real desktop.
    uitest_click(&win, "pw");
    uitest_click_in(&win, "pw", -(g_tb.h / 2), g_tb.h / 2);
    utest_check(!g_tb.revealed, "an empty password field has no eye: a click there reveals nothing");

    for (const char *s = "sauna7"; *s; s++) uitest_key(&win, *s, 0);
    utest_check(!g_tb.revealed && !strcmp(uui_textbox_text(&g_tb), "sauna7"), "typed, hidden");
    uint32_t stars = text_sum(&win);
    int caret = g_tb.ed.cursor;

    uitest_click_in(&win, "pw", -(g_tb.h / 2), g_tb.h / 2);
    utest_check(g_tb.revealed, "a click on the eye shows the password");
    utest_check(g_tb.ed.cursor == caret, "...and leaves the caret where it was");
    uint32_t shown = text_sum(&win);
    utest_check(shown != stars, "...and what the field draws changes (the letters, not stars)");

    uitest_click_in(&win, "pw", -(g_tb.h / 2), g_tb.h / 2);
    utest_check(!g_tb.revealed && text_sum(&win) == stars, "a second click hides it again, the stars back");

    uitest_click_in(&win, "pw", -(g_tb.h / 2), g_tb.h / 2);
    uui_textbox_set_text(&g_tb, "");
    utest_check(!g_tb.revealed, "new text (set_text, as after Enter or a wrong password) starts hidden");

    for (const char *s = "ab"; *s; s++) uitest_key(&win, *s, 0);
    uitest_click_in(&win, "pw", -(g_tb.h / 2), g_tb.h / 2);
    uitest_key(&win, '\b', 0);
    uitest_key(&win, '\b', 0);
    utest_check(!g_tb.revealed && !g_tb.len, "emptied by Backspace: hidden, for the next password");

    return uitest_end();
}
