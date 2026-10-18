// uui_droprows: fixed rows, the person's own rows newest first, and an
// action row -- Disk Mark's size and location dropdowns.
//
// THE LOAD-BEARING CHECKS are the ones a row-index bookkeeping slip
// passes elsewhere: re-adding an own row MOVES it rather than
// duplicating it, the oldest is the one dropped, and the box goes back
// to the value in force after the action row -- not to the row that
// now sits at the old index.
#include <string.h>
#include <stdio.h>
#include "ui/uui_droprows.h"
#include "lib/utest.h"

static struct uui_dropdown dd;
static struct uui_droprows r;

static int shows(const char *label) {
    int i = uui_dropdown_selected(&dd);
    return i >= 0 && i < r.rows && !strcmp(r.items[i], label);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    utest_begin("droprows_test", "uui_droprows", UTEST_VERDICT_FILE);

    static const char *vols[] = { "/ on ata0p3", "/mnt/data on ata1p1" };
    uui_dropdown_init(&dd, 0, 0, 0, 0, NULL, 0);
    uui_droprows_init(&r, &dd, 3, "Choose folder...");
    uui_droprows_set_fixed(&r, vols, 2);
    utest_check(r.rows == 3 && !strcmp(r.items[2], "Choose folder..."),
                "the fixed rows, then the action row");

    uui_droprows_select(&r, UUI_DROPROW_FIXED, 1);
    utest_check(shows("/mnt/data on ata1p1"), "a fixed row is selected by index");

    uui_droprows_add_own(&r, "/home/a");
    uui_droprows_add_own(&r, "/home/b");
    utest_check(r.rows == 5 && !strcmp(r.items[2], "/home/b") && !strcmp(r.items[3], "/home/a"),
                "own rows sit between, newest first");
    utest_check(shows("/home/b"), "adding one selects it");

    uui_droprows_add_own(&r, "/home/a");
    utest_check(r.nown == 2 && !strcmp(uui_droprows_own(&r, 0), "/home/a") &&
                !strcmp(uui_droprows_own(&r, 1), "/home/b"),
                "adding one already there moves it up, no duplicate");

    uui_droprows_add_own(&r, "/home/c");
    uui_droprows_add_own(&r, "/home/d");
    utest_check(r.nown == 3 && uui_droprows_find_own(&r, "/home/b") < 0 &&
                !strcmp(uui_droprows_own(&r, 0), "/home/d"),
                "past the limit the OLDEST is dropped");

    int idx, kind = uui_droprows_kind(&r, r.rows - 1, &idx);
    utest_check(kind == UUI_DROPROW_MORE, "the last row is the action row");
    kind = uui_droprows_kind(&r, 3, &idx);
    utest_check(kind == UUI_DROPROW_OWN && idx == 1, "row 3 is the second own row");

    // The action row picked, then put back: the value in force shows.
    uui_droprows_select(&r, UUI_DROPROW_OWN, 1);
    uui_dropdown_set_selected(&dd, r.rows - 1);
    uui_droprows_reselect(&r);
    utest_check(shows("/home/c"), "reselect shows the value in force after the action row");

    uui_droprows_select(&r, UUI_DROPROW_MORE, 0);
    utest_check(shows("/home/c"), "the action row is never selected as a value");

    // Removing a row ABOVE the selection keeps the same folder selected.
    uui_droprows_remove_own(&r, 0);
    utest_check(r.nown == 2 && shows("/home/c"), "removing a row above keeps the selection on its value");

    // The fixed rows replaced (a volume mounted) keep an own selection.
    static const char *three[] = { "/ on ata0p3", "/mnt/data on ata1p1", "/mnt/usb on usb0p1" };
    uui_droprows_set_fixed(&r, three, 3);
    utest_check(shows("/home/c"), "new fixed rows keep the own row selected");

    char longp[UUI_DROPROWS_LABEL + 8];
    memset(longp, 'x', sizeof longp - 1);
    longp[0] = '/';
    longp[sizeof longp - 1] = 0;
    utest_check(!uui_droprows_add_own(&r, longp) && r.nown == 2,
                "a label too long to keep whole is refused, not truncated");

    // One own row: Disk Mark's custom size, replaced each time.
    static struct uui_dropdown sd;
    static struct uui_droprows sr;
    static const char *sizes[] = { "16 MiB", "64 MiB" };
    uui_dropdown_init(&sd, 0, 0, 0, 0, NULL, 0);
    uui_droprows_init(&sr, &sd, 1, "Custom...");
    uui_droprows_set_fixed(&sr, sizes, 2);
    uui_droprows_add_own(&sr, "3 GiB");
    uui_droprows_add_own(&sr, "1536 MiB");
    utest_check(sr.rows == 4 && !strcmp(sr.items[2], "1536 MiB"), "one own row is replaced, not added to");

    return utest_end();
}
