// uui_sizefield: a number and a MiB/GiB unit, driven through its OPS
// TABLE -- what the router calls -- with a font loaded so the geometry
// is real (focusring_test.c says why that needs ring 3).
//
// THE LOAD-BEARING PAIR is the two unit changes, which must DIFFER:
// clicking GiB converts what is shown (1536 MiB -> 2 GiB), typing `g`
// after "3" means 3 GiB. A widget that did either for both passes half
// of these and turns a typed "3g" into 1 GiB, or a click into 1536 GiB.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui_sizefield.h"
#include "keyboard.h"
#include "lib/utest.h"

static void ok(const char *name, int cond, uint64_t got) {
    char d[64];
    snprintf(d, sizeof d, "got %llu MiB", (unsigned long long)got);
    utest_check_detail(cond, name, d);
}

// A click in the middle of unit segment `i` (0 MiB, 1 GiB).
static void click_unit(struct uui_sizefield *f, int i) {
    int x = f->unit.x + f->unit.w * (2 * i + 1) / 4, y = f->unit.y + f->unit.h / 2;
    uui_sizefield_ops.press(f, x, y, 0);
    uui_sizefield_ops.release(f, x, y);
}

static void type(struct uui_sizefield *f, const char *s) {
    for (; *s; s++) uui_sizefield_ops.key(f, *s, 0);
}

static void select_all_and_type(struct uui_sizefield *f, const char *s) {
    int x = f->num.x + 4, y = f->num.y + f->num.h / 2;
    uui_sizefield_ops.press(f, x, y, 0);
    uui_sizefield_ops.release(f, x, y);
    uui_sizefield_ops.key(f, 0x01, 0);   // Ctrl+A
    type(f, s);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ugfx_font_init();
    utest_begin("sizefield_test", "uui_sizefield", UTEST_VERDICT_FILE);

    static struct uui_sizefield f;
    uui_sizefield_init(&f, 1024, 1, 9000);
    int w, h;
    uui_sizefield_ops.natural_size(&f, &w, &h);
    uui_sizefield_ops.set_geometry(&f, 10, 10, w, h);
    utest_check(w > 0 && h > 0, "it has a size once the font is loaded");
    ok("a whole GiB is shown in GiB", f.unit.selected == 1 && uui_spinbox_value(&f.num) == 1,
       uui_sizefield_mib(&f));

    uui_sizefield_set_mib(&f, 1536);
    ok("1536 MiB is shown in MiB", f.unit.selected == 0 && uui_sizefield_mib(&f) == 1536,
       uui_sizefield_mib(&f));

    click_unit(&f, 1);
    ok("clicking GiB CONVERTS: 1536 MiB rounds to 2 GiB", f.unit.selected == 1 &&
       uui_sizefield_mib(&f) == 2048, uui_sizefield_mib(&f));
    click_unit(&f, 0);
    ok("and back: 2048 MiB", f.unit.selected == 0 && uui_sizefield_mib(&f) == 2048,
       uui_sizefield_mib(&f));

    select_all_and_type(&f, "3g");
    uui_sizefield_commit(&f);
    ok("typing 3 then g REINTERPRETS: 3 GiB", f.unit.selected == 1 &&
       uui_sizefield_mib(&f) == 3072, uui_sizefield_mib(&f));

    select_all_and_type(&f, "700m");
    uui_sizefield_commit(&f);
    ok("typing 700 then m: 700 MiB", f.unit.selected == 0 && uui_sizefield_mib(&f) == 700,
       uui_sizefield_mib(&f));

    // The range is the volume's room: 9000 MiB is 8 whole GiB.
    select_all_and_type(&f, "9g");
    uui_sizefield_commit(&f);
    ok("a typed size over the range is REFUSED, not clamped", uui_sizefield_mib(&f) == 700,
       uui_sizefield_mib(&f));
    uui_sizefield_ops.key(&f, KEY_ARROW_UP, 0);
    ok("Up steps one unit", uui_sizefield_mib(&f) == 701, uui_sizefield_mib(&f));

    uui_sizefield_set_range(&f, 1, 512);
    ok("a narrower range clamps the value into it", uui_sizefield_mib(&f) == 512,
       uui_sizefield_mib(&f));
    click_unit(&f, 1);
    ok("GiB is refused when the range holds less than one", f.unit.selected == 0 &&
       uui_sizefield_mib(&f) == 512, uui_sizefield_mib(&f));

    uui_sizefield_init(&f, 2000000, 1, 2000000);
    ok("past a million MiB it is shown in GiB", f.unit.selected == 1 &&
       uui_sizefield_mib(&f) <= 2000000 && uui_sizefield_mib(&f) >= 1999000,
       uui_sizefield_mib(&f));

    return utest_end();
}
