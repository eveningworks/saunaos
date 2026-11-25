// A dialog window's DEFAULT BUTTON (uapp_window_desc.default_button): an
// Enter the focus ring passed on commits it, as a click would.
//
// What a broken version would still pass, and what stops it: one that
// committed on every key passes "Enter commits", so a letter must reach
// on_key and commit nothing; one that ignored `disabled` passes the first
// half, so a greyed default must commit nothing; one that took Ctrl+Enter
// too would steal a field's own binding, so that must reach on_key.
#include <string.h>
#include "ui/uapp.h"
#include "ui/uapp_internal.h"
#include "ui/uui_button.h"
#include "keyboard.h"
#include "uitest.h"

static int g_action = -1, g_keys;

static void on_action(struct uapp_window *w, int code) { (void)w; g_action = code; }
static void on_key(struct uapp_window *w, int key, unsigned mods) { (void)w; (void)key; (void)mods; g_keys++; }

int main(void) {
    uitest_begin("default_button", "uapp_window_desc.default_button: Enter is the dialog's answer");
    static struct uui_button ok = { .code = 42 };
    static struct uapp_window w;
    w.desc.on_action = on_action;
    w.desc.on_key = on_key;
    w.desc.default_button = &ok;

    WIN_OPS.key(&w.top, '\n', 0);
    utest_check(g_action == 42 && g_keys == 0, "Enter commits the default button, and is spent");
    g_action = -1;
    WIN_OPS.key(&w.top, '\r', 0);
    utest_check(g_action == 42, "...Return too");

    g_action = -1;
    WIN_OPS.key(&w.top, 'a', 0);
    utest_check(g_action == -1 && g_keys == 1, "a letter reaches on_key and commits nothing");

    g_action = -1;
    WIN_OPS.key(&w.top, '\n', KEY_MOD_CTRL);
    utest_check(g_action == -1 && g_keys == 2, "Ctrl+Enter is the field's, not the default's");

    ok.disabled = 1;
    g_action = -1;
    WIN_OPS.key(&w.top, '\n', 0);
    utest_check(g_action == -1, "a greyed default commits nothing");

    w.desc.default_button = 0;
    WIN_OPS.key(&w.top, '\n', 0);
    utest_check(g_action == -1 && g_keys == 3, "with no default, Enter reaches on_key");
    return uitest_end();
}
