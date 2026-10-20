// uui_table's chosen and moved columns -- what Task Manager's header menu
// and column drag are built on. Logic through the widget's own ops table,
// the way the router calls it, so a drag that only works when an app
// forwards it by hand would fail here.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "ui/ugfx.h"
#include "ui/uui_table.h"
#include "ui/uui_ctxmenu.h"
#include "ui/uui_sbar.h"
#include "lib/utest.h"
#include "win_proto.h"   // WIN_CURSOR_*

static void ok(const char *name, int cond, const char *detail) {
    utest_check_detail(cond, name, detail);
}
static void oks(const char *name, const char *got, const char *want) {
    char d[200];
    snprintf(d, sizeof d, "got '%s', want '%s'", got, want);
    ok(name, strcmp(got, want) == 0, d);
}

static const struct uui_table_column COLS[] = {
    { "Name", 0, UUI_TALIGN_LEFT, "name", 0 },
    { "PID", 6, UUI_TALIGN_RIGHT, "pid", 0 },
    { "CPU", 6, UUI_TALIGN_RIGHT, "cpu", 0 },
    { "Read", 8, UUI_TALIGN_RIGHT, "read", 0 },
    { "Extra", 8, UUI_TALIGN_RIGHT, "extra", UUI_TCOL_HIDDEN },
};
#define NC ((int)(sizeof COLS / sizeof COLS[0]))

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    snprintf(out, cap, "%d.%d", row, col);
}
static int cmp(void *ctx, int a, int b, int col) { (void)ctx; (void)col; return a - b; }

static const char *layout(struct uui_table *t) {
    static char buf[200];
    if (!uui_table_columns_save(t, buf, sizeof buf)) strcpy(buf, "(refused)");
    return buf;
}

// The header's middle over app column `col`, at its settled place.
static void header_at(struct uui_table *t, int col, int *x, int *y) {
    int cx, cw;
    uui_table_column_rect(t, col, &cx, &cw);
    *x = cx + cw / 2;
    *y = t->y + uui_table_header_h(t) / 2;
}

// Press on a header, move to `to_x` in small steps, release: what the
// router does with a real drag.
static void drag(struct uui_table *t, int col, int to_x) {
    int x, y;
    header_at(t, col, &x, &y);
    uui_table_ops.press(t, x, y, 0);
    int step = to_x > x ? 7 : -7;
    for (int px = x + step; step > 0 ? px < to_x : px > to_x; px += step)
        uui_table_ops.motion(t, px, y, 1);
    uui_table_ops.motion(t, to_x, y, 1);
    uui_table_ops.release(t, to_x, y);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ugfx_font_init();
    utest_begin("table_columns_test", "uui_table: choosing and moving columns", UTEST_VERDICT_FILE);
    ok("the font loaded, so widths are real", ugfx_char_h() > 0, "char_h is 0");

    static struct uui_table t;
    uui_table_init(&t, 0, 0, 600, 300, COLS, NC, cell, 0);
    uui_table_set_compare(&t, cmp);
    uui_table_set_rows(&t, 5);

    // --- defaults, off ---
    oks("a declared-hidden column starts hidden", layout(&t), "name,pid,cpu,read,-extra");
    struct uui_ctxmenu_src src;
    ok("no menu while the mode is off", !uui_table_ops.context_menu(&t, 10, 2, &src), "");
    int x, y;
    header_at(&t, 2, &x, &y);
    uui_table_ops.press(&t, x, y, 0);
    ok("with the mode off a header PRESS still sorts at once", t.sort_col == 2, "");
    uui_table_ops.release(&t, x, y);

    uui_table_set_columns_mode(&t, UUI_TABLE_COLS_COLUMN);
    uui_table_take_layout_changed(&t);

    // --- widths ---
    int ex, ew, rx, rw;
    uui_table_column_rect(&t, 4, &ex, &ew);
    ok("a hidden column is 0 wide", ew == 0, "");
    uui_table_column_show(&t, 4, 1);
    uui_table_column_rect(&t, 4, &ex, &ew);
    uui_table_column_rect(&t, 3, &rx, &rw);
    ok("shown, it takes its width after Read", ew > 0 && ex == rx + rw, "");
    ok("...and showing it is a layout change", uui_table_take_layout_changed(&t), "");
    uui_table_column_show(&t, 0, 0);
    ok("the first column cannot be hidden", uui_table_column_shown(&t, 0), "");

    // --- moving ---
    uui_table_column_move(&t, 3, 1);
    oks("move Read to the front (after Name)", layout(&t), "name,read,pid,cpu,extra");
    uui_table_column_move(&t, 0, 3);
    oks("Name does not move", layout(&t), "name,read,pid,cpu,extra");
    uui_table_columns_reset(&t);
    oks("reset puts every column back, hidden ones hidden", layout(&t), "name,pid,cpu,read,-extra");

    // --- save and load ---
    char tiny[8];
    ok("a save that does not fit writes nothing", uui_table_columns_save(&t, tiny, sizeof tiny) == 0 && !tiny[0], tiny);
    uui_table_columns_load(&t, "cpu,-pid,bogus,name");
    oks("load: order, hidden, an unknown id skipped, the unnamed kept after",
        layout(&t), "name,cpu,-pid,read,-extra");
    uui_table_columns_load(&t, "");
    oks("an empty string changes nothing", layout(&t), "name,cpu,-pid,read,-extra");
    uui_table_columns_reset(&t);

    // --- a drag through the ops table, the whole column ---
    uui_table_column_rect(&t, 3, &rx, &rw);
    t.sort_col = 0;
    drag(&t, 1, rx + rw + 20);   // PID past Read, to the end
    oks("dragging PID past Read moves it there", layout(&t), "name,cpu,read,pid,-extra");
    ok("...and a drag does not sort", t.sort_col == 0, "");
    ok("...and the app is told the layout changed", uui_table_take_layout_changed(&t), "");
    header_at(&t, 2, &x, &y);
    uui_table_ops.press(&t, x, y, 0);
    ok("a press alone does not sort while columns move", t.sort_col == 0, "");
    uui_table_ops.release(&t, x, y);
    ok("a press and release without moving sorts", t.sort_col == 2, "");
    drag(&t, 2, 2);   // CPU to the very left: it must stop after Name
    oks("nothing lands before the locked first column", layout(&t), "name,cpu,read,pid,-extra");

    // --- the header-only mode moves the same way ---
    uui_table_columns_reset(&t);
    uui_table_set_columns_mode(&t, UUI_TABLE_COLS_HEADER);
    uui_table_column_rect(&t, 3, &rx, &rw);
    drag(&t, 2, rx + rw + 20);
    oks("HEADER mode: the drop reorders as COLUMN mode does", layout(&t), "name,pid,read,cpu,-extra");

    // --- the menu ---
    uui_table_columns_reset(&t);
    ok("a right-click on the header has a menu", uui_table_ops.context_menu(&t, 10, 2, &src), "");
    ok("...one row per column, a separator and Reset", src.count == NC + 2, "");
    ok("...on a row it has none", !uui_table_ops.context_menu(&t, 10, uui_table_header_h(&t) + 5, &src), "");
    uui_table_ops.context_menu(&t, 10, 2, &src);
    ok("Name is ticked and greyed", (src.flags(src.w, 1) & UUI_MI_CHECKED) && (src.flags(src.w, 1) & UUI_MI_DISABLED), "");
    ok("Extra is not ticked", !(src.flags(src.w, 5) & UUI_MI_CHECKED), "");
    ok("choosing Extra shows it", src.run(src.w, 5) && uui_table_column_shown(&t, 4), "");
    ok("choosing PID hides it", src.run(src.w, 2) && !uui_table_column_shown(&t, 1), "");
    ok("choosing Name does nothing", !src.run(src.w, 1) && uui_table_column_shown(&t, 0), "");
    src.run(src.w, src.items[src.count - 1].code);
    oks("Reset columns", layout(&t), "name,pid,cpu,read,-extra");

    // --- resizing: a header divider, in any mode ---
    uui_table_set_columns_mode(&t, UUI_TABLE_COLS_FIXED);
    uui_table_columns_reset(&t);
    uui_table_take_layout_changed(&t);
    int px, pw, cx2, cw2;
    uui_table_column_rect(&t, 1, &px, &pw);
    uui_table_column_rect(&t, 2, &cx2, &cw2);
    int hy = t.y + uui_table_header_h(&t) / 2;
    ok("over PID's right edge the pointer is the resize arrow",
       uui_table_ops.cursor(&t, px + pw, hy) == WIN_CURSOR_RESIZE_H, "");
    ok("...and over its title it is not", uui_table_ops.cursor(&t, px + pw / 2, hy) == WIN_CURSOR_DEFAULT, "");
    uui_table_ops.press(&t, px + pw, hy, 0);
    uui_table_ops.motion(&t, px + pw + 15, hy, 1);
    uui_table_ops.motion(&t, px + pw + 30, hy, 1);
    uui_table_ops.release(&t, px + pw + 30, hy);
    int nx, nw;
    uui_table_column_rect(&t, 1, &nx, &nw);
    char d[96];
    snprintf(d, sizeof d, "was %d, now %d", pw, nw);
    ok("dragging PID's divider 30 px right widens it by 30", nw == pw + 30, d);
    // Name, the stretching column, gives up the 30: CPU keeps its width
    // and starts where PID now ends.
    int p2x, p2w;
    uui_table_column_rect(&t, 1, &p2x, &p2w);
    uui_table_column_rect(&t, 2, &nx, &nw);
    ok("...CPU follows it, its width kept", nx == p2x + p2w && nw == cw2, "");
    ok("...no sort happened", t.sort_col != 1, "");
    ok("...and the app is told the layout changed", uui_table_take_layout_changed(&t), "");
    char want[64];
    snprintf(want, sizeof want, "name,pid:%d,cpu,read,-extra", pw + 30);
    oks("the width is saved with the layout", layout(&t), want);
    uui_table_columns_load(&t, "name,pid,cpu:77,read");
    uui_table_column_rect(&t, 2, &nx, &nw);
    ok("load: cpu:77 makes CPU 77 wide", nw == 77, "");
    uui_table_column_rect(&t, 1, &nx, &nw);
    snprintf(d, sizeof d, "PID %d, declared %d", nw, pw);
    ok("...and a column named without one keeps its declared width", nw == pw, d);
    // A double click on the divider fits the column: "4.3" is the widest
    // of CPU's cells (rows 0..4), wider than its title.
    uui_table_column_rect(&t, 2, &nx, &nw);
    uui_table_ops.press(&t, nx + nw, hy, 0);
    uui_table_ops.release(&t, nx + nw, hy);
    uui_table_ops.press(&t, nx + nw, hy, 0);
    uui_table_ops.release(&t, nx + nw, hy);
    int fx, fw, need = ugfx_text_width("CPU") > ugfx_text_width("4.2") ? ugfx_text_width("CPU") : ugfx_text_width("4.2");
    uui_table_column_rect(&t, 2, &fx, &fw);
    snprintf(d, sizeof d, "width %d, the widest text %d", fw, need);
    ok("a double click on CPU's divider fits it to its widest cell", fw >= need && fw < need + 24 && fw != 77, d);
    uui_table_columns_reset(&t);
    uui_table_column_rect(&t, 2, &fx, &fw);
    ok("Reset columns forgets the widths", fw == cw2, "");

    // --- wider than the table: it scrolls sideways ---
    ok("columns that fit show no sideways bar", !uui_table_hbar_visible(&t), "");
    uui_table_columns_load(&t, "name,pid:900,cpu,read");
    ok("PID 900 wide in a 600-wide table: the sideways bar shows", uui_table_hbar_visible(&t), "");
    uui_table_column_rect(&t, 0, &nx, &nw);
    snprintf(d, sizeof d, "Name is %d wide", nw);
    ok("...and Name, which stretches, keeps ten digits' width instead of none", nw >= ugfx_char_advance('0') * 10, d);
    int bottom = t.y + t.h - uui_sbar_width() / 2;
    uui_table_ops.press(&t, t.x + t.w - 30, bottom, 0);   // the bar's track, right of its thumb: a page
    uui_table_ops.release(&t, t.x + t.w - 30, bottom);
    snprintf(d, sizeof d, "hscroll %d", t.hscroll);
    ok("a press on the bar's track scrolls the columns sideways", t.hscroll > 0, d);
    int sx, sw;
    uui_table_column_rect(&t, 0, &sx, &sw);
    ok("...Name moves left by as much", sx == t.x - t.hscroll, "");
    ok("...and the press selected no row", t.selected < 0 || t.selected == 0, "");
    // A column moved while the columns are scrolled lands by where they
    // ARE: Read dropped with its middle just past PID's lands after PID.
    uui_table_set_columns_mode(&t, UUI_TABLE_COLS_COLUMN);
    uui_table_columns_load(&t, "name,pid:560,cpu,read");
    t.hscroll = 150;
    int qx, qw;
    uui_table_column_rect(&t, 1, &qx, &qw);
    drag(&t, 3, qx + qw / 2 + 40);
    oks("scrolled: Read dropped just past PID's middle lands after PID", layout(&t), "name,pid:560,read,cpu,-extra");
    uui_table_set_columns_mode(&t, UUI_TABLE_COLS_FIXED);
    t.hscroll = 0;
    uui_table_columns_reset(&t);
    ok("back to the declared widths: no bar, scrolled back", !uui_table_hbar_visible(&t), "");

    return utest_end();
}
