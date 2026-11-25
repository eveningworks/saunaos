// uitest-app: userland/apps/system/confedit
//
// The Configuration Editor's own main, headless on the host
// (tools/lib/uitest/uitest_app.h): a merged registry the case declares --
// schema files in its own /etc/settings.d, a person's ~/.config, one
// kernel-registered setting and a page of facts -- and the window driven
// by its widget map and its `confedit:` lines.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: reading a value back through
// the app proves nothing about where it went, so every write is checked
// in BOTH files by reading their bytes (uitest_read_file()) -- an edit
// that landed in /etc would show the same value in the table. And the
// From column is asserted for each of its four answers, because a
// constant "Machine" would read right for half the rows.
//
// THE PANES SCROLL WITH MORE ROWS THAN FIT: forty fact providers make the
// tree taller than its pane, sixty settings under one branch the table,
// and End and Home must move each view (the harness has no wheel; the
// wheel, the thumb and the trough are tools/tests/gui/confedit_test.py's,
// on the real screen).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uitest_app.h"
#include "setting_abi.h"
#include "keyboard.h"

#define ROWS_IN_MANY 60
#define PROVIDERS 40

static size_t g_mark;

// The value after `key ` on the last `confedit: <what>` line.
static int field(const char *what, const char *key, char *out, size_t cap) {
    char line[512], needle[64];
    snprintf(needle, sizeof needle, "confedit: %s", what);
    if (!uitest_log_find(g_mark, needle, line, sizeof line)) return 0;
    char k[32];
    snprintf(k, sizeof k, " %s ", key);
    const char *p = strstr(line, k);
    if (!p) return 0;
    p += strlen(k);
    size_t n = strcspn(p, " \n");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int num(const char *what, const char *key) {
    char v[32];
    return field(what, key, v, sizeof v) ? atoi(v) : -9999;
}

static int layout(const char *name, int *x, int *y, int *w, int *h) {
    char line[256], needle[64];
    snprintf(needle, sizeof needle, "confedit: layout %s ", name);
    if (!uitest_log_find(g_mark, needle, line, sizeof line)) return 0;
    return sscanf(strstr(line, needle) + strlen(needle), "%d %d %d %d", x, y, w, h) == 4;
}

static int layout_int(const char *name) {
    char line[256], needle[64];
    snprintf(needle, sizeof needle, "confedit: layout %s ", name);
    if (!uitest_log_find(g_mark, needle, line, sizeof line)) return -1;
    return atoi(strstr(line, needle) + strlen(needle));
}

static int file_has(const char *path, const char *line) {
    char buf[4096];
    return uitest_read_file(path, buf, sizeof buf) && strstr(buf, line) != NULL;
}

static void declare(const char *name, const char *body) {
    char path[128];
    snprintf(path, sizeof path, "/etc/settings.d/%s", name);
    uitest_file(path, body);
}

// The row at table position `row` (header included), clicked.
static void click_row(int row) {
    int x, y, w, h;
    layout("values", &x, &y, &w, &h);
    int rh = layout_int("values.row_h"), hh = layout_int("values.header_h");
    uitest_app_click_at(x + 20, y + hh + rh * row + rh / 2);
}

static void click_tool(int index) {
    char name[32];
    snprintf(name, sizeof name, "toolbar.button %d", index);
    char line[256], needle[64];
    snprintf(needle, sizeof needle, "confedit: layout %s ", name);
    int x, y, w, h;
    if (!uitest_log_find(g_mark, needle, line, sizeof line) ||
        sscanf(strstr(line, needle) + strlen(needle), "%d %d %d %d", &x, &y, &w, &h) != 4) {
        utest_check(0, "the toolbar reports its buttons");
        return;
    }
    uitest_app_click_at(x + w / 2, y + h / 2);
}

int main(void) {
    uitest_begin("confedit_app", "Configuration Editor's own main, headless: the tree, From, edit, reset");
    uitest_setting("desktop.layout_log", "on");
    setenv("XDG_CONFIG_HOME", "/home/user/.config", 1);

    declare("desktop.clock_seconds", "Type=enum\nFile=/etc/desktop.conf\nScope=user\nLabel=Seconds\n"
                                     "Default=off\nChoices=off,on\nDescription=Seconds in the clock\n");
    declare("desktop.icon_size", "Type=enum\nFile=/etc/desktop.conf\nScope=user\nLabel=Size\n"
                                 "Default=medium\nChoices=small,medium,large\n");
    declare("desktop.wrap", "Type=enum\nFile=/etc/desktop.conf\nScope=user\nDefault=off\nChoices=off,on\n");
    declare("update.channel", "Type=enum\nFile=/etc/update.conf\nLabel=Channel\nDefault=stable\n"
                              "Choices=stable,dev\n");
    for (int i = 0; i < ROWS_IN_MANY; i++) {
        char n[48];
        snprintf(n, sizeof n, "desktop.many.k%02d", i);
        declare(n, "Type=int\nFile=/etc/desktop.conf\nScope=user\nDefault=1\nMin=0\nMax=9\n");
    }
    uitest_file("/etc/desktop.conf", "icon_size=large\n");
    uitest_file("/etc/update.conf", "channel=dev\n");
    uitest_file("/home/user/.config/desktop.conf", "clock_seconds=on\n");
    uitest_kernel_setting("system.mouse.speed", "5", SETTING_ABI_TYPE_INT, 1, 10);
    for (int i = 0; i < PROVIDERS; i++) {
        char n[48];
        snprintf(n, sizeof n, "p%02d.count", i);
        uitest_fact(n, (unsigned long long)i * 7);
    }

    g_mark = uitest_log_mark();
    char *argv[] = { "confedit", NULL };
    if (!uitest_app_start(1, argv)) return uitest_end();
    utest_check(!strcmp(uitest_app_title(), "Configuration Editor"), "the window is titled Configuration Editor");

    char v[96];
    int want = 3 + ROWS_IN_MANY + 1 + 1 + PROVIDERS;
    {
        char line[160];
        int n = -1, b = -1;
        if (uitest_log_find(g_mark, "confedit: loaded ", line, sizeof line))
            sscanf(strstr(line, "loaded ") + 7, "%d entries, %d branches", &n, &b);
        utest_checkf(n == want, "every setting, the kernel's one and every fact are read (%d, want %d)", n, want);
        // desktop, desktop.many, system, system.mouse, update, p00..p39
        utest_checkf(b == 5 + PROVIDERS, "a branch per distinct prefix (%d)", b);
    }

    // --- the first branch, and From ------------------------------------------
    field("branch", "branch", v, sizeof v);
    utest_checkf(!strcmp(v, "desktop"), "it opens on the first branch, in the tree's order (%s)", v);
    uitest_check_int("desktop lists its own and everything under it", num("branch", "rows"), 3 + ROWS_IN_MANY);
    field("branch", "selected", v, sizeof v);
    utest_checkf(!strcmp(v, "desktop.clock_seconds"), "the first row is selected (%s)", v);
    field("branch", "from", v, sizeof v);
    utest_checkf(!strcmp(v, "You"), "a value in ~/.config is From You (%s)", v);
    utest_check(num("branch", "reset") == 1, "...and can be reset");

    click_row(1);
    field("branch", "selected", v, sizeof v);
    utest_checkf(!strcmp(v, "desktop.icon_size"), "a click selects a row (%s)", v);
    field("branch", "from", v, sizeof v);
    utest_checkf(!strcmp(v, "Machine"), "a value in /etc alone is From Machine (%s)", v);
    utest_check(num("branch", "edit") == 1 && num("branch", "reset") == 0,
                "a person's key edits, and has nothing of the person's to reset");
    click_row(2);
    field("branch", "from", v, sizeof v);
    utest_checkf(!strcmp(v, "Default"), "a value in neither file is From Default (%s)", v);

    // --- edit: the person's file, never /etc -----------------------------------
    click_row(1);
    uitest_app_key('\n', 0);
    utest_check(uitest_app_has_widget("value"), "Enter opens the edit dialog with its field");
    uitest_app_type("small");
    uitest_app_key('\n', 0);
    utest_check(file_has("/home/user/.config/desktop.conf", "icon_size=small"),
                "Save writes the person's ~/.config file");
    utest_check(file_has("/etc/desktop.conf", "icon_size=large"), "...and /etc keeps the machine's value");
    field("branch", "from", v, sizeof v);
    utest_checkf(!strcmp(v, "You"), "the row is From You now (%s)", v);
    utest_check(num("branch", "reset") == 1, "...and can be reset");

    // A refused value keeps the dialog, and changes nothing.
    uitest_app_key('\n', 0);
    uitest_app_type("huge");
    uitest_app_key('\n', 0);
    utest_check(uitest_app_has_widget("value"), "a value the setting refuses keeps the dialog open");
    utest_check(file_has("/home/user/.config/desktop.conf", "icon_size=small"), "...and writes nothing");
    uitest_app_key(0x1B, 0);
    utest_check(!uitest_app_has_widget("value"), "Esc cancels it");

    // --- reset: the person's value goes, the machine's shows ----------------------
    click_tool(1);
    utest_check(!file_has("/home/user/.config/desktop.conf", "icon_size="), "Reset removes the person's value");
    utest_check(file_has("/etc/desktop.conf", "icon_size=large"), "...and leaves /etc alone");
    field("branch", "from", v, sizeof v);
    utest_checkf(!strcmp(v, "Machine"), "the row is From Machine again (%s)", v);

    // --- the table scrolls: sixty rows under one branch ----------------------------
    click_row(0);
    utest_check(num("table", "top") == 0 && num("table", "visible") < 3 + ROWS_IN_MANY,
                "the table has more rows than fit, and opens at the top");
    uitest_app_key(KEY_END, 0);
    int t1 = num("table", "top");
    utest_checkf(t1 > 0, "End scrolls the table down (top %d)", t1);
    uitest_app_key(KEY_HOME, 0);
    utest_checkf(num("table", "top") == 0, "Home scrolls it back (top %d)", num("table", "top"));

    // --- machine keys are read-only; kernel keys edit; facts never -----------------
    // The tree: Tab back to it, End to its last branch (update).
    uitest_app_key('\t', KEY_MOD_SHIFT);
    uitest_app_key(KEY_END, 0);
    field("branch", "branch", v, sizeof v);
    utest_checkf(!strcmp(v, "update"), "End in the tree reaches the last branch (%s)", v);
    utest_check(num("tree", "top") > 0, "...scrolling the tree, taller than its pane");
    field("branch", "from", v, sizeof v);
    utest_checkf(!strcmp(v, "Machine") && num("branch", "edit") == 0,
                 "a machine-wide setting shows From Machine and is read-only here (%s)", v);
    uitest_app_key(KEY_ARROW_UP, 0);
    field("branch", "branch", v, sizeof v);
    utest_checkf(!strcmp(v, "system"), "Up reaches system (%s)", v);
    field("branch", "from", v, sizeof v);
    utest_checkf(!strcmp(v, "Kernel") && num("branch", "edit") == 1,
                 "a kernel-registered setting is From Kernel and edits (%s)", v);
    uitest_app_key('\t', 0);
    uitest_app_key('\n', 0);
    uitest_app_type("11");
    uitest_app_key('\n', 0);
    utest_check(uitest_app_has_widget("value"), "the kernel's range refuses 11");
    uitest_app_key(0x1B, 0);
    uitest_app_key('\n', 0);
    uitest_app_type("7");
    uitest_app_key('\n', 0);
    {
        char line[160];
        utest_check(uitest_log_find(g_mark, "confedit: set system.mouse.speed = \"7\" -> 0 result 1", line, sizeof line),
                    "...and saves 7 through the registry");
    }
    // Collapsing a branch above the selection selects the branch
    // collapsed, and the table follows it.
    uitest_app_key('\t', KEY_MOD_SHIFT);
    uitest_app_key(KEY_ARROW_RIGHT, 0);
    uitest_app_key(KEY_ARROW_DOWN, 0);
    field("branch", "branch", v, sizeof v);
    utest_checkf(!strcmp(v, "system.mouse"), "Right and Down reach system.mouse (%s)", v);
    {
        int x, y, w, h, rh = layout_int("tree.row_h"), top = num("tree", "top"), row = -1;
        char line[160];
        if (uitest_log_find(g_mark, "confedit: tree row ", line, sizeof line)) {
            // The newest listing: find system's row in it.
            for (int r = 0; r < 64 && row < 0; r++) {
                char needle[64];
                snprintf(needle, sizeof needle, "confedit: tree row %d \"system\"", r);
                if (uitest_log_find(g_mark, needle, line, sizeof line)) row = r;
            }
        }
        layout("tree", &x, &y, &w, &h);
        uitest_app_click_at(x + 6, y + (row - top) * rh + rh / 2);   // its expander
    }
    field("branch", "branch", v, sizeof v);
    utest_checkf(!strcmp(v, "system") && num("branch", "rows") == 1,
                 "collapsing system over system.mouse selects system, the table following (%s)", v);
    uitest_app_key(KEY_ARROW_UP, 0);
    field("branch", "branch", v, sizeof v);
    utest_checkf(!strcmp(v, "p39"), "the fact branches sit above (%s)", v);
    field("branch", "from", v, sizeof v);
    utest_checkf(!strcmp(v, "Kernel") && num("branch", "edit") == 0 && num("branch", "reset") == 0,
                 "a fact is the kernel's and never edits (%s)", v);

    // --- find: names and values across the whole tree ---------------------------------
    uitest_app_key(0x06, KEY_MOD_CTRL);
    uitest_app_type("k5");
    uitest_check_int("Find lists every name holding the text, from any branch", num("branch", "rows"), 10);
    uitest_app_key(0x1B, 0);
    field("branch", "find", v, sizeof v);
    utest_checkf(!strcmp(v, "\"\""), "Esc empties the find (%s)", v);
    return uitest_end();
}
