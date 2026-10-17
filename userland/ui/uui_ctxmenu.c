// A widget's own right-click menu, hosted once per process -- see
// uui_ctxmenu.h. A uui_menubar with no bar, as uui_editmenu is.
#include "ui/uui_ctxmenu.h"

static struct uui_menubar g_menu;
static struct uui_ctxmenu_src g_src;   // valid while the menu is open
static int g_target_id;
static int g_inited;

static struct uui_item g_item = {
    .ops = &uui_menubar_ops, .widget = &g_menu, .id = UUI_CTXMENU_ID, .name = "ctxmenu",
};

struct uui_item *uui_ctxmenu_item(void) { return &g_item; }

// uui_menubar's flags callback takes no context: the one open source is
// the context, which is why there is one menu per process.
static unsigned row_flags(int code) {
    return g_src.flags ? g_src.flags(g_src.w, code) : 0;
}

void uui_ctxmenu_open(const struct uui_ctxmenu_src *src, int target_id,
                      int x, int y, int bw, int bh) {
    if (!g_inited) {
        uui_menubar_init(&g_menu, 0, 0);   // no bar: a context menu (ui/uui_menubar.h)
        g_menu.item_flags = row_flags;
        g_inited = 1;
    }
    g_src = *src;
    g_target_id = target_id;
    uui_menubar_set_bounds(&g_menu, 0, 0, bw, bh);
    uui_menubar_open_at(&g_menu, g_src.items, g_src.count, x, y);
}

int uui_ctxmenu_is_open(void) { return g_inited && uui_menubar_is_open(&g_menu); }

void uui_ctxmenu_close(void) { if (g_inited) uui_menubar_close(&g_menu); }

static int run(int code) {
    if (code <= 0 || !g_src.run || (row_flags(code) & UUI_MI_DISABLED)) return 0;
    return g_src.run(g_src.w, code);
}

int uui_ctxmenu_take(int *out_target_id) {
    if (!run(uui_menubar_take_code(&g_menu))) return 0;
    if (out_target_id) *out_target_id = g_target_id;
    return 1;
}

int uui_ctxmenu_key(int key, int *out_target_id) {
    if (!uui_ctxmenu_is_open()) return 0;
    int code;
    if (!uui_menubar_key(&g_menu, key, &code)) return 0;
    if (run(code)) {
        if (out_target_id) *out_target_id = g_target_id;
        return 2;
    }
    return 1;
}
