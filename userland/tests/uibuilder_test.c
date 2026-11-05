// base/udecl (the .ui language) and ui/uui_builder (a window from it).
//
// THE LOAD-BEARING CHECKS are the REJECTIONS, each at the right line: a
// builder that guessed -- skipped an attribute it did not know, took an
// unknown command as 0, put width= on the wrong axis -- builds a window
// that looks nearly right, which is the failure a person editing a file
// cannot see. And the shipped crashreports.ui must build with every name
// the app asks for, read from the disk the app reads it from.
#include <string.h>
#include <stdio.h>
#include "base/udecl.h"
#include "ui/uui_builder.h"
#include "ui/uui_layout.h"
#include "ui/uui_button.h"
#include "ui/uui_label.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_table.h"
#include "ui/uui_markdown.h"
#include "ui/uui_focus.h"
#include "ui/utheme.h"
#include "base/utest.h"

static struct udecl d;

static int parses(const char *src) {
    int rc = udecl_parse(&d, "t.ui", src, strlen(src));
    return rc;
}

// The parse fails, and its message starts "t.ui:<line>:" and holds `has`.
static void rejects(const char *src, int line, const char *has, const char *what) {
    char want[32];
    snprintf(want, sizeof want, "t.ui:%d:", line);
    int rc = parses(src);
    utest_checkf(rc < 0 && !strncmp(d.error, want, strlen(want)) && strstr(d.error, has),
                 "%s (%s)", what, rc < 0 ? d.error : "parsed");
    udecl_free(&d);
}

static void parser(void) {
    const char *src =
        "// a comment\n"
        "\n"
        "window #main title=\"A \\\"quoted\\\" title\" size=10x4 resizable {   // trailing\n"
        "    row gap=2 {\n"
        "        label \"one\\ntwo\" fill-w\n"
        "    }\n"
        "}\n"
        "window #second {\n"
        "}\n";
    int rc = parses(src);
    utest_checkf(rc == 0, "a two-window file parses (%s)", d.error);
    if (rc == 0) {
        struct udecl_node *w = &d.top.kid[0];
        utest_check(d.top.nkid == 2 && !strcmp(w->type, "window") && !strcmp(w->name, "main") && w->line == 3,
                    "the top holds both windows, the first named, at its line");
        utest_checkf(!strcmp(udecl_get(w, "title"), "A \"quoted\" title"), "escapes are undone in place (%s)",
                     udecl_get(w, "title"));
        utest_check(udecl_flag(w, "resizable") && !udecl_flag(w, "nope"), "a bare flag is present, another is not");
        int cw = 0, ch = 0;
        utest_check(udecl_pair(&d, w, "size", 1, 100, &cw, &ch) == 1 && cw == 10 && ch == 4, "size=10x4 reads as a pair");
        struct udecl_node *l = &w->kid[0].kid[0];
        utest_check(w->nkid == 1 && w->kid[0].nkid == 1 && !strcmp(l->text, "one\ntwo") && l->line == 5,
                    "a nested node keeps its text and line");
        utest_check(udecl_unused(&d, l) < 0 && strstr(d.error, "fill-w"), "an attribute nobody read is named");
    }
    udecl_free(&d);

    rejects("window #a {\n  row {\n  }\n", 1, "never closed", "an unclosed block names the line that opened it");
    rejects("row\n}\n", 2, "no block open", "a stray '}' is refused at its line");
    rejects("row \"abc\n", 1, "not closed", "a string must close on its line");
    rejects("row \"a\\qb\"\n", 1, "escape", "an unknown escape is refused");
    rejects("\nrow { label\n", 2, "end of a line", "'{' only ends a line");
    rejects("row gap=1 gap=2\n", 1, "twice", "a key given twice is refused");
    rejects("row #a #b\n", 1, "two names", "a node has one name");
    rejects("row{\n}\n", 1, "space", "a word runs into '{' with no space");
    rejects("row gap=\n", 1, "no value", "key= with no value is refused");
    rejects("label \"x\" \"y\"\n", 1, "two texts", "a node has one text");

    rc = parses("row margin=abc gap=5000\n");
    int v = 0;
    utest_check(rc == 0 && udecl_int(&d, &d.top.kid[0], "margin", 0, 10, &v) < 0 && strstr(d.error, "not a number"),
                "a number that is not one is refused");
    udecl_free(&d);
    rc = parses("row gap=5000\n");
    utest_check(rc == 0 && udecl_int(&d, &d.top.kid[0], "gap", 0, 1000, &v) < 0 && strstr(d.error, "outside"),
                "a number out of range is refused");
    udecl_free(&d);
}

enum { ID_LIST = 1, CMD_GO = 10, CMD_STOP, CMD_HELP };
static const struct uui_builder_name NAMES[] = {
    { "list", ID_LIST }, { "go", CMD_GO }, { "stop", CMD_STOP }, { "help", CMD_HELP },
};
#define NN (int)(sizeof NAMES / sizeof NAMES[0])

static const char GOOD[] =
    "window #w title=\"Test\" app-id=tw size=40x10 min=200x100 resizable focus=\"stop go list\" {\n"
    "    column margin=3 gap=2 {\n"
    "        toolbar #tb fill-w {\n"
    "            item \"Go\" icon=tb-open tip=\"Go now\" command=go role=nav\n"
    "            separator\n"
    "            item icon=tb-help command=help role=danger end\n"
    "        }\n"
    "        row {\n"
    "            label #side width=12 fill-h dim\n"
    "            table #list fill {\n"
    "                column \"Name\" width=20\n"
    "                column \"Size\" align=right\n"
    "            }\n"
    "        }\n"
    "        row {\n"
    "            stretch\n"
    "            button #go \"Go\" default\n"
    "            button #stop \"Stop\" hidden\n"
    "        }\n"
    "        markdown #doc height=5\n"
    "    }\n"
    "}\n";

// Builds `src` and expects it refused with `has` in the message.
static void build_rejects(const char *src, const char *has, const char *what) {
    struct uui_builder *b = uui_builder_parse("t.ui", src, strlen(src), NAMES, NN);
    const char *e = uui_builder_error(b);
    utest_checkf(e && strstr(e, has), "%s (%s)", what, e ? e : "built");
    uui_builder_free(b);
}

static void builder(void) {
    struct uui_builder *b = uui_builder_parse("t.ui", GOOD, sizeof GOOD - 1, NAMES, NN);
    utest_checkf(b && !uui_builder_error(b), "a window with every type builds (%s)",
                 uui_builder_error(b) ? uui_builder_error(b) : "ok");
    struct uapp_desc desc = { .title = "kept?" };
    struct uui_toolbar *tb = uui_builder_widget(b, "w", "tb", &uui_toolbar_ops);
    struct uui_table *t = uui_builder_widget(b, "w", "list", &uui_table_ops);
    struct uui_item *side = uui_builder_item(b, "w", "side");
    struct uui_item *go = uui_builder_item(b, "w", "go");
    struct uui_item *doc = uui_builder_item(b, "w", "doc");
    utest_check(!uui_builder_error(b) && tb && t && side && go && doc, "every name is found, of its type");
    if (uui_builder_error(b)) { uui_builder_free(b); return; }

    utest_check(uui_builder_window(b, "w", &desc) && !strcmp(desc.title, "Test") && !strcmp(desc.app_id, "tw") &&
                desc.cells_w == 40 && desc.cells_h == 10 && desc.min_w == 200 && desc.min_h == 100 &&
                (desc.flags & UAPP_RESIZABLE), "the window's attributes reach the desc");
    struct uui_layout *root = desc.layout;
    utest_check(root && root->dir == UUI_COLUMN && root->margin == 3 && root->gap == 2 && root->count == 4 &&
                desc.widgets == root->items && desc.widget_count == 4, "the root column is the desc's layout and widgets");

    utest_check(tb->count == 3 && tb->items[0].code == CMD_GO && !strcmp(tb->items[0].label, "Go") &&
                tb->items[0].tint == UTHEME_ACT_NAV && !tb->items[1].icon &&
                tb->items[2].code == CMD_HELP && !tb->items[2].label && (tb->items[2].flags & UUI_TB_END) &&
                tb->items[2].tint == UTHEME_ACT_DANGER, "toolbar items carry their codes, roles, separators and flags");
    utest_check(t->col_count == 2 && !strcmp(t->cols[0].title, "Name") && t->cols[0].width_chars == 20 &&
                t->cols[1].width_chars == 0 && t->cols[1].align == UUI_TALIGN_RIGHT, "table columns are built");
    utest_check(uui_builder_item(b, "w", "list")->id == ID_LIST && side->id == 0,
                "a #name in the app's table is the widget's id; one not in it is 0");
    utest_check(side->main_cells == 12 && side->flags == UUI_FILL_H && ((struct uui_label *)side->widget)->dim,
                "width= pins a row's child in cells, beside fill-h and dim");
    utest_check(doc->main_cells == 5 && doc->flags == 0, "height= pins a column's child");
    struct uui_layout *btns = root->items[2].widget;
    struct uui_button *gb = go->widget;
    utest_check(btns->items[0].flags == UUI_FILL_W && go->id == CMD_GO && gb->code == CMD_GO &&
                gb->bg == UUI_ROLE(accent) && btns->items[2].hidden,
                "stretch fills a row; a button's id and code are its command; default is the accent; hidden holds");
    // The focus ring is built at window(), skipping the hidden Stop.
    utest_check(desc.focus && desc.focus->count == 2 && desc.focus->items[0].widget == gb &&
                desc.focus->items[1].widget == t, "focus= is the order given, without the hidden widget");
    utest_check(!uui_builder_widget(b, "w", "side", &uui_button_ops) && uui_builder_error(b) &&
                strstr(uui_builder_error(b), "not the kind"), "a lookup of the wrong type is an error, not a cast");
    uui_builder_free(b);

    build_rejects("window #w {\n row {\n  lable \"x\"\n }\n}\n", "t.ui:3:", "a misspelt type is refused at its line");
    build_rejects("window #w {\n row {\n  label \"x\" fil-w\n }\n}\n", "no 'fil-w'", "a misspelt attribute is refused");
    build_rejects("window #w {\n row {\n  button \"x\" command=fly\n }\n}\n", "no command 'fly'",
                  "a command the app lacks is refused, not left 0");
    build_rejects("window #w {\n row {\n  button \"x\"\n }\n}\n", "needs command=", "a button needs a command");
    build_rejects("window #w {\n column {\n  label width=3\n }\n}\n", "not in a row", "width= in a column is refused");
    build_rejects("window #w {\n row {\n  label #a\n  label #a\n }\n}\n", "two #a", "a name twice in a window is refused");
    build_rejects("window #w focus=nope {\n row {\n  label\n }\n}\n", "lacks", "focus= naming nothing is refused");
    build_rejects("window #w focus=a {\n row {\n  label #a\n }\n}\n", "cannot be focused", "focus= on a label is refused");
    build_rejects("window #w {\n row {\n  label\n }\n column {\n  label\n }\n}\n", "one row", "a window has one root");
    build_rejects("row {\n label\n}\n", "holds windows", "the top holds only windows");
    build_rejects("window #w {\n row {\n  label {\n   label\n  }\n }\n}\n", "no children", "a label takes no children");
    build_rejects("window #w {\n row {\n  toolbar {\n   item \"x\" command=go\n  }\n }\n}\n", "icon=",
                  "a toolbar item needs an icon");

    b = uui_builder_parse("t.ui", GOOD, sizeof GOOD - 1, NAMES, NN);
    utest_check(!uui_builder_widget(b, "nowin", "tb", 0) && strstr(uui_builder_error(b), "#nowin"),
                "asking for a window the file lacks is an error");
    uui_builder_free(b);
}

// The shipped file, from where Crash Reports reads it: every name the app
// asks for, of its type. A rename in the file alone fails here, not on
// a person's screen.
static void shipped(void) {
    static const struct uui_builder_name CR[] = {
        { "reports", 1 }, { "tb", 2 }, { "details", 3 }, { "report", 4 },
        { "open", 100 }, { "copy", 101 }, { "delete", 102 }, { "reopen", 103 },
        { "all", 104 }, { "close", 105 }, { "notepad", 106 }, { "show", 107 },
    };
    struct uui_builder *b = uui_builder_load("crashreports", CR, (int)(sizeof CR / sizeof CR[0]));
    uui_builder_widget(b, "list", "reports", &uui_table_ops);
    uui_builder_widget(b, "list", "tb", &uui_toolbar_ops);
    uui_builder_widget(b, "list", "details", &uui_label_ops);
    uui_builder_widget(b, "dialog", "body", &uui_label_ops);
    uui_builder_item(b, "dialog", "reopen");
    uui_builder_widget(b, "dialog", "close", &uui_button_ops);
    uui_builder_widget(b, "viewer", "report", &uui_markdown_ops);
    uui_builder_widget(b, "viewer", "tb", &uui_toolbar_ops);
    const char *e = uui_builder_error(b);
    utest_checkf(b && !e, "/usr/share/ui/crashreports.ui builds with every name the app needs (%s)", e ? e : "ok");
    utest_notef("crashreports.ui read and built in %u us", uui_builder_build_us(b));
    uui_builder_free(b);
}

int main(void) {
    utest_begin("uibuilder_test", "base/udecl and ui/uui_builder", UTEST_VERDICT_FILE);
    parser();
    builder();
    shipped();
    return utest_end();
}
