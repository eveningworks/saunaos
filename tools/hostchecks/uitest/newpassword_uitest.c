// uui_newpassword: a new password asked twice, driven through the ROUTER
// -- clicks focus each field, keys go where the focus is -- with a font
// loaded so the geometry and the message's pixels are real.
//
// What a broken version would still pass, and what stops it: a widget
// that compared nothing would say MATCH as soon as both had text, so
// "differ" must be seen with both typed; one that showed the message
// always passes "it says so", so the line must be ABSENT once they agree;
// a field that took no keys would pass "empty", so the text must come
// back exactly; fields that were not children would not take a click.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_layout.h"
#include "ui/uui_newpassword.h"
#include "keyboard.h"
#include "uitest.h"

#define W 420
#define H 140

static struct uui_newpassword g_np;
static struct uui_item g_page[1];
static struct uui_layout g_column;
static struct uui_item g_items[1];
static struct uui_focusable g_ring[2];
static struct uui_focus g_focus;

static void type(struct uitest_win *win, const char *s) {
    for (; *s; s++) uitest_key(win, *s, 0);
}

// Pixels of the error colour on the message line, under the fields.
static int error_pixels(struct uitest_win *win) {
    uitest_frame(win);
    uint32_t red = utheme_severity(UTHEME_SEV_ERROR);
    int y0 = g_np.confirm.y + g_np.confirm.h, n = 0;
    for (int y = y0; y < g_np.y + g_np.h + 2 && y < H; y++)
        for (int x = g_np.x; x < g_np.x + g_np.w && x < W; x++)
            n += uitest_pixel_at(win, x, y) == red;
    return n;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uitest_begin("newpassword", "uui_newpassword");

    uui_newpassword_init(&g_np, "New password", "Confirm");
    g_page[0] = (struct uui_item){ .ops = &uui_newpassword_ops, .widget = &g_np, .flags = UUI_FILL_W,
                                   .name = "newpw" };
    g_column = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_page, .count = 1 };
    g_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_column, .name = "page" };
    static struct uitest_win win;
    uui_layout_ops.set_geometry(&g_column, 4, 4, W - 8, H - 8);
    uitest_window(&win, g_items, 1, NULL, W, H);
    uui_focus_init(&g_focus, g_ring, uui_newpassword_focusables(&g_np, g_ring));
    uitest_set_focus(&win, &g_focus);

    utest_check(uui_newpassword_state(&g_np) == UUI_NEWPW_EMPTY && !uui_newpassword_text(&g_np),
                "it opens empty, with no password to give");
    utest_check(g_np.field.w > 0 && g_np.confirm.y > g_np.field.y, "two fields, one under the other");

    uitest_click(&win, "newpw_field");
    type(&win, "sauna");
    utest_check(uui_newpassword_state(&g_np) == UUI_NEWPW_PARTIAL, "the first typed: not yet confirmed");
    utest_check(strcmp(uui_textbox_text(&g_np.field), "sauna") == 0 && !uui_textbox_text(&g_np.confirm)[0],
                "a click on the first field sends the keys there, and only there");

    uitest_click(&win, "newpw_confirm");
    type(&win, "saun");
    utest_check(uui_newpassword_state(&g_np) == UUI_NEWPW_DIFFER && !uui_newpassword_text(&g_np),
                "both typed, different: DIFFER, and no password given");
    int red = error_pixels(&win);
    utest_checkf(red > 20, "...and the line under them says so, in the error colour (%d px)", red);

    type(&win, "a");
    const char *t = uui_newpassword_text(&g_np);
    utest_check(uui_newpassword_state(&g_np) == UUI_NEWPW_MATCH && t && !strcmp(t, "sauna"),
                "the same in both: MATCH, and the password is the one typed");
    red = error_pixels(&win);
    utest_checkf(red == 0, "...and the message is gone (%d px)", red);

    uui_newpassword_clear(&g_np);
    utest_check(uui_newpassword_state(&g_np) == UUI_NEWPW_EMPTY, "clear empties both");

    uitest_window_free(&win);
    return uitest_end();
}
