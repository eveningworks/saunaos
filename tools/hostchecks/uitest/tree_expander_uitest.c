// A TREE'S EXPANDER AND ITS DOUBLE-CLICK, in every uui_tree (the
// maintainer's picks, 2026-11-25):
//   - the chevron is drawn in a near-text grey, not the muted one, so a
//     parent reads as one -- asserted on the darkest pixel in its column;
//   - under the pointer it gets a chip of its own, a step darker than the
//     row's hover -- asserted as a pixel beside the chevron that changes
//     when the pointer moves from the label to the expander;
//   - a double-click on a parent's ROW toggles it, a single click does not,
//     and an app that set `no_dbl_toggle` keeps the row as it was.
// A plain (UUI_TREE_AUTO) tree, so the widget's own bitmap is what flips.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_tree.h"
#include "uitest.h"

static const struct uui_tree_node NODES[] = {
    { "desktop", 0, 1 },
    {   "corners", 1, 2 },
    {   "taskbar", 1, 3 },
    { "system", 0, 4 },
    {   "mouse", 1, 5 },
};

static struct uui_tree g_tree;
static struct uui_item g_items[] = {
    { .ops = &uui_tree_ops, .widget = &g_tree, .id = 1, .name = "tree" },
};

static unsigned luma(uint32_t c) { return ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF); }

int main(void) {
    uitest_begin("tree_expander", "a tree's expander: its ink, its hover chip, a double-click toggles");
    uui_tree_init(&g_tree, 0, 0, 0, 0, NODES, 5);
    struct uitest_win win;
    uitest_window(&win, g_items, 1, 0, 240, 200);
    g_tree.x = 0; g_tree.y = 0; g_tree.w = 240; g_tree.h = 200;
    int rh = uui_tree_row_h(&g_tree);
    uitest_frame(&win);

    // The chevron's darkest pixel, in the expander column of row 0.
    unsigned dark = 999;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < 4 + ugfx_px(16); x++) {
            unsigned l = luma(uitest_pixel_at(&win, x, y));
            if (l < dark) dark = l;
        }
    char d[64];
    snprintf(d, sizeof d, "darkest %u of 765", dark);
    utest_check_detail(dark < 3 * 100, "the chevron is drawn in a near-text grey, not the muted one", d);

    // The chip: a pixel inside it but off the chevron's stroke.
    int ex = 4 + ugfx_px(16) / 2, chip_x = ex - ugfx_px(18) / 2 + 2, chip_y = rh / 2 - ugfx_px(18) / 2 + 2;
    uitest_hover(&win, "tree", 120, rh / 2);   // on the label: the row's hover
    uitest_frame(&win);
    uint32_t on_label = uitest_pixel_at(&win, chip_x, chip_y);
    uitest_hover(&win, "tree", ex, rh / 2);    // on the expander
    uitest_frame(&win);
    uint32_t on_exp = uitest_pixel_at(&win, chip_x, chip_y);
    snprintf(d, sizeof d, "%06x on the label, %06x on the expander", (unsigned)on_label, (unsigned)on_exp);
    utest_check_detail(luma(on_exp) < luma(on_label), "the expander under the pointer gets a darker chip", d);

    // A double-click on the row toggles; a single click does not.
    uitest_check_int("desktop starts open", uui_tree_is_collapsed(&g_tree, 0), 0);
    uitest_click_in(&win, "tree", 120, rh / 2);
    uitest_check_int("one click on its row leaves it open", uui_tree_is_collapsed(&g_tree, 0), 0);
    uitest_double_click_in(&win, "tree", 120, rh / 2);
    uitest_check_int("a double-click on its row closes it", uui_tree_is_collapsed(&g_tree, 0), 1);
    uitest_double_click_in(&win, "tree", 120, rh / 2);
    uitest_check_int("...and another opens it", uui_tree_is_collapsed(&g_tree, 0), 0);
    g_tree.no_dbl_toggle = 1;
    uitest_double_click_in(&win, "tree", 120, rh / 2);
    uitest_check_int("with no_dbl_toggle set, a double-click leaves it", uui_tree_is_collapsed(&g_tree, 0), 0);

    uitest_window_free(&win);
    return uitest_end();
}
