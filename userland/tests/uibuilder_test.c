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
#include "ui/uui_dropdown.h"
#include "ui/uui_textbox.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_progress.h"
#include "ui/uui_listbox.h"
#include "ui/uui_colourpick.h"
#include "ui/uui_themepreview.h"
#include "ui/uui_stack.h"
#include "ui/uui_wizard.h"
#include "ui/uui_dialog.h"
#include "ui/uui_menubar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_loglist.h"
#include "ui/uui_route.h"
#include "ui/uui_meter.h"
#include "ui/uui_chart.h"
#include "ui/uui_image.h"
#include "ui/uui_thumbstrip.h"
#include "ui/uui_transport.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_tree.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "base/utest.h"
#include "base/uprefs.h"
#include "ui/uui_prefs.h"
#include <stddef.h>
#include "keyboard.h"

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
        utest_checkf(!strcmp(udecl_get(&d, w, "title"), "A \"quoted\" title"), "escapes are undone in place (%s)",
                     udecl_get(&d, w, "title"));
        utest_check(udecl_flag(&d, w, "resizable") && !udecl_flag(&d, w, "nope"), "a bare flag is present, another is not");
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

    rc = parses("row hidden=no\n");
    utest_check(rc == 0 && !udecl_flag(&d, &d.top.kid[0], "hidden") && strstr(d.error, "is a flag"),
                "a flag given a value is refused, not read as set");
    udecl_free(&d);
    rc = parses("window title\n");
    utest_check(rc == 0 && !udecl_get(&d, &d.top.kid[0], "title") && strstr(d.error, "needs a value"),
                "a value written bare is refused, not read as \"\"");
    udecl_free(&d);
    rc = parses("row margin=12x gap=1\n");
    int pw = 0, ph = 0;
    utest_check(rc == 0 && udecl_pair(&d, &d.top.kid[0], "margin", 0, 99, &pw, &ph) < 0, "a pair missing its height is refused");
    udecl_free(&d);

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

// The form types: what each reads from the file, and how Tab reaches it.
enum { ID_PICK = 40 };
static const struct uui_builder_name FNAMES[] = { { "pick", ID_PICK }, { "go", CMD_GO } };
static const char FORM[] =
    "window #f focus=\"size name keep pick list\" autofocus {\n"
    "    column {\n"
    "        dropdown #size selected=2 {\n"
    "            item \"Small\"\n"
    "            item \"Medium\"\n"
    "            item \"Large\"\n"
    "        }\n"
    "        dropdown #later\n"
    "        textbox #name \"untitled\"\n"
    "        checkbox #keep \"Keep a copy\" checked\n"
    "        progress #bar value=250\n"
    "        listbox #list {\n"
    "            item \"One\"\n"
    "            item \"Two\"\n"
    "        }\n"
    "        custom #area fill\n"
    "        themepreview #preview\n"
    "        colourpick #pick swatch\n"
    "    }\n"
    "}\n";

static void form(void) {
    struct uui_builder *b = uui_builder_parse("f.ui", FORM, sizeof FORM - 1, FNAMES, 2);
    struct uui_dropdown *size = uui_builder_widget(b, "f", "size", "dropdown");
    struct uui_dropdown *later = uui_builder_widget(b, "f", "later", "dropdown");
    struct uui_textbox *name = uui_builder_widget(b, "f", "name", "textbox");
    struct uui_checkbox *keep = uui_builder_widget(b, "f", "keep", "checkbox");
    struct uui_progress *bar = uui_builder_widget(b, "f", "bar", "progress");
    struct uui_listbox *list = uui_builder_widget(b, "f", "list", "listbox");
    struct uui_custom *area = uui_builder_widget(b, "f", "area", "custom");
    struct uui_colourpick *pick = uui_builder_widget(b, "f", "pick", "colourpick");
    struct uui_item *pick_it = uui_builder_item(b, "f", "pick");
    uui_builder_widget(b, "f", "preview", "themepreview");
    const char *e = uui_builder_error(b);
    utest_checkf(!e, "a form with every new type builds (%s)", e ? e : "ok");
    if (e) { uui_builder_free(b); return; }
    utest_check(size->list.count == 3 && !strcmp(size->list.items[2], "Large") && uui_dropdown_selected(size) == 2,
                "a dropdown's items and selected= are read");
    utest_check(later->list.count == 0, "a dropdown may leave its items to the app");
    utest_checkf(!strcmp(uui_textbox_text(name), "untitled"), "a textbox starts with its text (%s)", uui_textbox_text(name));
    utest_check(keep->checked && !strcmp(keep->label, "Keep a copy"), "a checkbox reads its text and checked");
    utest_check(bar->value == 250, "progress reads value= per mille");
    utest_check(list->count == 2 && !strcmp(list->items[1], "Two"), "a listbox reads its items");
    utest_check(area->draw == 0 && (uui_builder_item(b, "f", "area")->flags & UUI_FILL_W), "custom is the app's to draw");
    utest_check(pick && pick->id_base == ID_PICK && pick_it->widget != pick && pick_it->id == 0,
                "colourpick: the object, its ids from its name's value, the tree item answering to none");
    struct uapp_desc desc = { 0 };
    uui_builder_window(b, "f", &desc);
    // size, name, keep, the picker's six, list: ten, through the focus tables.
    utest_checkf(desc.focus && desc.focus->count == 10 && desc.focus->items[0].ops == &uui_dropdown_focus_ops &&
                 desc.focus->items[1].ops == &uui_textbox_focus_ops && desc.focus->items[2].ops == &uui_checkbox_ops &&
                 desc.focus->items[9].ops == &uui_listbox_focus_ops && desc.focus->current == 0,
                 "focus= reaches each type through its focus table, the picker as its six, autofocus on the first (%d)",
                 desc.focus ? desc.focus->count : -1);
    uui_builder_free(b);

    // A stack: pages in one place, sized to the largest, one shown.
    static const char STACK[] =
        "window #s {\n"
        " column {\n"
        "  stack #pages shown=1 {\n"
        "   column #a {\n"
        "    custom #wide\n"
        "   }\n"
        "   column #b {\n"
        "    custom #tall\n"
        "    button #go \"Go\"\n"
        "   }\n"
        "  }\n"
        " }\n"
        "}\n";
    b = uui_builder_parse("s.ui", STACK, sizeof STACK - 1, FNAMES, 2);
    struct uui_stack *st = uui_builder_widget(b, "s", "pages", "stack");
    struct uui_custom *wide = uui_builder_widget(b, "s", "wide", "custom");
    struct uui_custom *tall = uui_builder_widget(b, "s", "tall", "custom");
    struct uui_item *pa = uui_builder_item(b, "s", "a"), *pb = uui_builder_item(b, "s", "b");
    e = uui_builder_error(b);
    utest_checkf(!e, "a stack builds (%s)", e ? e : "ok");
    if (!e) {
        utest_check(st->count == 2 && st->current == 1 && pa->hidden && !pb->hidden, "shown= picks the page; the rest are hidden");
        wide->w = 300; wide->h = 10;
        tall->w = 20; tall->h = 200;
        int nw = 0, nh = 0;
        uui_stack_ops.natural_size(st, &nw, &nh);
        utest_checkf(nw == 300 && nh >= 200, "its size is the LARGEST page's, shown or not (%dx%d)", nw, nh);
        uui_stack_ops.set_geometry(st, 5, 7, 400, 250);
        utest_check(wide->x == 5 && wide->y == 7 && tall->x == 5 && tall->y == 7,
                    "every page is placed in its rect, so a switch moves nothing");
        utest_check(uui_stack_show(st, pa) && st->current == 0 && !pa->hidden && pb->hidden &&
                    !uui_stack_show(st, uui_builder_item(b, "s", "go")), "show() by the page's item, and only a page");
    }
    uui_builder_free(b);
    // Tab and a key stop at what is on the shown page only.
    static const char PAGES[] =
        "window #p focus=\"x y\" autofocus {\n"
        " column #body {\n"
        "  stack #st {\n"
        "   column #pa {\n"
        "    dropdown #x {\n"
        "     item \"One\" id=one\n"
        "     item \"Two\" id=two\n"
        "    }\n"
        "   }\n"
        "   column #pb {\n"
        "    dropdown #y {\n"
        "     item \"Three\"\n"
        "    }\n"
        "   }\n"
        "  }\n"
        "  label #outside\n"
        " }\n"
        "}\n";
    b = uui_builder_parse("p.ui", PAGES, sizeof PAGES - 1, FNAMES, 2);
    struct uapp_desc pd = { 0 };
    struct uui_stack *ps = uui_builder_widget(b, "p", "st", "stack");
    struct uui_item *pb_page = uui_builder_page(b, "p", "st", "pb");
    struct uui_item *body = uui_builder_item(b, "p", "body");
    e = uui_builder_error(b);
    utest_checkf(!e && uui_builder_window(b, "p", &pd), "a stack in a focus ring builds (%s)", e ? e : "ok");
    if (!e) {
        struct uui_focus *f = pd.focus;
        utest_check(body && body->widget == pd.layout, "the window's root is looked up by its #name (it outlives the build)");
        utest_check(f->count == 2 && f->page && f->page[0] && f->page[1] && f->current == 0, "the ring knows each control's page");
        uui_focus_next(f);
        utest_check(f->current == 0, "Tab skips a control on a hidden page");
        utest_check(uui_stack_show(ps, pb_page) && uui_focus_key(f, KEY_ARROW_DOWN, 0) == 0,
                    "a key is not delivered to a control whose page went away");
        uui_focus_next(f);
        utest_check(f->current == 1, "...and Tab reaches the page shown now");
        utest_check(uui_builder_choice(b, "p", "x", "two") == 1 && !strcmp(uui_builder_choice_id(b, "p", "x", 0), "one"),
                    "a choice is found by its id, and an index gives its id");
        utest_check(!uui_builder_page(b, "p", "st", "outside") && strstr(uui_builder_error(b), "not a page of #st"),
                    "a page lookup refuses an item outside the stack");
    }
    uui_builder_free(b);
    b = uui_builder_parse("p.ui", PAGES, sizeof PAGES - 1, FNAMES, 2);
    utest_check(uui_builder_choice(b, "p", "x", "three") < 0 && strstr(uui_builder_error(b), "id=three"),
                "an id the app needs and the file lacks is an error");
    uui_builder_free(b);
    build_rejects("window #w {\n column {\n  stack {\n   column hidden {\n    label\n   }\n  }\n }\n}\n",
                  "say shown=", "hidden on a stack page is refused: the stack decides");
    build_rejects("window #w {\n column {\n  dropdown {\n   item \"a\" id=k\n   item \"b\" id=k\n  }\n }\n}\n",
                  "two items have id=k", "two items with one id are refused");

    build_rejects("window #w {\n column {\n  stack {\n   label \"a\" width=3\n  }\n }\n}\n", "not in a row",
                  "a page takes no width=: the stack sizes it");
    build_rejects("window #w {\n column {\n  stack {\n   button #go \"A\"\n   button \"B\" command=go\n  }\n }\n}\n",
                  "both answer", "duplicate ids are found inside a stack too");

    build_rejects("window #w {\n column {\n  dropdown {\n   choice \"x\"\n  }\n }\n}\n", "holds item",
                  "a dropdown holds only items");
    build_rejects("window #w {\n column {\n  dropdown selected=3 {\n   item \"x\"\n  }\n }\n}\n", "outside",
                  "selected= past the items is refused");
    build_rejects("window #w {\n column {\n  colourpick #nope\n }\n}\n", "names table",
                  "a colourpick whose ids have no base in the app's table is refused");
    build_rejects("window #w autofocus {\n column {\n  label\n }\n}\n", "autofocus needs",
                  "autofocus with no focus= is refused");
}

static void builder(void) {
    struct uui_builder *b = uui_builder_parse("t.ui", GOOD, sizeof GOOD - 1, NAMES, NN);
    utest_checkf(b && !uui_builder_error(b), "a window with every type builds (%s)",
                 uui_builder_error(b) ? uui_builder_error(b) : "ok");
    struct uapp_desc desc = { .title = "kept?" };
    struct uui_toolbar *tb = uui_builder_widget(b, "w", "tb", "toolbar");
    struct uui_table *t = uui_builder_widget(b, "w", "list", "table");
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
    utest_check(!desc.focus || desc.focus->current < 0, "without autofocus the ring starts on nothing");
    utest_check(!uui_builder_widget(b, "w", "side", "button") && uui_builder_error(b) &&
                strstr(uui_builder_error(b), "is a label; the app expects a button"),
                "a lookup of the wrong type is an error, not a cast");
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
    build_rejects("window #w {\n row margin=0 {\n  label\n }\n}\n", "0 means the default",
                  "margin=0 is refused, not turned into the default spacing");
    build_rejects("window #w resizable=no {\n row {\n  label\n }\n}\n", "is a flag", "a window flag given a value is refused");
    build_rejects("window #w {\n row {\n  button \"A\" command=go\n  button #go \"B\"\n }\n}\n",
                  "both answer to the app", "two widgets with one id are refused at load, not by uapp at start");
    build_rejects("window #w {\n row {\n  label {\n   label\n  }\n }\n}\n", "no children", "a label takes no children");
    build_rejects("window #w {\n row {\n  toolbar {\n   item \"x\" command=go\n  }\n }\n}\n", "icon=",
                  "a toolbar item needs an icon");

    b = uui_builder_parse("t.ui", GOOD, sizeof GOOD - 1, NAMES, NN);
    utest_check(!uui_builder_widget(b, "nowin", "tb", 0) && strstr(uui_builder_error(b), "#nowin"),
                "asking for a window the file lacks is an error");
    uui_builder_free(b);
}

// Spacing in lines and characters, bold labels, accent checkboxes and the
// wizard -- what the Windows installer's file needed.
static const char WIZ[] =
    "window #z {\n"
    "    column margin=0.5lh gap=1ch {\n"
    "        wizard #wz heading=\"Set up\" skip=\"Cancel\" fill {\n"
    "            step \"One\" note=\"First\" {\n"
    "                column #first gap=0.25lh {\n"
    "                    label #big \"Hi\" bold\n"
    "                    checkbox #opt \"Opt\" checked accent\n"
    "                }\n"
    "            }\n"
    "            step \"Two\" next=\"Go\" {\n"
    "                column {\n"
    "                    button \"Go\" #go\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    dialog #ask\n"
    "}\n";

// A top-level menu: the same rows the C's UUI_MENU arrays gave, a file of
// menus alone builds, and each mistake is refused at its line.
static const char MENUS[] =
    "menu #bar {\n"
    "    menu \"File\" {\n"
    "        item \"Go\" command=go accel=\"Ctrl+G\"\n"
    "        separator\n"
    "        menu \"More\" command=help {\n"
    "            item \"Stop\" command=stop icon=tb-delete role=danger\n"
    "        }\n"
    "    }\n"
    "}\n"
    "menu #ctx {\n"
    "    item \"Go\" command=go icon=tb-open strip\n"
    "    item \"Help\" command=help\n"
    "}\n";

static void menus(void) {
    struct uui_builder *b = uui_builder_parse("m.ui", MENUS, sizeof MENUS - 1, NAMES, NN);
    int n = -1, nc = -1;
    const struct uui_menu_item *bar = uui_builder_menu(b, "bar", &n);
    const struct uui_menu_item *ctx = uui_builder_menu(b, "ctx", &nc);
    utest_checkf(b && !uui_builder_error(b) && bar && n == 1 && ctx && nc == 2,
                 "a file of menus alone builds, each found by name (%s)",
                 uui_builder_error(b) ? uui_builder_error(b) : "ok");
    if (!bar || !ctx) { uui_builder_free(b); return; }
    const struct uui_menu_item *file = bar[0].sub;
    utest_check(!strcmp(bar[0].label, "File") && bar[0].sub_count == 3 && bar[0].code == 0,
                "a title is a submenu of its rows, with no command of its own");
    utest_check(!strcmp(file[0].label, "Go") && file[0].code == CMD_GO && !strcmp(file[0].accel, "Ctrl+G") &&
                !file[0].sub, "an item carries its command's code and its accel text");
    utest_check(!file[1].label && !file[1].code && !file[1].sub, "a separator is the all-zero row");
    utest_check(!strcmp(file[2].label, "More") && file[2].code == CMD_HELP && file[2].sub_count == 1 &&
                file[2].sub[0].code == CMD_STOP && !strcmp(file[2].sub[0].icon, "tb-delete") &&
                file[2].sub[0].tint == UTHEME_ACT_DANGER,
                "a nested menu keeps its command (for greying), its rows, an icon and its role");
    utest_check(ctx[0].style == UUI_MIS_STRIP && !ctx[1].style && ctx[1].code == CMD_HELP,
                "a context menu's leading strip button, then an ordinary row");
    uui_builder_menu(b, "nope", &n);
    utest_check(uui_builder_error(b) && strstr(uui_builder_error(b), "menu #nope") && n == 0,
                "asking for a menu the file lacks is the error");
    uui_builder_free(b);

    build_rejects("menu {\n    item \"Go\" command=go\n}\n", "needs a #name", "a menu without a #name");
    build_rejects("menu #m\n", "holds nothing", "an empty menu");
    build_rejects("menu #m \"Title\" {\n    item \"Go\" command=go\n}\n", "takes no text",
                  "a top menu with text");
    build_rejects("menu #m {\n    item \"Go\" command=gone\n}\n", "no command 'gone'",
                  "an item whose command the app lacks");
    build_rejects("menu #m {\n    item \"Go\"\n}\n", "needs command=", "an item with no command");
    build_rejects("menu #m {\n    item command=go\n}\n", "needs its text", "an item with no text");
    build_rejects("menu #m {\n    item \"Go\" #go command=go\n}\n", "not by a #name", "a named row");
    build_rejects("menu #m {\n    button \"Go\" command=go\n}\n", "not button", "a widget in a menu");
    build_rejects("menu #m {\n    item \"Go\" command=go acel=x\n}\n", "acel", "a misspelt attribute");
    build_rejects("menu #m {\n    separator command=go\n}\n", "command", "a separator with attributes");
    build_rejects("menu #m {\n    item \"Go\" command=go role=nav\n}\n", "give it icon=",
                  "a role with no icon to colour");
    build_rejects("menu #m {\n    item \"Go\" command=go strip\n}\n", "item with icon=", "a strip button with no icon");
    build_rejects("menu #m {\n    item \"Go\" command=go\n    item \"Help\" command=help icon=tb-help strip\n}\n",
                  "come first", "a strip button after an ordinary row");
    build_rejects("menu #m {\n    item \"Go\" command=go\n}\nmenu #m {\n    item \"Go\" command=go\n}\n",
                  "two menus are called #m", "two menus with one name");
    build_rejects("thing #t\n", "windows, menus and options, not thing", "a top-level node that is none of them");
    // One deeper than the widget opens: refused at load, not a submenu that never shows.
    char deep[1024];
    int at = snprintf(deep, sizeof deep, "menu #m {\n");
    for (int i = 0; i < UUI_MENU_MAX_DEPTH; i++) at += snprintf(deep + at, sizeof deep - (size_t)at, "menu \"L%d\" {\n", i);
    at += snprintf(deep + at, sizeof deep - (size_t)at, "item \"Go\" command=go\n");
    for (int i = 0; i <= UUI_MENU_MAX_DEPTH; i++) at += snprintf(deep + at, sizeof deep - (size_t)at, "}\n");
    build_rejects(deep, "nest at most", "menus nested past the widget's depth");
    // ...and exactly at it, they build.
    at = snprintf(deep, sizeof deep, "menu #m {\n");
    for (int i = 0; i < UUI_MENU_MAX_DEPTH - 1; i++) at += snprintf(deep + at, sizeof deep - (size_t)at, "menu \"L%d\" {\n", i);
    at += snprintf(deep + at, sizeof deep - (size_t)at, "item \"Go\" command=go\n");
    for (int i = 0; i < UUI_MENU_MAX_DEPTH; i++) at += snprintf(deep + at, sizeof deep - (size_t)at, "}\n");
    b = uui_builder_parse("t.ui", deep, strlen(deep), NAMES, NN);
    utest_checkf(!uui_builder_error(b), "menus nested exactly %d deep build (%s)", UUI_MENU_MAX_DEPTH,
                 uui_builder_error(b) ? uui_builder_error(b) : "ok");
    uui_builder_free(b);
}

// The types and layout words Log Viewer's window needed: a menu bar of a
// menu declared BELOW the window, segments, a status bar, a log list, a
// fixed space, a centred child, one control high, and margins that differ.
static const char BARS[] =
    "window #w {\n"
    "    column margin=none gap=none {\n"
    "        menubar #mb menu=m fill-w\n"
    "        row margin=\"medium large\" gap=none fill-w {\n"
    "            segmented #seg selected=1 {\n"
    "                item \"One\" id=one\n"
    "                item \"Two\" id=two\n"
    "            }\n"
    "            space size=medium\n"
    "            textbox #field fill-w height=control center\n"
    "        }\n"
    "        loglist #log fill\n"
    "        statusbar #sb fill-w\n"
    "    }\n"
    "}\n"
    "menu #m {\n"
    "    menu \"File\" {\n"
    "        item \"Go\" command=go\n"
    "    }\n"
    "}\n";

static void bars(void) {
    struct uui_builder *b = uui_builder_parse("b.ui", BARS, sizeof BARS - 1, NAMES, NN);
    struct uui_menubar *mb = uui_builder_widget(b, "w", "mb", "menubar");
    struct uui_segmented *seg = uui_builder_widget(b, "w", "seg", "segmented");
    struct uui_textbox *field = uui_builder_widget(b, "w", "field", "textbox");
    struct uui_loglist *log = uui_builder_widget(b, "w", "log", "loglist");
    struct uui_statusbar *sb = uui_builder_widget(b, "w", "sb", "statusbar");
    struct uui_item *fit = uui_builder_item(b, "w", "field");
    struct uapp_desc desc = { 0 };
    utest_checkf(b && !uui_builder_error(b) && mb && seg && field && log && sb && uui_builder_window(b, "w", &desc),
                 "a window of the new types builds, its menubar naming a menu declared below it (%s)",
                 uui_builder_error(b) ? uui_builder_error(b) : "ok");
    if (uui_builder_error(b)) { uui_builder_free(b); return; }
    utest_check(mb->count == 1 && !strcmp(mb->items[0].label, "File") && mb->items[0].sub[0].code == CMD_GO,
                "the menubar holds the named menu's rows");
    utest_check(seg->count == 2 && seg->selected == 1 && !strcmp(seg->options[1], "Two") &&
                uui_builder_choice(b, "w", "seg", "two") == 1, "segments are items, known by id=, selected= read");
    utest_check(log->set == 0 && log->selected == -1, "a log list starts with no lines: the program gives it its set");
    struct uui_layout *root = desc.layout, *row = root->items[1].widget;
    utest_check(root->margin == UTHEME_STEP_NONE && root->margin_unit == UUI_STEP && uui_layout_margin(root) == 0 &&
                uui_layout_gap(root) == 0, "none is an explicit zero, where 0 would be the default");
    utest_check(uui_layout_margin(row) == utheme_pad() && uui_layout_margin_x(row) == 2 * utheme_pad() &&
                uui_layout_margin_x(root) == 0, "two margins: top and bottom, then the sides; one is every side");
    utest_check((fit->flags & UUI_CONTROL_H) && (fit->flags & UUI_CENTER), "height=control and center are flags");

    // Placed: the row's children start two pads in and one pad down, the
    // space is one pad wide, the field is one control high and centred.
    uui_layout_run(root, 0, 0, 400, 300);
    int pad = utheme_pad(), sw, sh;
    uui_segmented_natural_size(seg, &sw, &sh);
    int rh = row->h - 2 * pad;
    utest_checkf(seg->x == 2 * pad && seg->y == row->y + pad && field->x == 2 * pad + sw + pad &&
                 field->w == 400 - 2 * pad - field->x && field->h == utheme_control_h() &&
                 field->y == row->y + pad + (rh - utheme_control_h()) / 2,
                 "placed: sides two pads, top one, a pad of space, the field a control high and centred "
                 "(seg %d,%d field %d,%d %dx%d row y %d h %d)", seg->x, seg->y, field->x, field->y, field->w,
                 field->h, row->y, row->h);
    uui_builder_free(b);

    build_rejects("window #w {\n column {\n  menubar menu=nope\n }\n}\n", "menu #nope", "a menubar naming no menu");
    build_rejects("window #w {\n column {\n  menubar\n }\n}\n", "needs menu=", "a menubar with no menu=");
    build_rejects("window #w {\n column {\n  space\n }\n}\n", "needs size=", "a space with no size");
    build_rejects("window #w {\n grid {\n  space size=medium\n }\n}\n", "row or a column", "a space in a grid");
    build_rejects("window #w {\n grid {\n  label center\n }\n}\n", "row or a column", "center in a grid");
    build_rejects("window #w {\n column margin=\"small medium large\" {\n  label\n }\n}\n", "or two",
                  "three margins");
    build_rejects("window #w {\n column margin=\"small huge\" {\n  label\n }\n}\n", "margin=huge",
                  "a bad second margin is named");
    build_rejects("window #w {\n row {\n  label height=2\n }\n}\n", "height=control works",
                  "height in cells in a row");
    build_rejects("window #w {\n row {\n  segmented selected=0\n }\n}\n", "there are none",
                  "selected= on a segmented with no items");
    build_rejects("window #w {\n row {\n  dropdown selected=0\n }\n}\n", "there are none",
                  "selected= on a dropdown with no items");
    build_rejects("window #w {\n column margin=\" \" {\n  label\n }\n}\n", "is empty", "an empty margin");

    // Spaces between the two words are a separator, however many.
    static const char SPACED[] = "window #w {\n column margin=\" small   large \" {\n  label\n }\n}\n";
    b = uui_builder_parse("t.ui", SPACED, sizeof SPACED - 1, NAMES, NN);
    struct uapp_desc sd = { 0 };
    utest_checkf(!uui_builder_error(b) && uui_builder_window(b, "w", &sd) &&
                 sd.layout->margin == UTHEME_STEP_SMALL && sd.layout->margin_x == UTHEME_STEP_LARGE,
                 "two margins with runs of spaces read as two (%s)", uui_builder_error(b) ? uui_builder_error(b) : "ok");
    uui_builder_free(b);

    // The FIRST error in the file is the one kept, though menus are named first.
    b = uui_builder_parse("t.ui", "window #w {\n column {\n  label fil\n }\n}\nmenu #m {\n item \"Go\" command=gone\n}\n",
                          strlen("window #w {\n column {\n  label fil\n }\n}\nmenu #m {\n item \"Go\" command=gone\n}\n"),
                          NAMES, NN);
    utest_checkf(uui_builder_error(b) && !strncmp(uui_builder_error(b), "t.ui:3:", 7),
                 "a window's error above a menu's is the one reported (%s)", uui_builder_error(b));
    uui_builder_free(b);
}

// uapp paints a window's layout only when its router does not already
// draw it: a .ui window's router covers its layout, and a router holding
// only some of a layout's widgets does not.
// A toolbar hosting controls (UUI_TB_CONTROL, QToolBar::addWidget()):
// two half-lines from a button, one between two controls, at most a
// button high and centred on it, narrowed before the right-hand group,
// and reached by the router as the strip's children.
static const char HOSTED[] =
    "window #w {\n"
    "    column margin=none gap=none {\n"
    "        toolbar #tb fill-w {\n"
    "            item \"Go\" icon=tb-play command=go\n"
    "            item \"Stop\" icon=tb-stop command=stop\n"
    "            dropdown #a {\n"
    "                item \"Sixteen megabytes\"\n"
    "            }\n"
    "            dropdown #b {\n"
    "                item \"A long volume name to narrow\"\n"
    "            }\n"
    "            item icon=tb-help command=help end\n"
    "        }\n"
    "        meter #m\n"
    "        chart #c \"THROUGHPUT\" fill\n"
    "    }\n"
    "}\n";

static void hosted(void) {
    // A FONT, or every spacing below is 0 and the checks hold whatever the strip does.
    utest_checkf(ugfx_font_init() && ugfx_char_h() > 0, "the session font is loaded (line %d)", ugfx_char_h());
    struct uui_builder *b = uui_builder_parse("h.ui", HOSTED, sizeof HOSTED - 1, NAMES, NN);
    struct uui_toolbar *tb = uui_builder_widget(b, "w", "tb", "toolbar");
    struct uui_dropdown *da = uui_builder_widget(b, "w", "a", "dropdown");
    struct uui_dropdown *db = uui_builder_widget(b, "w", "b", "dropdown");
    struct uui_chart *ch = uui_builder_widget(b, "w", "c", "chart");
    uui_builder_widget(b, "w", "m", "meter");
    struct uapp_desc desc = { 0 };
    utest_checkf(b && !uui_builder_error(b) && tb && da && db && ch && uui_builder_window(b, "w", &desc),
                 "a toolbar hosting two dropdowns builds, each named; a meter and a chart too (%s)",
                 uui_builder_error(b) ? uui_builder_error(b) : "ok");
    if (uui_builder_error(b)) { uui_builder_free(b); return; }
    utest_check(tb->count == 5 && tb->control_count == 2 && (tb->items[2].flags & UUI_TB_CONTROL) &&
                tb->items[2].code == 0 && tb->items[3].code == 1 && tb->controls[1].widget == db &&
                !strcmp(ch->label, "THROUGHPUT"), "the strip's items index its controls; the chart has its heading");
    int n = 0;
    utest_check(uui_toolbar_ops.children(tb, &n) == tb->controls && n == 2, "the router reaches them as its children");

    uui_layout_run(desc.layout, 0, 0, 900, 400);
    int g = ugfx_char_h() / 2, sx, sy, sw, sh, ax, ay, aw, ah, bx, by, bw, bh;
    uui_toolbar_item_rect(tb, 1, &sx, &sy, &sw, &sh);
    uui_toolbar_item_rect(tb, 2, &ax, &ay, &aw, &ah);
    uui_toolbar_item_rect(tb, 3, &bx, &by, &bw, &bh);
    utest_checkf(ax == sx + sw + 2 * g && bx == ax + aw + g && da->x == ax && db->x == bx && da->w == aw,
                 "two half-lines after a button, one between controls (stop %d+%d, a %d, b %d)", sx, sw, ax, bx);
    utest_checkf(da->h <= sh && da->y == sy + (sh - da->h) / 2, "at most a button high, centred on it (%d in %d)",
                 da->h, sh);

    // Narrow: the second control gives way before the right-hand group.
    uui_layout_run(desc.layout, 0, 0, ax + aw + g + ugfx_char_h() * 4 + 2 * g + 40, 400);
    int rx, ry, rw, rh;
    uui_toolbar_item_rect(tb, 3, &bx, &by, &bw, &bh);
    uui_toolbar_item_rect(tb, 4, &rx, &ry, &rw, &rh);
    utest_checkf(bw < db->w + 1 && bw >= ugfx_char_h() * 3 && bx + bw <= rx - 2 * g,
                 "narrowed to fit before the right-hand group, never under three lines (%d at %d, group at %d)",
                 bw, bx, rx);
    uui_builder_free(b);

    build_rejects("window #w {\n column {\n  toolbar {\n   dropdown width=4\n  }\n }\n}\n", "not in a row",
                  "a hosted control takes no width=: the strip sizes it");
    build_rejects("window #w {\n column {\n  toolbar {\n   dropdown fill-w\n  }\n }\n}\n", "sizes what it holds",
                  "...nor fill-w, which it would never read");

    // A control the FILE hides stays hidden through a layout, and takes no room.
    static const char HID[] =
        "window #w {\n column margin=none gap=none {\n  toolbar #tb fill-w {\n"
        "   item \"Stop\" icon=tb-stop command=stop\n   dropdown #a hidden\n   dropdown #b\n"
        "   item icon=tb-help command=help end\n  }\n }\n}\n";
    b = uui_builder_parse("h.ui", HID, sizeof HID - 1, NAMES, NN);
    struct uapp_desc hd = { 0 };
    tb = uui_builder_widget(b, "w", "tb", "toolbar");
    struct uui_item *ai = uui_builder_item(b, "w", "a");
    utest_check(!uui_builder_error(b) && uui_builder_window(b, "w", &hd), "a strip with a hidden control builds");
    if (!uui_builder_error(b)) {
        uui_layout_run(hd.layout, 0, 0, 900, 200);
        uui_toolbar_item_rect(tb, 0, &sx, &sy, &sw, &sh);
        uui_toolbar_item_rect(tb, 2, &bx, &by, &bw, &bh);
        utest_checkf(ai->hidden && bx == sx + sw + 2 * g, "it stays hidden and the next control takes its place "
                     "(b at %d, stop ends %d)", bx, sx + sw);
    }
    uui_builder_free(b);
}

// A FLOATING widget beside the root: routed and drawn, never laid out --
// the program places it (Image Viewer's slideshow transport).
static const char FLOAT[] =
    "window #w {\n"
    "    column {\n"
    "        image #pic fill\n"
    "        thumbstrip #strip fill-w\n"
    "    }\n"
    "    transport #tp floating hidden\n"
    "}\n";

static void floating(void) {
    struct uui_builder *b = uui_builder_parse("f.ui", FLOAT, sizeof FLOAT - 1, NAMES, NN);
    struct uui_transport *tp = uui_builder_widget(b, "w", "tp", "transport");
    struct uui_image *pic = uui_builder_widget(b, "w", "pic", "image");
    struct uui_item *ti = uui_builder_item(b, "w", "tp");
    uui_builder_widget(b, "w", "strip", "thumbstrip");
    struct uapp_desc desc = { 0 };
    utest_checkf(b && !uui_builder_error(b) && tp && pic && uui_builder_window(b, "w", &desc),
                 "a floating transport beside a column of an image and a thumbstrip builds (%s)",
                 uui_builder_error(b) ? uui_builder_error(b) : "ok");
    if (uui_builder_error(b)) { uui_builder_free(b); return; }
    struct uui_layout *root = desc.layout;
    int routed = 0, laid = 0;
    for (int i = 0; i < desc.widget_count; i++) routed |= desc.widgets[i].widget == tp;
    for (int i = 0; i < root->count; i++) laid |= root->items[i].widget == tp;
    utest_check(routed && !laid && ti->hidden, "it is routed beside the root, not laid out in it, and starts hidden");
    struct uui_router r;
    uui_router_init(&r, desc.widgets, desc.widget_count);
    utest_check(uui_router_covers_layout(&r, root), "the router still covers the layout it holds");
    uui_builder_free(b);
    build_rejects("window #w {\n column {\n  transport floating\n }\n}\n", "floating",
                  "floating inside the tree is refused: it is only beside the root");
    build_rejects("window #w {\n column {\n  label\n }\n transport floating fill-w\n}\n", "places a floating",
                  "...and a floating widget takes no fill: nothing lays it out");
}

static void paints_once(void) {
    struct uui_builder *b = uui_builder_parse("c.ui", GOOD, sizeof GOOD - 1, NAMES, NN);
    struct uapp_desc desc = { 0 };
    utest_check(b && uui_builder_window(b, "w", &desc), "the window is filled");
    struct uui_router r;
    uui_router_init(&r, desc.widgets, desc.widget_count);
    utest_check(uui_router_covers_layout(&r, desc.layout), "a .ui window's router covers its layout");
    struct uui_layout *root = desc.layout;
    struct uui_item copy[8];
    int n = root->count < 8 ? root->count : 8;
    memcpy(copy, root->items, sizeof copy[0] * (size_t)n);
    uui_router_init(&r, copy, n);
    utest_check(n == root->count && uui_router_covers_layout(&r, root),
                "...and so does a separate array of the same widgets");
    uui_router_init(&r, copy, n - 1);
    utest_check(!uui_router_covers_layout(&r, root), "a router missing one of them does not: the layout is painted");
    uui_router_init(&r, copy, 0);
    utest_check(!uui_router_covers_layout(&r, root), "nor does an empty one");
    uui_builder_free(b);
}

static void wizard(void) {
    struct uui_builder *b = uui_builder_parse("t.ui", WIZ, sizeof WIZ - 1, NAMES, NN);
    struct uui_wizard *wz = uui_builder_widget(b, "z", "wz", "wizard");
    struct uui_label *big = uui_builder_widget(b, "z", "big", "label");
    struct uui_checkbox *opt = uui_builder_widget(b, "z", "opt", "checkbox");
    struct uui_button *go = uui_builder_widget(b, "z", "go", "button");
    const char *e = uui_builder_error(b);
    utest_checkf(!e && wz && big && opt && go, "a wizard's pages are built, their names found (%s)", e ? e : "ok");
    if (e || !wz) { uui_builder_free(b); return; }
    utest_check(wz->d.count == 2 && !strcmp(wz->d.heading, "Set up") && !strcmp(wz->d.skip, "Cancel") && !wz->d.finish,
                "the wizard reads heading=, skip= and finish=");
    utest_check(!strcmp(wz->d.steps[0].title, "One") && !strcmp(wz->d.steps[0].note, "First") && !wz->d.steps[0].next &&
                !strcmp(wz->d.steps[1].title, "Two") && !wz->d.steps[1].note && !strcmp(wz->d.steps[1].next, "Go"),
                "each step reads its title, note= and next=");
    utest_check(wz->d.steps[1].page.ops == &uui_layout_ops && go->code == CMD_GO, "a step's page is its one child");
    utest_check(big->bold && !big->font && opt->checked && opt->accent, "bold, checked and accent are read");
    utest_check(uui_builder_item(b, "z", "first") == uui_wizard_page(wz, 0),
                "a page's #name is the item the wizard shows and hides, not the step's copy");
    utest_check(uui_builder_wizard(b, "z", "wz", 2) == wz && !uui_builder_error(b), "the program's step count is met");

    struct uapp_desc desc = { 0 };
    utest_check(uui_builder_window(b, "z", &desc), "the window is filled");
    struct uui_layout *root = desc.layout;
    struct uui_dialog *ask = uui_builder_widget(b, "z", "ask", "dialog");
    utest_check(ask && desc.widget_count == 2 && desc.widgets[0].widget == root &&
                desc.widgets[1].ops == &uui_dialog_ops && desc.widgets[1].widget == ask && root->count == 1,
                "a dialog is routed beside the root, and not laid out");
    utest_check(root->margin == 50 && root->margin_unit == UUI_LH && root->gap == 100 && root->gap_unit == UUI_CH,
                "0.5lh and 1ch are kept as hundredths of their unit");
    utest_check(uui_layout_margin(root) == ugfx_char_h() / 2 && uui_layout_gap(root) == ugfx_char_advance('n'),
                "...and resolve at layout to what the C wrote: half a line, one character");
    struct uui_layout *page0 = wz->d.steps[0].page.widget;
    utest_check(page0->gap == 25 && uui_layout_gap(page0) == ugfx_char_h() * 25 / 100, "two decimals are exact");
    uui_builder_free(b);

    // Named steps: kept as the step, resolved at layout to the theme's pad ladder.
    static const char STEPS_SRC[] =
        "window #s {\n    column margin=large gap=small {\n        row gap=medium {\n            label\n        }\n    }\n}\n";
    b = uui_builder_parse("s.ui", STEPS_SRC, sizeof STEPS_SRC - 1, NAMES, NN);
    struct uapp_desc sd = { 0 };
    utest_checkf(b && !uui_builder_error(b) && uui_builder_window(b, "s", &sd), "named steps build (%s)",
                 uui_builder_error(b) ? uui_builder_error(b) : "ok");
    struct uui_layout *sroot = sd.layout, *srow = sroot ? sroot->items[0].widget : 0;
    utest_check(sroot && sroot->margin == UTHEME_STEP_LARGE && sroot->margin_unit == UUI_STEP &&
                sroot->gap == UTHEME_STEP_SMALL && srow->gap == UTHEME_STEP_MEDIUM && srow->gap_unit == UUI_STEP,
                "small, medium and large are kept as theme steps");
    utest_checkf(sroot && uui_layout_margin(sroot) == 2 * utheme_pad() && uui_layout_gap(sroot) == utheme_pad() / 2 &&
                 uui_layout_gap(srow) == utheme_pad() && utheme_pad() > 0,
                 "...and resolve at layout to two pads, half a pad and one (pad %d)", utheme_pad());
    uui_builder_free(b);
    build_rejects("window #w {\n column gap=huge {\n  label\n }\n}\n", "small, medium, large or none", "an unknown step is refused");

    build_rejects("window #w {\n column gap=1.5 {\n  label\n }\n}\n", "not a spacing", "pixels take no decimals");
    build_rejects("window #w {\n column gap=0.125lh {\n  label\n }\n}\n", "not a spacing", "three decimals are refused");
    build_rejects("window #w {\n column gap=2em {\n  label\n }\n}\n", "not a spacing", "an unknown unit is refused");
    build_rejects("window #w {\n column gap=lh {\n  label\n }\n}\n", "not a spacing", "a unit with no number is refused");
    build_rejects("window #w {\n column gap=0lh {\n  label\n }\n}\n", "0 means the default", "0lh is the default's 0");
    b = uui_builder_parse("t.ui", WIZ, sizeof WIZ - 1, NAMES, NN);
    utest_check(!uui_builder_wizard(b, "z", "wz", 3) && strstr(uui_builder_error(b), "t.ui:3:") &&
                strstr(uui_builder_error(b), "2 steps; the program has 3"),
                "a wizard whose steps the program does not match is an error at its line");
    uui_builder_free(b);
    build_rejects("window #w {\n column {\n  dialog\n }\n}\n", "beside the window", "a dialog in a column is refused");
    build_rejects("window #w {\n column {\n wizard {\n  step \"A\" #a {\n   label\n  }\n }\n}\n}\n", "takes no #name",
                  "a #name on a step is refused, not ignored");
    build_rejects("window #w {\n column {\n wizard {\n  label\n }\n}\n}\n", "holds steps", "a wizard holds only steps");
    build_rejects("window #w {\n column {\n wizard {\n  step {\n   label\n  }\n }\n}\n}\n", "its title", "a step needs a title");
    build_rejects("window #w {\n column {\n wizard {\n  step \"A\" {\n   label\n   label\n  }\n }\n}\n}\n", "one page",
                  "a step holds one page");
    build_rejects("window #w {\n column {\n wizard {\n  step \"A\" nxt=\"B\" {\n   label\n  }\n }\n}\n}\n", "nxt",
                  "a misspelt step attribute is refused");
    static char many[2048];
    int n = snprintf(many, sizeof many, "window #w {\n column {\n wizard {\n");
    for (int i = 0; i <= UUI_WIZARD_STEPS_MAX; i++)
        n += snprintf(many + n, sizeof many - (size_t)n, "  step \"S\" {\n   label\n  }\n");
    snprintf(many + n, sizeof many - (size_t)n, " }\n }\n}\n");
    build_rejects(many, "at most", "a wizard past the widget's step limit is refused, not cut short");
}

// The shipped file, from where Crash Reports reads it: it builds, and has
// every name the app looks up, of its type. CR[] mirrors the app's NAMES by
// hand, so this catches the FILE changing; the app's own table is
// exercised by crashtest_test and crashview_test, which open all three faces.
static void shipped(void) {
    static const struct uui_builder_name CR[] = {
        { "reports", 1 }, { "tb", 2 }, { "details", 3 }, { "report", 4 },
        { "open", 100 }, { "copy", 101 }, { "delete", 102 }, { "reopen", 103 },
        { "all", 104 }, { "close", 105 }, { "notepad", 106 }, { "show", 107 },
    };
    struct uui_builder *b = uui_builder_load("crashreports", CR, (int)(sizeof CR / sizeof CR[0]));
    uui_builder_widget(b, "list", "reports", "table");
    uui_builder_widget(b, "list", "tb", "toolbar");
    uui_builder_widget(b, "list", "details", "label");
    uui_builder_widget(b, "dialog", "body", "label");
    uui_builder_item(b, "dialog", "reopen");
    uui_builder_widget(b, "dialog", "close", "button");
    uui_builder_widget(b, "viewer", "report", "markdown");
    uui_builder_widget(b, "viewer", "tb", "toolbar");
    const char *e = uui_builder_error(b);
    utest_checkf(b && !e, "/usr/share/ui/crashreports.ui builds with every name the app needs (%s)", e ? e : "ok");
    utest_notef("crashreports.ui read and built in %u us", uui_builder_build_us(b));
    uui_builder_free(b);

    static const struct uui_builder_name TE[] = {
        { "toolbar", 1 }, { "themes", 2 }, { "caption_buttons", 3 }, { "title_align", 4 }, { "titlebar_pad", 5 },
        { "corners", 6 }, { "taskbar", 7 }, { "pick", 20 },
        { "save", 1 }, { "saveas", 2 }, { "apply", 3 }, { "revert", 4 },
    };
    static const char *const TE_IDS[][5] = {
        { "caption_buttons", "discs", "rectangles" }, { "title_align", "left", "centred" },
        { "titlebar_pad", "compact", "normal", "roomy", "tall" },
        { "corners", "square", "small", "medium", "round" }, { "taskbar", "dark", "light" },
    };
    b = uui_builder_load("themeedit", TE, (int)(sizeof TE / sizeof TE[0]));
    static const char *const TE_WIDGETS[][2] = {
        { "toolbar", "toolbar" }, { "themes", "dropdown" }, { "roles", "custom" }, { "preview", "themepreview" },
        { "inspector_title", "label" }, { "inspector_desc", "label" }, { "pick", "colourpick" },
        { "contrast", "custom" }, { "status", "label" }, { "pages", "stack" },
    };
    for (unsigned i = 0; i < sizeof TE_WIDGETS / sizeof TE_WIDGETS[0]; i++)
        uui_builder_widget(b, "main", TE_WIDGETS[i][0], TE_WIDGETS[i][1]);
    uui_builder_page(b, "main", "pages", "colour_page");
    uui_builder_page(b, "main", "pages", "frame_page");
    uui_builder_page(b, "main", "pages", "desktop_page");
    for (unsigned i = 0; i < sizeof TE_IDS / sizeof TE_IDS[0]; i++)
        for (int k = 1; k < 5 && TE_IDS[i][k]; k++) uui_builder_choice(b, "main", TE_IDS[i][0], TE_IDS[i][k]);
    e = uui_builder_error(b);
    utest_checkf(b && !e, "/usr/share/ui/themeedit.ui builds with every name, page and choice id the app needs (%s)",
                 e ? e : "ok");
    uui_builder_free(b);

    static const struct uui_builder_name CA[] = {
        { "clear", 'C' }, { "clear_entry", 'E' }, { "percent", '%' }, { "divide", '/' },
        { "seven", '7' }, { "eight", '8' }, { "nine", '9' }, { "multiply", '*' },
        { "four", '4' }, { "five", '5' }, { "six", '6' }, { "minus", '-' },
        { "one", '1' }, { "two", '2' }, { "three", '3' }, { "plus", '+' },
        { "sign", 's' }, { "zero", '0' }, { "point", '.' }, { "equals", '=' },
    };
    b = uui_builder_load("calculator", CA, (int)(sizeof CA / sizeof CA[0]));
    uui_builder_widget(b, "main", "display", "custom");
    for (unsigned i = 0; i < sizeof CA / sizeof CA[0]; i++) uui_builder_widget(b, "main", CA[i].name, "button");
    e = uui_builder_error(b);
    utest_checkf(b && !e, "/usr/share/ui/calculator.ui builds with every key the app needs (%s)", e ? e : "ok");
    uui_builder_free(b);

    static const struct uui_builder_name WI[] = {
        { "src", 1 }, { "browse", 2 }, { "keep_ini", 3 }, { "stop", 4 }, { "open_launcher", 5 }, { "dialog", 6 },
    };
    static const char *const WI_WIDGETS[][2] = {
        { "wizard", "wizard" }, { "src", "textbox" }, { "browse", "button" }, { "found", "custom" },
        { "disk_error", "label" }, { "plan", "custom" }, { "keep_ini", "checkbox" }, { "missing", "label" },
        { "install_line", "label" }, { "bar", "progress" }, { "log", "custom" }, { "stop", "button" },
        { "done_title", "label" }, { "done_text", "label" }, { "done_facts", "custom" },
        { "open_launcher", "button" }, { "dialog", "dialog" },
    };
    b = uui_builder_load("wininstall", WI, (int)(sizeof WI / sizeof WI[0]));
    for (unsigned i = 0; i < sizeof WI_WIDGETS / sizeof WI_WIDGETS[0]; i++)
        uui_builder_widget(b, "main", WI_WIDGETS[i][0], WI_WIDGETS[i][1]);
    struct uui_wizard *wz = uui_builder_wizard(b, "main", "wizard", 4);
    e = uui_builder_error(b);
    utest_checkf(b && !e && wz,
                 "/usr/share/ui/wininstall.ui builds with every name and the program's four steps (%s)", e ? e : "ok");
    uui_builder_free(b);
}

// Log Viewer's menus, against the bar its C declared until logview.ui:
// every title, label, accel and code where it was.
static void shipped_logview(void) {
    enum { SAVE = 1, EXIT, COPY, COPY_ALL, FIND, CLEAR, UNMUTE_ALL, FOLLOW, DETAILS, TIMELINE,
           SHOW_ALL, SHOW_ERR, SHOW_WARN, NEXT_ERR, PREV_ERR, FIRST, LAST };
    static const struct uui_builder_name LV[] = {
        { "save", SAVE }, { "exit", EXIT }, { "copy", COPY }, { "copy_all", COPY_ALL }, { "find", FIND },
        { "clear", CLEAR }, { "unmute_all", UNMUTE_ALL }, { "follow", FOLLOW }, { "details", DETAILS },
        { "timeline", TIMELINE }, { "show_all", SHOW_ALL }, { "show_err", SHOW_ERR }, { "show_warn", SHOW_WARN },
        { "next_err", NEXT_ERR }, { "prev_err", PREV_ERR }, { "first", FIRST }, { "last", LAST },
    };
    static const struct uui_menu_item file[] = {
        UUI_MENU("Save shown lines as...", SAVE, "Ctrl+S"), UUI_MENU_SEP, UUI_MENU("Exit", EXIT, "Alt+F4"),
    };
    static const struct uui_menu_item edit[] = {
        UUI_MENU("Copy line", COPY, "Ctrl+C"), UUI_MENU("Copy shown lines", COPY_ALL, 0), UUI_MENU_SEP,
        UUI_MENU("Find", FIND, "Ctrl+F"), UUI_MENU("Clear filters", CLEAR, 0), UUI_MENU("Unmute all", UNMUTE_ALL, 0),
    };
    static const struct uui_menu_item view[] = {
        UUI_MENU("Follow new lines", FOLLOW, 0), UUI_MENU("Details", DETAILS, 0), UUI_MENU("Timeline", TIMELINE, 0),
        UUI_MENU_SEP, UUI_MENU("All levels", SHOW_ALL, 0), UUI_MENU("Errors only", SHOW_ERR, 0),
        UUI_MENU("Warnings only", SHOW_WARN, 0),
    };
    static const struct uui_menu_item go[] = {
        UUI_MENU("Next error", NEXT_ERR, "F8"), UUI_MENU("Previous error", PREV_ERR, "F7"), UUI_MENU_SEP,
        UUI_MENU("First line", FIRST, "Home"), UUI_MENU("Newest line", LAST, "End"),
    };
    static const struct uui_menu_item want[] = {
        UUI_SUBMENU("File", file), UUI_SUBMENU("Edit", edit), UUI_SUBMENU("View", view), UUI_SUBMENU("Go", go),
    };
    static const struct uui_builder_name LVW[] = {
        { "menu", 101 }, { "toolbar", 102 }, { "source", 103 }, { "show", 104 }, { "from", 105 }, { "search", 106 },
        { "log", 107 }, { "around", 108 }, { "only", 109 }, { "mute", 110 }, { "copyline", 111 }, { "unmute", 112 },
    };
    struct uui_builder_name all[sizeof LV / sizeof LV[0] + sizeof LVW / sizeof LVW[0]];
    memcpy(all, LV, sizeof LV);
    memcpy(all + sizeof LV / sizeof LV[0], LVW, sizeof LVW);
    struct uui_builder *b = uui_builder_load("logview", all, (int)(sizeof all / sizeof all[0]));
    static const char *const LV_WIDGETS[][2] = {
        { "menu", "menubar" }, { "toolbar", "toolbar" }, { "source", "dropdown" }, { "show", "segmented" },
        { "from", "segmented" }, { "search", "textbox" }, { "log", "loglist" }, { "around", "button" },
        { "only", "button" }, { "mute", "button" }, { "copyline", "button" }, { "unmute", "button" },
        { "status", "statusbar" }, { "panel", "custom" },
    };
    for (unsigned i = 0; i < sizeof LV_WIDGETS / sizeof LV_WIDGETS[0]; i++)
        uui_builder_widget(b, "main", LV_WIDGETS[i][0], LV_WIDGETS[i][1]);
    uui_builder_item(b, "main", "actions");
    uui_builder_item(b, "main", "filter");
    int n = 0;
    const struct uui_menu_item *bar = uui_builder_menu(b, "bar", &n);
    const char *e = uui_builder_error(b);
    utest_checkf(b && !e && n == 4, "/usr/share/ui/logview.ui builds its window, with every name the program asks "
                 "for, and its four menus (%s)", e ? e : "ok");
    if (e || n != 4) { uui_builder_free(b); return; }
    int same = 1;
    for (int t = 0; t < 4; t++) {
        same &= !strcmp(bar[t].label, want[t].label) && bar[t].sub_count == want[t].sub_count;
        for (int i = 0; same && i < want[t].sub_count; i++) {
            const struct uui_menu_item *x = &bar[t].sub[i], *y = &want[t].sub[i];
            same &= x->code == y->code && !x->label == !y->label && (!x->label || !strcmp(x->label, y->label)) &&
                    !x->accel == !y->accel && (!x->accel || !strcmp(x->accel, y->accel)) && !x->sub && !x->icon;
            if (!same) utest_notef("differs: %s row %d (%s)", want[t].label, i, y->label ? y->label : "separator");
        }
    }
    utest_check(same, "...row for row the bar logview.c declared: labels, accels, separators and codes");
    uui_builder_free(b);
}

// Disk Mark's window: every name the program asks for, of its type.
static void shipped_diskmark(void) {
    static const struct uui_builder_name DM[] = {
        { "menu", 1 }, { "toolbar", 2 }, { "size", 3 }, { "location", 4 }, { "history", 5 }, { "dialog", 7 },
        { "run", 101 }, { "stop", 102 }, { "copy", 103 }, { "show_history", 104 }, { "exit", 105 },
        { "size_16m", 110 }, { "size_64m", 111 }, { "size_256m", 112 }, { "size_512m", 113 }, { "size_1g", 114 },
        { "custom", 120 }, { "folder", 121 },
    };
    struct uui_builder *b = uui_builder_load("diskmark", DM, (int)(sizeof DM / sizeof DM[0]));
    static const char *const W[][2] = {
        { "menu", "menubar" }, { "toolbar", "toolbar" }, { "size", "dropdown" }, { "location", "dropdown" },
        { "dialog", "dialog" }, { "seq_read", "meter" }, { "seq_write", "meter" }, { "rnd_read", "meter" },
        { "rnd_write", "meter" }, { "cards", "grid" }, { "bottom", "stack" }, { "chart", "chart" },
        { "history", "table" }, { "status", "statusbar" },
    };
    for (unsigned i = 0; i < sizeof W / sizeof W[0]; i++) uui_builder_widget(b, "main", W[i][0], W[i][1]);
    uui_builder_page(b, "main", "bottom", "chart");
    uui_builder_page(b, "main", "bottom", "history");
    int n = 0;
    uui_builder_menu(b, "bar", &n);
    const char *e = uui_builder_error(b);
    utest_checkf(b && !e && n == 3, "/usr/share/ui/diskmark.ui builds its window with every name, page and menu the "
                 "program asks for (%s)", e ? e : "ok");
    uui_builder_free(b);
}

// Image Viewer's window: every name the program asks for, of its type.
static void shipped_imgview(void) {
    static const struct uui_builder_name IV[] = {
        { "menu", 1 }, { "toolbar", 2 }, { "strip", 3 }, { "image", 4 }, { "slide", 7 }, { "wallpaper", 120 },
        { "open", 101 }, { "reload", 102 }, { "exit", 103 }, { "copy", 104 }, { "prev", 105 }, { "next", 106 },
        { "zoom_in", 107 }, { "zoom_out", 108 }, { "fit", 109 }, { "actual", 110 }, { "rot_l", 111 },
        { "rot_r", 112 }, { "props", 113 }, { "slideshow", 114 }, { "wallpaper_fill", 115 },
        { "wallpaper_fit", 116 }, { "wallpaper_none", 117 },
    };
    struct uui_builder *b = uui_builder_load("imgview", IV, (int)(sizeof IV / sizeof IV[0]));
    static const char *const W[][2] = {
        { "menu", "menubar" }, { "toolbar", "toolbar" }, { "image", "image" }, { "strip", "thumbstrip" },
        { "wpfill", "button" }, { "wpfit", "button" }, { "status", "statusbar" }, { "slide", "transport" },
    };
    for (unsigned i = 0; i < sizeof W / sizeof W[0]; i++) uui_builder_widget(b, "main", W[i][0], W[i][1]);
    struct uui_item *panel = uui_builder_item(b, "main", "panel");
    int n = 0;
    uui_builder_menu(b, "bar", &n);
    const char *e = uui_builder_error(b);
    utest_checkf(b && !e && n == 4 && panel && panel->ops == &uui_layout_ops,
                 "/usr/share/ui/imgview.ui builds its window with every name the program asks for, #panel a "
                 "container (%s)", e ? e : "ok");
    uui_builder_free(b);
}

// A spinbox: its range, step, unit and start from the file, and Tab stops on it.
static void spinbox(void) {
    static const char SRC[] =
        "window #w focus=\"n\" autofocus {\n"
        "    row {\n"
        "        spinbox #n range=1..10 step=2 unit=px value=4\n"
        "        spinbox #m range=-5..5\n"
        "    }\n"
        "}\n";
    struct uui_builder *b = uui_builder_parse("s.ui", SRC, sizeof SRC - 1, 0, 0);
    struct uui_spinbox *n = uui_builder_widget(b, "w", "n", "spinbox");
    struct uui_spinbox *m = uui_builder_widget(b, "w", "m", "spinbox");
    struct uapp_desc desc = { 0 };
    int win = uui_builder_window(b, "w", &desc);
    const char *e = uui_builder_error(b);
    utest_checkf(!e && win && n && m, "a spinbox builds (%s)", e ? e : "ok");
    utest_check(n && n->min == 1 && n->max == 10 && n->step == 2 && n->value == 4 && n->unit &&
                !strcmp(n->unit, "px"), "range=, step=, unit= and value= reach the spinbox");
    utest_check(m && m->value == -5 && m->step == 1 && !m->unit, "with no value= it starts at the range's low end");
    utest_check(desc.focus && desc.focus->count == 1 && desc.focus->items[0].widget == n,
                "Tab stops on a spinbox");
    uui_builder_free(b);
    build_rejects("window #w {\n    row {\n        spinbox #n\n    }\n}\n", "spinbox needs range=",
                  "a spinbox with no range");
    build_rejects("window #w {\n    row {\n        spinbox #n range=1..10 value=11\n    }\n}\n", "value",
                  "a spinbox starting outside its range");
    build_rejects("window #w {\n    row {\n        spinbox #n range=10..1\n    }\n}\n", "is not LO..HI",
                  "a range written backwards");
}

// An app place in a window: the program's widget, given before the window
// is asked for; reached by Tab when focus= lists it and its ops take keys.
static int app_key(void *w, int key, unsigned mods) { (void)w; (void)key; (void)mods; return 0; }
static void app_in_window(void) {
    static const char SRC[] =
        "window #w focus=\"x t\" autofocus {\n"
        "    column {\n"
        "        app #x fill\n"
        "        tree #t\n"
        "    }\n"
        "}\n";
    static struct uui_widget_ops keyed;
    static int mine;
    keyed = uui_label_ops;
    keyed.key = app_key;
    struct uui_builder *b = uui_builder_parse("a.ui", SRC, sizeof SRC - 1, 0, 0);
    struct uapp_desc desc = { 0 };
    utest_check(!uui_builder_window(b, "w", &desc) && uui_builder_error(b) &&
                strstr(uui_builder_error(b), "gave app #x no widget"), "an app place left empty is refused");
    uui_builder_free(b);

    b = uui_builder_parse("a.ui", SRC, sizeof SRC - 1, 0, 0);
    struct uui_item *x = uui_builder_item(b, "w", "x");
    struct uui_tree *t = uui_builder_widget(b, "w", "t", "tree");
    if (x) { x->ops = &keyed; x->widget = &mine; }
    memset(&desc, 0, sizeof desc);
    int ok = uui_builder_window(b, "w", &desc);
    const char *e = uui_builder_error(b);
    utest_checkf(ok && !e && t && x && (x->flags & UUI_FILL_W) && desc.focus && desc.focus->count == 2 &&
                 desc.focus->items[0].widget == &mine && desc.focus->items[0].ops == &keyed &&
                 desc.focus->items[1].widget == t, "an app place takes the program's widget, in Tab order (%s)",
                 e ? e : "ok");
    uui_builder_free(b);

    b = uui_builder_parse("a.ui", SRC, sizeof SRC - 1, 0, 0);
    x = uui_builder_item(b, "w", "x");
    if (x) { x->ops = &uui_label_ops; x->widget = &mine; }
    memset(&desc, 0, sizeof desc);
    utest_check(!uui_builder_window(b, "w", &desc) && uui_builder_error(b) &&
                strstr(uui_builder_error(b), "takes no keys"), "an app place in focus= whose ops take no keys");
    uui_builder_free(b);
}

// Help's window: every name the program asks for, of its type; #doc an
// app place, #contents the column the program sizes.
static void shipped_help(void) {
    static const struct uui_builder_name HE[] = {
        { "toolbar", 1 }, { "search", 2 }, { "tree", 3 }, { "doc", 4 },
        { "back", 1 }, { "forward", 2 }, { "home", 3 },
    };
    struct uui_builder *b = uui_builder_load("help", HE, (int)(sizeof HE / sizeof HE[0]));
    uui_builder_widget(b, "main", "toolbar", "toolbar");
    uui_builder_widget(b, "main", "search", "textbox");
    uui_builder_widget(b, "main", "tree", "tree");
    uui_builder_widget(b, "main", "crumb", "label");
    struct uui_item *contents = uui_builder_item(b, "main", "contents");
    struct uui_item *doc = uui_builder_item(b, "main", "doc");
    const char *e = uui_builder_error(b);
    utest_checkf(b && !e && contents && contents->ops == &uui_layout_ops && doc && !doc->ops,
                 "/usr/share/ui/help.ui builds its window with every name the program asks for (%s)", e ? e : "ok");
    uui_builder_free(b);
}

// The Installer's window: its six steps, every name it asks for, of its
// type; the disk cards, the plan, the software checklist and the stages
// app places.
static void shipped_installer(void) {
    static const struct uui_builder_name IN[] = {
        { "disks", 1 }, { "table", 2 }, { "plan", 3 }, { "ask_all", 4 }, { "restart", 5 }, { "dialog", 6 },
        { "keep", 7 }, { "shutdown", 8 }, { "software", 9 }, { "sw_all", 10 }, { "sw_none", 11 },
    };
    static const char *const W[][2] = {
        { "dialog", "dialog" }, { "intro", "label" }, { "intro_live", "label" }, { "facts", "custom" },
        { "none", "label" }, { "refused", "label" }, { "table", "segmented" }, { "summary", "custom" },
        { "ask_all", "checkbox" }, { "restart", "checkbox" }, { "done_title", "label" }, { "done_text", "label" },
        { "done_facts", "custom" }, { "keep", "button" }, { "shutdown", "button" }, { "sw_total", "label" },
        { "sw_all", "button" }, { "sw_none", "button" },
    };
    struct uui_builder *b = uui_builder_load("installer", IN, (int)(sizeof IN / sizeof IN[0]));
    struct uui_wizard *wz = uui_builder_wizard(b, "main", "wizard", 6);
    for (unsigned i = 0; i < sizeof W / sizeof W[0]; i++) uui_builder_widget(b, "main", W[i][0], W[i][1]);
    uui_builder_choice(b, "main", "table", "gpt");
    uui_builder_choice(b, "main", "table", "mbr");
    int empty = 1;
    static const char *const APPS[] = { "disks", "plan", "software", "stages" };
    for (unsigned i = 0; i < sizeof APPS / sizeof APPS[0]; i++) {
        struct uui_item *it = uui_builder_item(b, "main", APPS[i]);
        empty &= it && !it->ops;
    }
    const char *e = uui_builder_error(b);
    utest_checkf(b && !e && wz && empty, "/usr/share/ui/installer.ui builds its window with every name, choice id "
                 "and step the program asks for (%s)", e ? e : "ok");
    uui_builder_free(b);
}

// --- options: an Options window's rows bound to a uprefs table ----------

struct optconf {
    int wrap, tab, font, view, cols, rows;
    char shell[32];
    struct { int thumbs; } folder;
};
static const char *const VIEW_WORDS[] = { "icons", "large", "details" };
static const struct upref OPT_KEYS[] = {
    UPREF_BOOL_KEY("wrap", struct optconf, wrap, 1),
    UPREF_INT_KEY("tab", struct optconf, tab, 2, 8, 4),
    UPREF_INT_KEY("font", struct optconf, font, 0, 40, 0),
    UPREF_WORD_KEY("view", struct optconf, view, VIEW_WORDS, 3, 0),
    UPREF_INT_KEY("cols", struct optconf, cols, 40, 300, 120),
    UPREF_INT_KEY("rows", struct optconf, rows, 10, 100, 30),
    UPREF_TEXT_KEY("shell", struct optconf, shell, "", 0),
};
static const struct uprefs OPT_PREFS = { "/tmp/opt.conf", OPT_KEYS, (int)(sizeof OPT_KEYS / sizeof OPT_KEYS[0]) };
struct folderconf { int thumbs; };
static const struct upref FOLDER_KEYS[] = { UPREF_BOOL_KEY("thumbs", struct folderconf, thumbs, 1) };
static const struct uprefs FOLDER_PREFS = { "/tmp/folder.conf", FOLDER_KEYS, 1 };
static const struct uui_builder_prefs OPT_TABLES[] = {
    { &OPT_PREFS, 0 }, { &FOLDER_PREFS, offsetof(struct optconf, folder) },
};
enum { ID_CLEAR = 30, ID_SHELL = 31 };
static const struct uui_builder_name ONAMES[] = { { "clear", ID_CLEAR }, { "shell", ID_SHELL } };

static const char OPTS[] =
    "options #o title=\"T Options\" size=80x20 {\n"
    "    page #ed \"Editor\" note=\"What typing does.\" {\n"
    "        choice #wrap caption=\"Wrap:\" key=wrap {\n"
    "            item \"On\" value=on\n"
    "            item \"Off\" value=off\n"
    "        }\n"
    "        choice caption=\"Tab:\" key=tab {\n"
    "            item \"2\" value=2\n"
    "            item \"8\" value=8\n"
    "        }\n"
    "        fontsize #font caption=\"Font:\" key=font range=8..40 unit=px \"Use the desktop's size\"\n"
    "        row \"Size:\" {\n"
    "            number #cols key=cols unit=columns\n"
    "            number #rows key=rows step=2\n"
    "        }\n"
    "        check #thumbs \"Thumbnails\" key=thumbs\n"
    "    }\n"
    "    page \"More\" {\n"
    "        choice #view caption=\"View:\" key=view {\n"
    "            item \"Large\" value=large\n"
    "            item \"Details\" value=details\n"
    "        }\n"
    "        dropdown #shell caption=\"Shell:\"\n"
    "        row \"Recent:\" {\n"
    "            app #list fill-w\n"
    "            button #clear \"Clear\"\n"
    "        }\n"
    "        rows #effect\n"
    "    }\n"
    "}\n";

// Builds OPTS with `from` replaced by `to`, binds it, and expects `has`.
static void opt_rejects(const char *from, const char *to, const char *has, const char *what) {
    static char src[sizeof OPTS + 512];
    const char *at = strstr(OPTS, from);
    if (!at) { utest_checkf(0, "%s: the fixture lacks \"%s\"", what, from); return; }
    snprintf(src, sizeof src, "%.*s%s%s", (int)(at - OPTS), OPTS, to, at + strlen(from));
    struct uui_builder *b = uui_builder_parse("o.ui", src, strlen(src), ONAMES, 2);
    uui_builder_options(b, "o", OPT_TABLES, 2);
    const char *e = uui_builder_error(b);
    utest_checkf(e && strstr(e, has), "%s (%s)", what, e ? e : "built and bound");
    uui_builder_free(b);
}

static void options(void) {
    struct uui_builder *b = uui_builder_parse("o.ui", OPTS, sizeof OPTS - 1, ONAMES, 2);
    struct uui_dropdown *shell = uui_builder_widget(b, "o", "shell", "dropdown");
    struct uui_button *clear = uui_builder_widget(b, "o", "clear", "button");
    struct uui_item *list = uui_builder_item(b, "o", "list");
    int bound = uui_builder_options(b, "o", OPT_TABLES, 2);
    const char *e = uui_builder_error(b);
    utest_checkf(!e && bound && shell && clear && list, "an options file builds and binds to two tables (%s)",
                 e ? e : "ok");
    utest_check(clear && clear->code == ID_CLEAR && list && !list->ops && (list->flags & UUI_FILL_W),
                "an app place is the program's to fill; a button's command is its #name");
    struct uapp_desc desc = { 0 };
    utest_check(!uui_builder_window(b, "o", &desc) && uui_builder_error(b) &&
                strstr(uui_builder_error(b), "is an options"), "an options is not opened as a window");
    uui_builder_free(b);

    // Nothing opens until the binding is done: a test cannot open a window,
    // so the refusal before it is what is checked.
    b = uui_builder_parse("o.ui", OPTS, sizeof OPTS - 1, ONAMES, 2);
    struct uui_prefs_desc d = { 0 };
    utest_check(!uui_builder_options_open(b, "o", &d, 0, 0, 0), "an options not yet bound does not open");
    uui_builder_free(b);

    opt_rejects("key=wrap", "key=wrapp", "no setting key=wrapp", "a key the tables lack");
    opt_rejects("check #thumbs \"Thumbnails\" key=thumbs", "check #thumbs \"Thumbnails\" key=tab",
                "is a number setting", "a check over a number setting");
    opt_rejects("number #rows key=rows", "number #rows key=shell", "is a text setting", "a number over a text one");
    opt_rejects("value=large", "value=huge", "takes icons, large, details", "a choice's value the word lacks");
    opt_rejects("value=on", "value=yes", "takes on or off", "a bool's value is on or off, nothing else");
    opt_rejects("value=8", "value=9", "a number in 2..8", "an int choice past the setting's range");
    opt_rejects("value=off", "value=on", "two items store value=on", "two items storing one value");
    opt_rejects("range=8..40", "range=8..41", "reaches past key=font's own", "a range past the setting's");
    opt_rejects(" range=8..40", "", "fontsize needs range=", "a fontsize says its sizes");
    opt_rejects("key=cols unit", "key=cols range=0..10 unit", "reaches past", "a number's range under the table's");
    opt_rejects("            button #clear \"Clear\"\n",
                "            button #clear \"Clear\"\n            check \"x\" key=wrap\n            check \"y\" key=wrap\n",
                "two or three controls", "a row of four");
    opt_rejects("        rows #effect\n", "        row \"x\" {\n            rows #r\n            check \"y\" key=wrap\n        }\n",
                "a run of rows", "rows inside a row");
    opt_rejects("            number #rows key=rows step=2\n", "            choice key=view {\n"
                "                item \"a\" value=icons\n                item \"b\" value=large\n            }\n",
                "not a choice", "a choice beside a row's first control");
    opt_rejects("check #thumbs \"Thumbnails\" key=thumbs", "check #wrap \"Thumbnails\" key=thumbs",
                "has two #wrap", "two rows with one name");
    opt_rejects("choice caption=\"Tab:\" key=tab", "choice caption=\"Tab:\" key=wrap", "has two #wrap",
                "a key= is a row's name when it has no #name");
    opt_rejects("dropdown #shell caption=\"Shell:\"", "dropdown #shell caption=\"Shell:\" hidden",
                "places its controls", "a hidden row");
    opt_rejects("button #clear \"Clear\"", "button #clear \"Clear\" fill-w", "first control", "fill on a second control");
    opt_rejects("app #list fill-w", "app fill-w", "app needs a #name", "an app place unnamed");
    opt_rejects("        rows #effect\n", "        column {\n            label \"x\"\n        }\n",
                "not column", "a container on an Options page");
    opt_rejects("page \"More\"", "page #ed \"More\"", "two pages are called #ed", "two pages with one name");
    app_in_window();
    build_rejects("options #o {\n}\n", "options holds no page", "an options with no page");
}

// The five shipped Options files, from where each program reads them:
// they build, with every name and choice id the program looks up. The
// binding is each program's own, at its start (its tables are static to it).
static void shipped_options(void) {
    static const struct {
        const char *app;
        struct uui_builder_name names[8];
        int nnames;
        const char *widgets[8][2];
        const char *choices[3][3];   // { widget, id, id }
        const char *apps[4];
    } F[] = {
        { "notepad", { { "clear", 1 } }, 1, { { "clear", "button" } }, { { 0 } }, { 0 } },
        { "files", { { 0 } }, 0, { { "scripts", "segmented" }, { "programs", "segmented" } },
          { { "scripts", "ask", "edit" }, { "programs", "ask", "run" } }, { 0 } },
        { "terminal", { { "scheme", 1 }, { "shell", 2 } }, 2, { { "shell", "dropdown" } }, { { 0 } }, { "scheme" } },
        { "screenshot", { { "key0", 1 }, { "key1", 2 }, { "key2", 3 }, { "delays", 4 }, { "folder", 5 },
                          { "browse", 6 }, { "name", 7 }, { "format", 8 } }, 8,
          { { "key0", "dropdown" }, { "key1", "dropdown" }, { "key2", "dropdown" }, { "delays", "dropdown" },
            { "folder", "label" }, { "format", "segmented" } },
          { { "format", "qoi", "png" } }, { "name" } },
        { "retrolauncher", { { "fontcard", 1 }, { "sound-table", 2 }, { "drive-type", 3 }, { "drive-label", 4 } }, 4,
          { { "drive-type", "segmented" }, { "drive-label", "textbox" }, { "help-note", "label" } },
          { { "drive-type", "folder", "cd" } }, { "fontcard", "sound-table", "drives", "folder-list" } },
    };
    for (unsigned f = 0; f < sizeof F / sizeof F[0]; f++) {
        struct uui_builder *b = uui_builder_load(F[f].app, F[f].names, F[f].nnames);
        for (int i = 0; i < 8 && F[f].widgets[i][0]; i++) uui_builder_widget(b, "options", F[f].widgets[i][0], F[f].widgets[i][1]);
        for (int i = 0; i < 3 && F[f].choices[i][0]; i++)
            for (int k = 1; k < 3; k++) uui_builder_choice(b, "options", F[f].choices[i][0], F[f].choices[i][k]);
        int empty = 1;
        for (int i = 0; i < 4 && F[f].apps[i]; i++) {
            struct uui_item *it = uui_builder_item(b, "options", F[f].apps[i]);
            empty &= it && !it->ops;
        }
        const char *e = uui_builder_error(b);
        utest_checkf(b && !e && empty, "/usr/share/ui/%s.ui's options build with every name and choice id the "
                     "program asks for (%s)", F[f].app, e ? e : "ok");
        uui_builder_free(b);
    }
}

int main(void) {
    utest_begin("uibuilder_test", "base/udecl and ui/uui_builder", UTEST_VERDICT_FILE);
    parser();
    builder();
    form();
    menus();
    bars();
    paints_once();
    hosted();
    floating();
    wizard();
    shipped();
    shipped_logview();
    shipped_diskmark();
    shipped_imgview();
    spinbox();
    shipped_help();
    shipped_installer();
    options();
    shipped_options();
    return utest_end();
}
