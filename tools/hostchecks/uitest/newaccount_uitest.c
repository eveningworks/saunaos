// uui_newaccount: the Add account form, driven through the ROUTER --
// clicks focus each field, keys go where the focus is -- with a font
// loaded so the geometry and the notes' pixels are real, and an
// /etc/passwd fixture so "taken" is a real lookup.
//
// What a broken version would still pass, and what stops it: a name that
// never followed the full name passes "it can be typed", so the folded
// first word must appear by itself; one that ALWAYS followed would
// overwrite the person's own edit, so an edited name must survive a
// change to the full name; a check that compared nothing would say READY
// for a taken name, so the fixture's account must read TAKEN, in the
// error colour; a password pair with its own label column would sit
// left of the form's fields, so the fields' x must agree.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_layout.h"
#include "ui/uui_newaccount.h"
#include "keyboard.h"
#include "uitest.h"

#define W 520
#define H 300

static struct uui_newaccount g_na;
static struct uui_item g_page[1];
static struct uui_layout g_column;
static struct uui_item g_items[1];
static struct uui_focusable g_ring[5];
static struct uui_focus g_focus;

// Each key as the app sees it: the router takes it, then on_widget's
// call to uui_newaccount_changed().
static void type(struct uitest_win *win, const char *s) {
    for (; *s; s++) {
        uitest_key(win, (unsigned char)*s, 0);
        uui_newaccount_changed(&g_na);
    }
}

static void erase(struct uitest_win *win, int n) {
    while (n-- > 0) {
        uitest_key(win, '\b', 0);
        uui_newaccount_changed(&g_na);
    }
}

// Pixels of the error colour on the account name's note line.
static int error_pixels(struct uitest_win *win) {
    uitest_frame(win);
    uint32_t red = utheme_severity(UTHEME_SEV_ERROR);
    int y0 = g_na.name.y + g_na.name.h, n = 0;
    for (int y = y0; y < y0 + ugfx_char_h() + 6 && y < H; y++)
        for (int x = g_na.x; x < g_na.x + g_na.w && x < W; x++)
            n += uitest_pixel_at(win, x, y) == red;
    return n;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uitest_begin("newaccount", "uui_newaccount");
    uitest_file("/etc/passwd", "root:x:0:0:root:/:/bin/tosh\n"
                               "user:x:1000:1000:User:/home/user:/bin/tosh\n"
                               "anna:x:1001:1001:Anna Virtanen:/home/anna:/bin/tosh\n");
    uitest_file("/etc/group", "root:x:0:\nwheel:x:10:user\nuser:x:1000:\nanna:x:1001:\n");

    uui_newaccount_init(&g_na);
    g_page[0] = (struct uui_item){ .ops = &uui_newaccount_ops, .widget = &g_na, .flags = UUI_FILL_W,
                                   .name = "newacct" };
    g_column = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_page, .count = 1 };
    g_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_column, .name = "page" };
    static struct uitest_win win;
    uui_layout_ops.set_geometry(&g_column, 4, 4, W - 8, H - 8);
    uitest_window(&win, g_items, 1, NULL, W, H);
    int nring = uui_newaccount_focusables(&g_na, g_ring);
    uui_focus_init(&g_focus, g_ring, nring);
    uitest_set_focus(&win, &g_focus);

    utest_check(nring == 5, "five focusables: full name, account name, type, password, confirm");
    utest_check(uui_newaccount_state(&g_na) == UUI_NEWACCT_NO_NAME, "it opens with no name");
    utest_checkf(g_na.pw.field.x == g_na.full.x && g_na.name.x == g_na.full.x,
                 "every field starts at one x, the password's too (%d, %d, %d)", g_na.full.x, g_na.name.x,
                 g_na.pw.field.x);

    uitest_click(&win, "newacct_full");
    uui_textbox_insert(&g_na.full, "\xc3\x85" "ke \xc3\x96" "berg");   // "Åke Öberg", as an IME commits it
    uui_newaccount_changed(&g_na);
    utest_checkf(!strcmp(uui_newaccount_name(&g_na), "ake"),
                 "the account name follows the full name's first word, folded to ASCII (\"%s\")",
                 uui_newaccount_name(&g_na));
    utest_check(uui_newaccount_state(&g_na) == UUI_NEWACCT_READY && !strcmp(uui_newaccount_password(&g_na), ""),
                "a free name and no password: READY, with \"\" for the password");

    uitest_click(&win, "newacct_name");
    type(&win, "o");
    uitest_click(&win, "newacct_full");
    type(&win, "x");
    utest_checkf(!strcmp(uui_newaccount_name(&g_na), "akeo"),
                 "an account name the person edited stays as they left it (\"%s\")", uui_newaccount_name(&g_na));

    uitest_click(&win, "newacct_name");
    erase(&win, 4);
    type(&win, "anna");
    utest_checkf(!strcmp(uui_newaccount_name(&g_na), "anna"),
                 "an emptied name stays empty for the person to type (\"%s\")", uui_newaccount_name(&g_na));
    utest_check(uui_newaccount_state(&g_na) == UUI_NEWACCT_TAKEN, "a name /etc/passwd has: TAKEN");
    int red = error_pixels(&win);
    utest_checkf(red > 20, "...and the note under it says so, in the error colour (%d px)", red);

    erase(&win, 4);
    type(&win, "Anna");
    utest_check(uui_newaccount_state(&g_na) == UUI_NEWACCT_BAD_NAME, "upper case: not a name an account may have");

    erase(&win, 4);
    type(&win, "aino");
    red = error_pixels(&win);
    utest_checkf(uui_newaccount_state(&g_na) == UUI_NEWACCT_READY && red == 0,
                 "a free name: READY, and no error under it (%d px)", red);

    int tx, ty, tw, th;
    uitest_rect(&win, "newacct_type", &tx, &ty, &tw, &th);
    uitest_click_at(&win, tx + tw * 3 / 4, ty + th / 2);
    utest_check(uui_newaccount_admin(&g_na), "a click on Administrator makes it one");

    uitest_click(&win, "newpw_field");
    type(&win, "sauna");
    utest_check(uui_newaccount_state(&g_na) == UUI_NEWACCT_PW_UNSURE && !uui_newaccount_password(&g_na),
                "a password typed once: not ready, and no password given");
    uitest_click(&win, "newpw_confirm");
    type(&win, "sauna");
    const char *pw = uui_newaccount_password(&g_na);
    utest_check(uui_newaccount_state(&g_na) == UUI_NEWACCT_READY && pw && !strcmp(pw, "sauna"),
                "the same twice: READY, and the password is the one typed");

    uui_newaccount_clear(&g_na);
    utest_check(uui_newaccount_state(&g_na) == UUI_NEWACCT_NO_NAME && !uui_newaccount_admin(&g_na),
                "clear empties it and chooses Standard again");
    uitest_click(&win, "newacct_full");
    type(&win, "Mikko");
    utest_check(!strcmp(uui_newaccount_name(&g_na), "mikko"), "after a clear, the name follows again");

    uitest_window_free(&win);
    return uitest_end();
}
