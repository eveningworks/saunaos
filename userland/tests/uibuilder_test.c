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
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "base/utest.h"
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

int main(void) {
    utest_begin("uibuilder_test", "base/udecl and ui/uui_builder", UTEST_VERDICT_FILE);
    parser();
    builder();
    form();
    wizard();
    shipped();
    return utest_end();
}
