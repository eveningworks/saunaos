// See ui/uui_builder.h; the language and every type in docs/ui-files-design.md.
//
// ONE TABLE OF TYPES, below: a type is its word, its ops table, the size
// of its struct, a function reading its own attributes and, when Tab
// reaches it through other ops, how. What every item shares (#name, fill,
// hidden, width/height) is read once, in build_item(). A widget joins the
// language by gaining a row.
//
// THE OBJECT IS NOT ALWAYS THE ITEM'S WIDGET: a composite (the colour
// picker) puts its own inner layout in the tree, so a lookup returns the
// object a type allocated, checked by the type's word.
#include "ui/uui_builder.h"
#include "ui/uui_layout.h"
#include "ui/uui_label.h"
#include "ui/uui_button.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_table.h"
#include "ui/uui_markdown.h"
#include "ui/uui_focus.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_textbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_listbox.h"
#include "ui/uui_progress.h"
#include "ui/uui_colourpick.h"
#include "ui/uui_themepreview.h"
#include "ui/uui_stack.h"
#include "ui/uui_wizard.h"
#include "ui/uui_dialog.h"
#include "ui/uui_menubar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_loglist.h"
#include "ui/uui_meter.h"
#include "ui/uui_chart.h"
#include "ui/uui_image.h"
#include "ui/uui_thumbstrip.h"
#include "ui/uui_transport.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_tree.h"
#include "ui/uui_prefs.h"
#include "ui/uui_sidebar.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "base/udecl.h"
#include "base/uprefs.h"
#include "base/ugrow.h"
#include "base/ulog.h"
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct btype;
struct bopts;

struct named {
    const char *name;
    struct uui_item *item;
    void *obj;                  // what the type allocated
    const struct btype *type;
    const struct uui_item *page;   // the innermost stack page it is on, or NULL
    const char **ids;           // a dropdown's or listbox's item ids, by index
    int nids;
    int line;                   // where the file declares it
};

struct bwin {
    const char *name;
    int line;
    struct uui_item *root_item;   // allocated: a #name on it must outlive the build
    struct uui_layout *root;
    // With `dialog` lines: [the root, each dialog] -- routed, the root
    // alone laid out (uapp draws an open dialog over the window).
    struct uui_item *top;
    int ntop, building_top;
    const struct uui_item *cur_page;   // while a stack page's subtree is built
    const char *title, *app_id;
    int cells_w, cells_h, min_w, min_h, resizable;
    int *focus;                // the focus= list, indices into named[]
    int nfocus, focus_cap, autofocus;
    struct uui_focus ring;
    struct named *named;
    int nnamed, named_cap;
    struct bopts *opts;        // an `options`, not a window
};

// A top-level `menu`: the tree uui_menubar_init() and uui_menubar_open_at() take.
struct bmenu {
    const char *name;
    struct uui_menu_item *items;
    int count;
    struct udecl_node *node;   // where the file declares it
};

struct uui_builder {
    struct udecl d;
    const struct uui_builder_name *names;
    int nnames;
    struct bwin *win;
    int nwin, win_cap;   // built; allocated (a failed window's arrays are freed too)
    struct bmenu *menu;
    int nmenu;
    void **mem;   // every allocation the tree holds
    int nmem, mem_cap;
    int widgets;
    unsigned us;
};

static void *alloc(struct uui_builder *b, size_t size, int line) {
    void *p = size ? calloc(1, size) : 0;
    if (size && (!p || !UGROW(b->mem, b->mem_cap, b->nmem + 1))) {
        free(p);
        udecl_fail(&b->d, line, "out of memory");
        return 0;
    }
    if (p) b->mem[b->nmem++] = p;
    return p;
}

static int lookup(const struct uui_builder *b, const char *name, int *out) {
    for (int i = 0; i < b->nnames; i++)
        if (!strcmp(b->names[i].name, name)) { *out = b->names[i].value; return 1; }
    return 0;
}

// A command word, which the app MUST know.
static int command(struct uui_builder *b, struct udecl_node *n, const char *word, int *out) {
    if (!word) return udecl_fail(&b->d, n->line, "%s needs command= or a #name", n->type);
    if (!lookup(b, word, out)) return udecl_fail(&b->d, n->line, "the app has no command '%s'", word);
    return 0;
}

static int no_children(struct uui_builder *b, struct udecl_node *n) {
    return n->nkid ? udecl_fail(&b->d, n->kid[0].line, "%s takes no children", n->type) : 0;
}

// --- the types --------------------------------------------------------

// `dir` is the PARENT container's axis, or -1 (a grid's cell, a window's root).
// On entry it->widget is the freshly zeroed object; a composite re-aims
// it->ops and it->widget at what it puts in the tree.
typedef int (*build_fn)(struct uui_builder *b, struct bwin *w, struct udecl_node *n,
                        struct uui_item *it, int dir);

static int build_item(struct uui_builder *b, struct bwin *w, struct udecl_node *n,
                      struct uui_item *it, int dir);

// A spacing: pixels ("8"), lines and characters ("0.5lh", "1ch") as
// uui_layout keeps them -- hundredths, resolved at each layout -- or a
// theme step ("small", "medium", "large", and "none" for an explicit
// zero). `s` is one word of `len` bytes; 1 read, -1 the error set.
static const char *const STEPS[] = {
    [UTHEME_STEP_SMALL] = "small", [UTHEME_STEP_MEDIUM] = "medium", [UTHEME_STEP_LARGE] = "large",
    [UTHEME_STEP_NONE] = "none",
};
static int spacing(struct udecl *d, struct udecl_node *n, const char *key, const char *s, size_t len,
                   int *v, unsigned char *unit) {
    for (int k = UTHEME_STEP_SMALL; k <= UTHEME_STEP_NONE; k++)
        if (strlen(STEPS[k]) == len && !memcmp(s, STEPS[k], len)) {
            *unit = UUI_STEP;
            *v = k;
            return 1;
        }
    size_t all = len;   // the whole word, for the message
    *unit = UUI_PX;
    if (len > 2 && (!memcmp(s + len - 2, "lh", 2) || !memcmp(s + len - 2, "ch", 2))) {
        *unit = s[len - 2] == 'l' ? UUI_LH : UUI_CH;
        len -= 2;
    }
    // Digits, then -- for lh and ch only -- at most two after a point:
    // hundredths, exactly, so "0.5lh" is ugfx_char_h() / 2 as the C wrote it.
    int64_t whole = 0, frac = 0;
    size_t i = 0;
    for (; i < len && s[i] >= '0' && s[i] <= '9' && whole <= 1000; i++) whole = whole * 10 + (s[i] - '0');
    if (i == 0) goto bad;
    if (i < len) {
        if (s[i] != '.' || *unit == UUI_PX || len - i < 2 || len - i > 3) goto bad;
        for (size_t k = i + 1; k < i + 3; k++) {
            if (k < len && (s[k] < '0' || s[k] > '9')) goto bad;
            frac = frac * 10 + (k < len ? s[k] - '0' : 0);
        }
    }
    int64_t x = *unit == UUI_PX ? whole : whole * 100 + frac;
    if (x > (*unit == UUI_PX ? 1000 : 100000)) goto bad;
    // 0 IS uui_layout's "font-derived default", so an explicit 0 cannot be
    // said; refused rather than quietly turned into the default spacing.
    if (!x) return udecl_fail(d, n->line, "0 means the default spacing here: leave it out, write none for no "
                              "space, or 1 for a hairline");
    *v = (int)x;
    return 1;
bad:
    return udecl_fail(d, n->line, "%s=%.*s is not a spacing: small, medium, large or none, pixels (8), lines "
                      "(0.5lh) or characters (1ch)", key, (int)all, s);
}

// One spacing. 0 when absent, 1 read, -1 the error set.
static int length(struct udecl *d, struct udecl_node *n, const char *key, int *v, unsigned char *unit) {
    const char *s = udecl_get(d, n, key);
    return s ? spacing(d, n, key, s, strlen(s), v, unit) : 0;
}

// A margin: one spacing for every side, or "TOP-AND-BOTTOM SIDES" --
// CSS's two-value margin, the same order.
static int margins(struct udecl *d, struct udecl_node *n, struct uui_layout *l) {
    const char *s = udecl_get(d, n, "margin");
    if (!s) return 0;
    const char *word[3];
    size_t len[3];
    int nw = 0;
    for (const char *p = s; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (nw == 2) return udecl_fail(d, n->line, "margin= takes one spacing, or two: top-and-bottom, then the sides");
        word[nw] = p;
        len[nw++] = (size_t)(e - p);
        p = e;
    }
    if (!nw) return udecl_fail(d, n->line, "margin= is empty: leave it out for the default");
    if (spacing(d, n, "margin", word[0], len[0], &l->margin, &l->margin_unit) < 0) return -1;
    return nw == 2 ? spacing(d, n, "margin", word[1], len[1], &l->margin_x, &l->margin_x_unit) : 1;
}

static int build_container(struct uui_builder *b, struct bwin *w, struct udecl_node *n,
                           struct uui_item *it, int dir, enum uui_dir kind) {
    (void)dir;
    struct uui_layout *l = it->widget;
    struct udecl *d = &b->d;
    l->dir = kind;
    if (margins(d, n, l) < 0 || length(d, n, "gap", &l->gap, &l->gap_unit) < 0)
        return -1;
    if (kind == UUI_GRID && udecl_int(d, n, "cols", 1, 1000, &l->cols) < 0) return -1;
    if (!n->nkid) return udecl_fail(d, n->line, "%s holds nothing", n->type);
    l->items = alloc(b, sizeof *l->items * (size_t)n->nkid, n->line);
    if (!l->items) return -1;
    l->count = n->nkid;
    for (int i = 0; i < n->nkid; i++)
        if (build_item(b, w, &n->kid[i], &l->items[i], kind == UUI_GRID ? -1 : (int)kind) < 0) return -1;
    return 0;
}
static int build_row(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    return build_container(b, w, n, it, dir, UUI_ROW);
}
static int build_column(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    return build_container(b, w, n, it, dir, UUI_COLUMN);
}
static int build_grid(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    return build_container(b, w, n, it, dir, UUI_GRID);
}

static int build_label(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    struct uui_label *l = it->widget;
    const char *t = udecl_text(n);
    uui_label_init(l, t ? t : "");
    l->dim = udecl_flag(&b->d, n, "dim");
    l->bold = udecl_flag(&b->d, n, "bold");
    l->wrap = udecl_flag(&b->d, n, "wrap");
    if (udecl_int(&b->d, n, "rows", 1, 1000, &l->rows) < 0) return -1;
    return no_children(b, n);
}

// Qt's addStretch(): an empty label taking the parent's spare room.
static int build_stretch(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w;
    if (dir != UUI_ROW && dir != UUI_COLUMN) return udecl_fail(&b->d, n->line, "stretch belongs in a row or a column");
    uui_label_init(it->widget, "");
    it->flags |= dir == UUI_ROW ? UUI_FILL_W : UUI_FILL_H;
    return no_children(b, n);
}

static int build_button(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    struct uui_button *bt = it->widget;
    const char *t = udecl_text(n);
    if (!t) return udecl_fail(&b->d, n->line, "button needs its text, in quotes");
    const char *word = udecl_get(&b->d, n, "command");
    int code;
    if (command(b, n, word ? word : n->name, &code) < 0) return -1;
    uui_button_init(bt, 0, 0, 0, 0, t, 0, 0, code);
    uui_button_set_default(bt, udecl_flag(&b->d, n, "default"));
    bt->outlined = udecl_flag(&b->d, n, "outlined");
    bt->disabled = udecl_flag(&b->d, n, "disabled");
    it->id = code;   // a lone button is heard by its code (on_action)
    return no_children(b, n);
}

static const char *const ROLES[UTHEME_ACT_COUNT] = {
    [UTHEME_ACT_NONE] = "none", [UTHEME_ACT_NAV] = "nav", [UTHEME_ACT_VIEW] = "view",
    [UTHEME_ACT_CREATE] = "create", [UTHEME_ACT_EDIT] = "edit", [UTHEME_ACT_DANGER] = "danger",
    [UTHEME_ACT_ARRANGE] = "arrange", [UTHEME_ACT_MEDIA] = "media",
};

// Buttons are `item` and `separator` lines; any other line is a widget the
// strip hosts (UUI_TB_CONTROL, QToolBar::addWidget()): built as anywhere
// else, so it is named, focused and looked up as usual.
static int build_toolbar(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)dir;
    struct udecl *d = &b->d;
    if (!n->nkid) return udecl_fail(d, n->line, "toolbar holds nothing");
    struct uui_toolbar_item *items = alloc(b, sizeof *items * (size_t)n->nkid, n->line);
    if (!items) return -1;
    int ncontrols = 0;
    for (int i = 0; i < n->nkid; i++)
        ncontrols += strcmp(n->kid[i].type, "item") && strcmp(n->kid[i].type, "separator");
    if (ncontrols > UUI_TOOLBAR_CONTROLS_MAX)
        return udecl_fail(d, n->line, "a toolbar holds at most %d controls", UUI_TOOLBAR_CONTROLS_MAX);
    struct uui_item *controls = ncontrols ? alloc(b, sizeof *controls * (size_t)ncontrols, n->line) : 0;
    if (ncontrols && !controls) return -1;
    int k = 0;
    for (int i = 0; i < n->nkid; i++) {
        struct udecl_node *kn = &n->kid[i];
        if (strcmp(kn->type, "item") && strcmp(kn->type, "separator")) {
            if (build_item(b, w, kn, &controls[k], -1) < 0) return -1;
            // The strip sizes and places what it holds: these would be read and ignored.
            if (controls[k].flags & (UUI_FILL_W | UUI_FILL_H | UUI_CONTROL_H))
                return udecl_fail(d, kn->line, "the toolbar sizes what it holds: no fill, fill-w, fill-h or height=");
            items[i] = (struct uui_toolbar_item){ .code = k++, .flags = UUI_TB_CONTROL };
            continue;
        }
        if (no_children(b, kn) < 0) return -1;
        if (!strcmp(kn->type, "separator")) {
            if (udecl_unused(d, kn) < 0) return -1;
            continue;   // all zero: UUI_TOOLBAR_SEP
        }
        struct uui_toolbar_item *ti = &items[i];
        ti->label = udecl_text(kn);
        ti->icon = udecl_get(&b->d, kn, "icon");
        if (!ti->icon) return udecl_fail(d, kn->line, "item needs icon=");
        ti->tip = udecl_get(&b->d, kn, "tip");
        ti->accel = udecl_get(&b->d, kn, "accel");
        const char *word = udecl_get(&b->d, kn, "command");
        if (command(b, kn, word ? word : kn->name, &ti->code) < 0) return -1;
        int role = 0;
        if (udecl_word(d, kn, "role", ROLES, UTHEME_ACT_COUNT, &role) < 0) return -1;
        ti->tint = (uint32_t)role;
        ti->flags = (udecl_flag(&b->d, kn, "menu") ? UUI_TB_MENU : 0) | (udecl_flag(&b->d, kn, "end") ? UUI_TB_END : 0);
        if (kn->name) return udecl_fail(d, kn->line, "a toolbar item is named by command=, not #");
        if (udecl_unused(d, kn) < 0) return -1;
    }
    struct uui_toolbar *t = it->widget;
    uui_toolbar_init(t, items, n->nkid);
    t->controls = controls;
    t->control_count = ncontrols;
    return 0;
}

static const char *const ALIGNS[] = { [UUI_TALIGN_LEFT] = "left", [UUI_TALIGN_RIGHT] = "right" };

static int build_table(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    struct udecl *d = &b->d;
    if (!n->nkid) return udecl_fail(d, n->line, "table needs its columns");
    struct uui_table_column *cols = alloc(b, sizeof *cols * (size_t)n->nkid, n->line);
    if (!cols) return -1;
    for (int i = 0; i < n->nkid; i++) {
        struct udecl_node *k = &n->kid[i];
        if (strcmp(k->type, "column")) return udecl_fail(d, k->line, "a table holds column, not %s", k->type);
        if (no_children(b, k) < 0) return -1;
        cols[i].title = udecl_text(k);
        if (!cols[i].title) return udecl_fail(d, k->line, "column needs its title, in quotes");
        if (udecl_int(d, k, "width", 0, 1000, &cols[i].width_chars) < 0) return -1;
        if (udecl_word(d, k, "align", ALIGNS, 2, &cols[i].align) < 0) return -1;
        cols[i].id = udecl_get(&b->d, k, "id");
        if (udecl_unused(d, k) < 0) return -1;
    }
    // The rows' text is the app's: it sets `cell` once it has the table.
    uui_table_init(it->widget, 0, 0, 100, 100, cols, n->nkid, 0, 0);
    return 0;
}

static int build_markdown(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_markdown_init(it->widget);
    return no_children(b, n);
}

// An `item "text"` child per entry, for the list-shaped types; none is
// allowed (the app fills it later, as Theme Editor's theme list does).
// A CHOICE IS KNOWN BY ITS id=, NOT ITS PLACE: a person may reorder and
// relabel the items, and the app asks uui_builder_choice() which index
// holds the id it means -- GtkComboBox's active-id, Qt's itemData.
static struct named *named_here(struct bwin *w, const char *name);
static int text_items(struct uui_builder *b, struct bwin *w, struct udecl_node *n, const char ***out) {
    struct udecl *d = &b->d;
    *out = 0;
    if (!n->nkid) return 0;
    const char **items = alloc(b, sizeof *items * (size_t)n->nkid, n->line);
    const char **ids = alloc(b, sizeof *ids * (size_t)n->nkid, n->line);
    if (!items || !ids) return -1;
    for (int i = 0; i < n->nkid; i++) {
        struct udecl_node *k = &n->kid[i];
        if (strcmp(k->type, "item")) return udecl_fail(d, k->line, "%s holds item, not %s", n->type, k->type);
        if (no_children(b, k) < 0) return -1;
        items[i] = udecl_text(k);
        if (!items[i]) return udecl_fail(d, k->line, "item needs its text, in quotes");
        if (k->name) return udecl_fail(d, k->line, "an item is known by id=, not by a #name");
        ids[i] = udecl_get(d, k, "id");
        for (int j = 0; ids[i] && j < i; j++)
            if (ids[j] && !strcmp(ids[j], ids[i])) return udecl_fail(d, k->line, "two items have id=%s", ids[i]);
        if (udecl_unused(d, k) < 0) return -1;
    }
    struct named *nm = n->name ? named_here(w, n->name) : 0;
    if (nm) { nm->ids = ids; nm->nids = n->nkid; }
    *out = items;
    return n->nkid;
}

static int build_dropdown(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)dir;
    const char **items;
    int count = text_items(b, w, n, &items);
    if (count < 0) return -1;
    uui_dropdown_init(it->widget, 0, 0, 0, 0, items, count);
    int sel = 0;
    int r = udecl_int(&b->d, n, "selected", 0, count ? count - 1 : 0, &sel);
    if (r < 0) return -1;
    if (r && !count) return udecl_fail(&b->d, n->line, "selected= picks one of the item lines, and there are none");
    if (r) uui_dropdown_set_selected(it->widget, sel);
    return 0;
}

static int build_listbox(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)dir;
    const char **items;
    int count = text_items(b, w, n, &items);
    if (count < 0) return -1;
    uui_listbox_init(it->widget, 0, 0, 0, 0, items, count);
    return 0;
}

static int build_checkbox(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    struct uui_checkbox *cb = it->widget;
    const char *t = udecl_text(n);
    if (!t) return udecl_fail(&b->d, n->line, "checkbox needs its text, in quotes");
    uui_checkbox_init(cb, 0, 0, 0, t, UUI_COLOR_UNSET, UUI_COLOR_UNSET);
    cb->checked = udecl_flag(&b->d, n, "checked");
    cb->accent = udecl_flag(&b->d, n, "accent");
    return no_children(b, n);
}

static int build_textbox(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    const char *t = udecl_text(n);
    uui_textbox_init(it->widget, t ? t : "");
    return no_children(b, n);
}

static int build_progress(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_progress_init(it->widget);
    int v;
    int r = udecl_int(&b->d, n, "value", 0, 1000, &v);
    if (r < 0) return -1;
    if (r) uui_progress_set(it->widget, v);
    return no_children(b, n);
}

// An app-drawn area: the app sets its draw, and its size when it has one.
static int build_custom(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir; (void)it;
    return no_children(b, n);
}

static int build_themepreview(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_themepreview_init(it->widget, 0);   // the app points it at its theme
    return no_children(b, n);
}

// A COMPOSITE: its own layout goes in the tree, and its controls take ids
// from the #name's value up -- so that value must exist, and the layout
// item itself answers to none (it would collide with the first control).
// A modal question (ui/uui_dialog.h), declared beside the window's root:
// routed and drawn over the window when open, never laid out.
static int build_dialog(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)dir;
    if (!w->building_top) return udecl_fail(&b->d, n->line, "a dialog goes beside the window's row or column, not in it");
    uui_dialog_init(it->widget);
    return no_children(b, n);
}

// uui_wizard's steps, each a `step "Title" note="..." next="..."` holding
// its page. The callbacks are the app's: it sets them in the wizard's `d`.
static int build_wizard(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)dir;
    struct udecl *d = &b->d;
    if (!n->nkid) return udecl_fail(d, n->line, "wizard holds no step");
    if (n->nkid > UUI_WIZARD_STEPS_MAX)
        return udecl_fail(d, n->kid[UUI_WIZARD_STEPS_MAX].line, "a wizard has at most %d steps", UUI_WIZARD_STEPS_MAX);
    struct uui_wizard_step *steps = alloc(b, sizeof *steps * (size_t)n->nkid, n->line);
    if (!steps) return -1;
    for (int i = 0; i < n->nkid; i++) {
        struct udecl_node *k = &n->kid[i];
        if (strcmp(k->type, "step")) return udecl_fail(d, k->line, "a wizard holds steps, not %s", k->type);
        if (!udecl_text(k)) return udecl_fail(d, k->line, "step needs its title, in quotes");
        if (k->nkid != 1) return udecl_fail(d, k->line, "a step holds one page: put its controls in a column");
        if (k->name) return udecl_fail(d, k->line, "a step takes no #name: name its page");
        steps[i].title = udecl_text(k);
        steps[i].note = udecl_get(d, k, "note");
        steps[i].next = udecl_get(d, k, "next");
        if (build_item(b, w, &k->kid[0], &steps[i].page, -1) < 0 || udecl_unused(d, k) < 0) return -1;
    }
    struct uui_wizard_desc wd = {
        .heading = udecl_get(d, n, "heading"),
        .steps = steps,
        .count = n->nkid,
        .skip = udecl_get(d, n, "skip"),
        .finish = udecl_get(d, n, "finish"),
    };
    struct uui_wizard *wz = it->widget;
    uui_wizard_init(wz, &wd);
    it->flags |= uui_wizard_item(wz)->flags;
    // THE WIZARD DRAWS ITS OWN COPY of each page item (its body_items), so
    // a #name on a page must point there, or a lookup changes nothing shown.
    for (int k = 0; k < w->nnamed; k++)
        for (int i = 0; i < n->nkid; i++)
            if (w->named[k].item == &steps[i].page) w->named[k].item = uui_wizard_page(wz, i);
    return 0;
}

static int build_colourpick(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    struct uui_colourpick *c = it->widget;
    int base;
    if (!n->name || !lookup(b, n->name, &base))
        return udecl_fail(&b->d, n->line, "colourpick needs a #name in the app's names table: its controls' ids start there");
    uui_colourpick_init(c, base, 0);
    uui_colourpick_show_swatch(c, udecl_flag(&b->d, n, "swatch"));
    struct uui_item inner = uui_colourpick_item(c);
    it->ops = inner.ops;
    it->widget = inner.widget;
    it->flags |= inner.flags;
    it->id = 0;
    return no_children(b, n);
}
static int colourpick_focusables(void *obj, struct uui_focusable *out, int max) {
    return uui_colourpick_focusables(obj, out, max);
}

// Pages in one place, one shown: each child is a page, placed in the
// stack's whole rect, so a page takes no width= or height=.
static int build_stack(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)dir;
    struct udecl *d = &b->d;
    if (!n->nkid) return udecl_fail(d, n->line, "stack holds nothing");
    struct uui_item *pages = alloc(b, sizeof *pages * (size_t)n->nkid, n->line);
    if (!pages) return -1;
    const struct uui_item *outer = w->cur_page;
    for (int i = 0; i < n->nkid; i++) {
        w->cur_page = &pages[i];
        if (build_item(b, w, &n->kid[i], &pages[i], -1) < 0) return -1;
        // Which page shows is the stack's: shown=, then the app.
        if (pages[i].hidden) return udecl_fail(d, n->kid[i].line, "a stack shows one page: say shown=, not hidden");
    }
    w->cur_page = outer;
    uui_stack_init(it->widget, pages, n->nkid);
    int shown;
    int r = udecl_int(d, n, "shown", 0, n->nkid - 1, &shown);
    if (r < 0) return -1;
    if (r) uui_stack_set(it->widget, shown);
    return 0;
}

// Segments, like a dropdown's items: `item "..." id=word` lines, or none
// when the program fills them (its labels carry counts).
static int build_segmented(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)dir;
    const char **items;
    int count = text_items(b, w, n, &items);
    if (count < 0) return -1;
    int sel = 0, r = udecl_int(&b->d, n, "selected", 0, count ? count - 1 : 0, &sel);
    if (r < 0) return -1;
    if (r && !count) return udecl_fail(&b->d, n->line, "selected= picks one of the item lines, and there are none");
    uui_segmented_init(it->widget, items, count, sel);
    return 0;
}

// Its panes are the program's: their text changes as the program works.
static int build_statusbar(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_statusbar_init(it->widget);
    return no_children(b, n);
}

// The program points it at its lines: uui_loglist_init(list, &set).
static int build_loglist(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_loglist_init(it->widget, 0);
    return no_children(b, n);
}

// A reading card; its caption and value are the program's (uui_meter_set()).
static int build_meter(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_meter_init(it->widget);
    return no_children(b, n);
}

// A chart over time; the quoted text is its heading, the samples the program's.
static int build_chart(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    const char *t = udecl_text(n);
    uui_chart_init(it->widget, t ? t : "");
    return no_children(b, n);
}

// A picture, fitted; the program gives it its image (uui_image_set()).
static int build_image(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_image_init(it->widget, NULL, UIMG_FIT_CONTAIN);
    return no_children(b, n);
}

// A row of thumbnails; its pictures are the program's.
static int build_thumbstrip(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_thumbstrip_init(it->widget);
    return no_children(b, n);
}

// Previous, play or pause, next.
static int build_transport(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_transport_init(it->widget);
    return no_children(b, n);
}

// A number typed or stepped over range=LO..HI (needed: a guessed range
// would refuse what the program means to offer), moving by step=, its unit
// drawn after it, starting at value= or LO.
static int range_attr(struct udecl *d, struct udecl_node *n, int *lo, int *hi);
static int build_spinbox(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    struct udecl *d = &b->d;
    int lo, hi, step = 1, r = range_attr(d, n, &lo, &hi);
    if (r < 0) return -1;
    if (!r) return udecl_fail(d, n->line, "spinbox needs range=LO..HI, the numbers it offers");
    if (udecl_int(d, n, "step", 1, 1000000, &step) < 0) return -1;
    int value = lo;
    if (udecl_int(d, n, "value", lo, hi, &value) < 0) return -1;
    uui_spinbox_init(it->widget, value, lo, hi, step, udecl_get(d, n, "unit"));
    return no_children(b, n);
}

// Qt Designer's PROMOTED WIDGET: a place the program fills with a widget of
// its own (a gallery, a web view) -- it sets the item's ops and widget
// through uui_builder_item() before the window is asked for, which refuses
// one left empty.
static int build_app(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir; (void)it;
    if (!n->name) return udecl_fail(&b->d, n->line, "app needs a #name: the program gives that place its widget");
    return no_children(b, n);
}

// A tree; its nodes are the program's (uui_tree_set_nodes()).
static int build_tree(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    uui_tree_init(it->widget, 0, 0, 0, 0, 0, 0);
    return no_children(b, n);
}

// QBoxLayout::addSpacing(): `space size=medium`, along its row or column.
static int build_space(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w;
    struct uui_space *sp = it->widget;
    if (dir != UUI_ROW && dir != UUI_COLUMN) return udecl_fail(&b->d, n->line, "space belongs in a row or a column");
    int r = length(&b->d, n, "size", &sp->v, &sp->unit);
    if (r < 0) return -1;
    if (!r) return udecl_fail(&b->d, n->line, "space needs size= -- a step (medium), pixels or lh/ch");
    sp->axis = dir;
    return no_children(b, n);
}

// A bar of a top-level `menu #name` -- which the file may declare after
// the window: an unbuilt one is built here, where the bar needs it.
static struct bmenu *menu_named(struct uui_builder *b, const char *name);
static int build_menu(struct uui_builder *b, struct udecl_node *n, struct bmenu *m);
static int build_menubar(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    const char *name = udecl_get(&b->d, n, "menu");
    if (!name) return udecl_fail(&b->d, n->line, "menubar needs menu=, the #name of a menu in this file");
    struct bmenu *m = menu_named(b, name);
    if (!m) return udecl_fail(&b->d, n->line, "menubar names menu #%s, which the file lacks", name);
    if (!m->items && build_menu(b, m->node, m) < 0) return -1;
    uui_menubar_init(it->widget, m->items, m->count);
    return no_children(b, n);
}

typedef int (*focus_fn)(void *obj, struct uui_focusable *out, int max);

static const struct btype {
    const char *word;
    const struct uui_widget_ops *ops;
    size_t size;
    build_fn build;
    // How Tab reaches it, when not through `ops`: a focus-only table, or a
    // composite's own list. Neither and no ops->key: not focusable.
    const struct uui_widget_ops *focus_ops;
    focus_fn focusables;
    int nfocus;   // how many focusables() gives
} TYPES[] = {
    { .word = "row", .ops = &uui_layout_ops, .size = sizeof(struct uui_layout), .build = build_row },
    { .word = "column", .ops = &uui_layout_ops, .size = sizeof(struct uui_layout), .build = build_column },
    { .word = "grid", .ops = &uui_layout_ops, .size = sizeof(struct uui_layout), .build = build_grid },
    { .word = "label", .ops = &uui_label_ops, .size = sizeof(struct uui_label), .build = build_label },
    { .word = "stretch", .ops = &uui_label_ops, .size = sizeof(struct uui_label), .build = build_stretch },
    { .word = "button", .ops = &uui_button_ops, .size = sizeof(struct uui_button), .build = build_button },
    { .word = "toolbar", .ops = &uui_toolbar_ops, .size = sizeof(struct uui_toolbar), .build = build_toolbar },
    { .word = "table", .ops = &uui_table_ops, .size = sizeof(struct uui_table), .build = build_table },
    { .word = "markdown", .ops = &uui_markdown_ops, .size = sizeof(struct uui_markdown), .build = build_markdown },
    { .word = "dropdown", .ops = &uui_dropdown_ops, .size = sizeof(struct uui_dropdown), .build = build_dropdown, .focus_ops = &uui_dropdown_focus_ops },
    { .word = "listbox", .ops = &uui_listbox_ops, .size = sizeof(struct uui_listbox), .build = build_listbox, .focus_ops = &uui_listbox_focus_ops },
    { .word = "checkbox", .ops = &uui_checkbox_ops, .size = sizeof(struct uui_checkbox), .build = build_checkbox },
    { .word = "textbox", .ops = &uui_textbox_ops, .size = sizeof(struct uui_textbox), .build = build_textbox, .focus_ops = &uui_textbox_focus_ops },
    { .word = "progress", .ops = &uui_progress_ops, .size = sizeof(struct uui_progress), .build = build_progress },
    { .word = "custom", .ops = &uui_custom_ops, .size = sizeof(struct uui_custom), .build = build_custom },
    { .word = "themepreview", .ops = &uui_themepreview_ops, .size = sizeof(struct uui_themepreview), .build = build_themepreview },
    { .word = "dialog", .ops = &uui_dialog_ops, .size = sizeof(struct uui_dialog), .build = build_dialog },
    { .word = "wizard", .ops = &uui_wizard_ops, .size = sizeof(struct uui_wizard), .build = build_wizard },
    { .word = "stack", .ops = &uui_stack_ops, .size = sizeof(struct uui_stack), .build = build_stack },
    { .word = "colourpick", .size = sizeof(struct uui_colourpick), .build = build_colourpick, .focusables = colourpick_focusables, .nfocus = UUI_COLOURPICK_FOCUSABLES },
    { .word = "menubar", .ops = &uui_menubar_ops, .size = sizeof(struct uui_menubar), .build = build_menubar },
    { .word = "segmented", .ops = &uui_segmented_ops, .size = sizeof(struct uui_segmented), .build = build_segmented },
    { .word = "statusbar", .ops = &uui_statusbar_ops, .size = sizeof(struct uui_statusbar), .build = build_statusbar },
    { .word = "loglist", .ops = &uui_loglist_ops, .size = sizeof(struct uui_loglist), .build = build_loglist },
    { .word = "space", .ops = &uui_space_ops, .size = sizeof(struct uui_space), .build = build_space },
    { .word = "meter", .ops = &uui_meter_ops, .size = sizeof(struct uui_meter), .build = build_meter },
    { .word = "chart", .ops = &uui_chart_ops, .size = sizeof(struct uui_chart), .build = build_chart },
    { .word = "image", .ops = &uui_image_ops, .size = sizeof(struct uui_image), .build = build_image },
    { .word = "thumbstrip", .ops = &uui_thumbstrip_ops, .size = sizeof(struct uui_thumbstrip), .build = build_thumbstrip },
    { .word = "transport", .ops = &uui_transport_ops, .size = sizeof(struct uui_transport), .build = build_transport },
    { .word = "spinbox", .ops = &uui_spinbox_ops, .size = sizeof(struct uui_spinbox), .build = build_spinbox },
    { .word = "tree", .ops = &uui_tree_ops, .size = sizeof(struct uui_tree), .build = build_tree },
    { .word = "app", .build = build_app },   // ops, widget: the program's
};

static int focusable(const struct btype *t, const struct uui_item *it) {
    return t->focusables || t->focus_ops || (it->ops && it->ops->key);
}

static const struct btype *type_of(const char *word) {
    for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
        if (!strcmp(TYPES[i].word, word)) return &TYPES[i];
    return 0;
}

static int find_named_n(struct bwin *w, const char *name, size_t len) {
    for (int i = 0; i < w->nnamed; i++)
        if (strlen(w->named[i].name) == len && !memcmp(w->named[i].name, name, len)) return i;
    return -1;
}
static struct named *find_named(struct bwin *w, const char *name) {
    int i = find_named_n(w, name, strlen(name));
    return i >= 0 ? &w->named[i] : 0;
}
static struct named *named_here(struct bwin *w, const char *name) { return find_named(w, name); }

static int build_item(struct uui_builder *b, struct bwin *w, struct udecl_node *n,
                      struct uui_item *it, int dir) {
    struct udecl *d = &b->d;
    const struct btype *t = type_of(n->type);
    if (!t) return udecl_fail(d, n->line, "there is no widget type '%s'", n->type);
    it->ops = t->ops;
    void *obj = it->widget = alloc(b, t->size, n->line);
    if (!obj && t->size) return -1;
    b->widgets++;
    if (n->name) {
        if (find_named(w, n->name)) return udecl_fail(d, n->line, "window %s has two #%s", w->name, n->name);
        if (!UGROW(w->named, w->named_cap, w->nnamed + 1)) return udecl_fail(d, n->line, "out of memory");
        w->named[w->nnamed++] = (struct named){ n->name, it, obj, t, w->cur_page, 0, 0, n->line };
        it->name = n->name;
        lookup(b, n->name, &it->id);
    }
    if (udecl_flag(&b->d, n, "fill")) it->flags |= UUI_FILL_W | UUI_FILL_H;
    if (udecl_flag(&b->d, n, "fill-w")) it->flags |= UUI_FILL_W;
    if (udecl_flag(&b->d, n, "fill-h")) it->flags |= UUI_FILL_H;
    if (udecl_flag(&b->d, n, "track-hover")) it->flags |= UUI_TRACK_HOVER;
    if (udecl_flag(&b->d, n, "center")) {
        if (dir != UUI_ROW && dir != UUI_COLUMN) return udecl_fail(d, n->line, "center belongs in a row or a column");
        it->flags |= UUI_CENTER;
    }
    it->hidden = udecl_flag(&b->d, n, "hidden");
    // The pin is along the PARENT's axis, so each word is legal in one.
    int r;
    if ((r = udecl_int(d, n, "width", 1, 1000, &it->main_cells)) < 0) return -1;
    if (r && dir != UUI_ROW) return udecl_fail(d, n->line, "width= sizes a row's child; this is not in a row");
    const char *hv = udecl_get(d, n, "height");
    if (hv && !strcmp(hv, "control")) {
        it->flags |= UUI_CONTROL_H;   // the theme's one-line control height, in a row or a column
    } else if ((r = udecl_int(d, n, "height", 1, 1000, &it->main_cells)) < 0) {
        return -1;
    } else if (r && dir != UUI_COLUMN) {
        return udecl_fail(d, n->line, "height= sizes a column's child; this is not in a column "
                          "(height=control works in either)");
    }
    if (t->build(b, w, n, it, dir) < 0) return -1;
    return udecl_unused(d, n);
}

// Every item, into any container (a layout, a stack, a composite's own).
static int collect(struct uui_item *items, int count, struct uui_item ***all, int *n, int *cap) {
    for (int i = 0; i < count; i++) {
        struct uui_item *it = &items[i];
        if (!UGROW(*all, *cap, *n + 1)) return -1;
        (*all)[(*n)++] = it;
        if (it->ops && it->ops->children) {
            int k = 0;
            struct uui_item *sub = it->ops->children(it->widget, &k);
            if (sub && collect(sub, k, all, n, cap) < 0) return -1;
        }
    }
    return 0;
}

// uapp REFUSES TO START a window with an id twice (ids_unique(), with a
// log line only), so the builder says it first, where it can be shown: two
// buttons with one command, or a #name that is also another's command.
static int ids_differ(struct uui_builder *b, struct bwin *w) {
    struct uui_item **all = 0;
    int n = 0, cap = 0, rc = 0;
    if (collect(w->top, w->ntop, &all, &n, &cap) < 0) rc = udecl_fail(&b->d, w->line, "out of memory");
    for (int i = 0; !rc && i < n; i++)
        for (int j = i + 1; !rc && j < n; j++)
            if (all[i]->id && all[i]->id == all[j]->id)
                rc = udecl_fail(&b->d, w->line, "window #%s: %s%s and %s%s both answer to the app as %d",
                                w->name, all[i]->name ? "#" : "", all[i]->name ? all[i]->name : "a widget",
                                all[j]->name ? "#" : "", all[j]->name ? all[j]->name : "another",
                                all[i]->id);
    free(all);
    return rc;
}

static int is_container(const char *type) {
    return !strcmp(type, "row") || !strcmp(type, "column") || !strcmp(type, "grid");
}

// --- menus --------------------------------------------------------------

// A menu's rows: `item`, `separator`, and `menu "Title" { ... }` for a
// submenu. `depth` is the popup level these rows open at when the menu is
// a context menu (1), the deepest use, so a file that loads can be opened
// to its last level either way -- uui_menubar opens no level past
// UUI_MENU_MAX_DEPTH, which would be a submenu that silently never shows.
static int build_menu_rows(struct uui_builder *b, struct udecl_node *n, int depth,
                           struct uui_menu_item **out, int *count) {
    struct udecl *d = &b->d;
    if (!n->nkid) return udecl_fail(d, n->line, "this menu holds nothing");
    if (depth > UUI_MENU_MAX_DEPTH)
        return udecl_fail(d, n->line, "menus nest at most %d deep", UUI_MENU_MAX_DEPTH);
    struct uui_menu_item *rows = alloc(b, sizeof *rows * (size_t)n->nkid, n->line);
    if (!rows) return -1;
    int strip = 1;   // still in the leading run of strip buttons
    for (int i = 0; i < n->nkid; i++) {
        struct udecl_node *k = &n->kid[i];
        struct uui_menu_item *mi = &rows[i];
        if (k->name) return udecl_fail(d, k->line, "a menu row is known by command=, not by a #name");
        if (!strcmp(k->type, "separator")) {
            strip = 0;
            if (no_children(b, k) < 0 || udecl_unused(d, k) < 0) return -1;
            continue;   // all zero: UUI_MENU_SEP
        }
        int sub = !strcmp(k->type, "menu");
        if (!sub && strcmp(k->type, "item"))
            return udecl_fail(d, k->line, "a menu holds item, separator and menu, not %s", k->type);
        mi->label = udecl_text(k);
        if (!mi->label) return udecl_fail(d, k->line, "%s needs its text, in quotes", k->type);
        const char *word = udecl_get(d, k, "command");
        if (sub) {
            // A submenu's command is never sent, only asked about: item_flags
            // can grey a whole branch (an empty Recent list).
            if (word && command(b, k, word, &mi->code) < 0) return -1;
            struct uui_menu_item *subrows;
            if (build_menu_rows(b, k, depth + 1, &subrows, &mi->sub_count) < 0) return -1;
            mi->sub = subrows;
        } else {
            if (no_children(b, k) < 0 || command(b, k, word, &mi->code) < 0) return -1;
            mi->accel = udecl_get(d, k, "accel");
        }
        mi->icon = udecl_get(d, k, "icon");
        int role = 0, r = udecl_word(d, k, "role", ROLES, UTHEME_ACT_COUNT, &role);
        if (r < 0) return -1;
        if (r && !mi->icon) return udecl_fail(d, k->line, "role= colours the row's icon: give it icon= too");
        mi->tint = (uint32_t)role;
        if (udecl_flag(d, k, "strip")) {
            // The widget lays out only a LEADING run, and an icon is all a strip button shows.
            if (!strip) return udecl_fail(d, k->line, "strip buttons come first in a menu, before any other row");
            if (sub || !mi->icon) return udecl_fail(d, k->line, "a strip button is an item with icon=");
            mi->style = UUI_MIS_STRIP;
        } else {
            strip = 0;
        }
        if (udecl_unused(d, k) < 0) return -1;
    }
    *out = rows;
    *count = n->nkid;
    return 0;
}

// `menu #name { ... }` beside the windows: a bar's titles, or a context
// menu's rows -- Win32's .rc MENU, GtkBuilder's <menu>. A program whose
// window is still C can take its menus from here alone.
static int build_menu(struct uui_builder *b, struct udecl_node *n, struct bmenu *m) {
    struct udecl *d = &b->d;
    if (udecl_text(n)) return udecl_fail(d, n->line, "menu #%s takes no text: its rows are what show", n->name);
    if (build_menu_rows(b, n, 1, &m->items, &m->count) < 0) return -1;
    return udecl_unused(d, n);
}

static int build_window(struct uui_builder *b, struct udecl_node *n, struct bwin *w) {
    struct udecl *d = &b->d;
    if (!n->name) return udecl_fail(d, n->line, "a window needs a #name");
    for (int i = 0; i < b->nwin; i++)
        if (!strcmp(b->win[i].name, n->name)) return udecl_fail(d, n->line, "two windows are called #%s", n->name);
    w->name = n->name;
    w->line = n->line;
    w->title = udecl_get(&b->d, n, "title");
    w->app_id = udecl_get(&b->d, n, "app-id");
    w->resizable = udecl_flag(&b->d, n, "resizable");
    w->autofocus = udecl_flag(&b->d, n, "autofocus");
    if (udecl_pair(d, n, "size", 1, 1000, &w->cells_w, &w->cells_h) < 0) return -1;
    if (udecl_pair(d, n, "min", 0, 100000, &w->min_w, &w->min_h) < 0) return -1;
    // One container, and any number of `dialog` lines and FLOATING widgets
    // beside it: routed and drawn, never laid out -- the program places a
    // floating one itself (Qt's child placed by setGeometry()).
    int root = -1;
    for (int i = 0; i < n->nkid; i++) {
        if (!strcmp(n->kid[i].type, "dialog") || udecl_flag(d, &n->kid[i], "floating")) continue;
        if (root >= 0 || !is_container(n->kid[i].type))
            return udecl_fail(d, n->kid[i].line, "a window holds one row, column or grid, and dialogs "
                              "and floating widgets beside it");
        root = i;
    }
    if (root < 0) return udecl_fail(d, n->line, "a window holds one row, column or grid");
    w->ntop = n->nkid;
    w->top = alloc(b, sizeof *w->top * (size_t)w->ntop, n->line);
    if (!w->top) return -1;
    w->root_item = &w->top[0];
    if (build_item(b, w, &n->kid[root], w->root_item, -1) < 0) return -1;
    w->building_top = 1;
    for (int i = 0, k = 1; i < n->nkid; i++) {
        if (i == root) continue;
        struct uui_item *it = &w->top[k++];
        if (build_item(b, w, &n->kid[i], it, -1) < 0) return -1;
        // Nothing lays a floating widget out: these would be read and ignored.
        if (it->flags & (UUI_FILL_W | UUI_FILL_H | UUI_CONTROL_H | UUI_CENTER))
            return udecl_fail(d, n->kid[i].line, "the program places a floating widget: no fill, height= or center");
    }
    w->building_top = 0;
    w->root = w->root_item->widget;
    if (ids_differ(b, w) < 0) return -1;
    // After the tree, so every name it lists exists.
    const char *focus = udecl_get(&b->d, n, "focus");
    if (focus) {
        for (const char *p = focus; *p;) {
            while (*p == ' ') p++;
            const char *e = p;
            while (*e && *e != ' ') e++;
            if (e == p) break;
            int len = (int)(e - p);
            int k = find_named_n(w, p, (size_t)len);
            if (k < 0) return udecl_fail(d, n->line, "focus= names #%.*s, which this window lacks", len, p);
            // An app's ops are the program's, given after the load: asked at uui_builder_window().
            if (strcmp(w->named[k].type->word, "app") && !focusable(w->named[k].type, w->named[k].item))
                return udecl_fail(d, n->line, "#%.*s takes no keys, so it cannot be focused", len, p);
            if (!UGROW(w->focus, w->focus_cap, w->nfocus + 1)) return udecl_fail(d, n->line, "out of memory");
            w->focus[w->nfocus++] = k;
            p = e;
        }
    }
    if (w->autofocus && !w->nfocus) return udecl_fail(d, n->line, "autofocus needs a focus= list to start it on");
    return udecl_unused(d, n);
}

static struct bwin *window_of(struct uui_builder *b, const char *window);

// --- Options windows ------------------------------------------------------

// A control on an Options row. A BOUND one (check, choice, number,
// fontsize) names a setting by its key; uui_builder_options() finds it in
// the program's tables and fills `field`. A widget is any type the builder
// knows, built as in a window, or an `app` the program fills.
enum { OC_CHECK, OC_CHOICE, OC_NUMBER, OC_FONTSIZE, OC_WIDGET, OC_ROWS };
struct bocontrol {
    int kind, line;
    const char *name;           // the row's log name; a widget's lookup
    const char *key;
    const char *text;           // a check's, a fontsize's box
    const char *unit;
    int step, lo, hi, has_range;
    const char **labels, **words;   // a choice's segments, and the value= each stores
    int *values, count;
    struct uui_item *item;      // a widget's
    size_t field;               // bound: offsetof in the program's edit struct
};
struct borow {
    const char *caption;
    struct bocontrol *c;        // 1..3: what uui_prefs_row() and uui_prefs_also*() place
    int n;
};
struct bopage {
    const char *name, *note;
    struct borow *rows;
    int nrows, line;
};
struct bopts {
    struct bopage *pages;
    int npages, cells_w, cells_h, bound;
    struct uui_sidebar_row *side;   // a page's sidebar row; its id is its place in the file
    const char **notes;             // by page, or NULL when no page has one
};

static const char *const CONTROL_WORDS[] = {
    [OC_CHECK] = "check", [OC_CHOICE] = "choice", [OC_NUMBER] = "number", [OC_FONTSIZE] = "fontsize",
};
// Widgets that make sense as a row's control; a container or a bar does not.
static const char *const ROW_WIDGETS[] = { "label", "button", "checkbox", "textbox", "dropdown", "segmented",
                                           "spinbox", "table", "app" };

static int control_kind(const char *type) {
    for (int k = OC_CHECK; k <= OC_FONTSIZE; k++)
        if (!strcmp(CONTROL_WORDS[k], type)) return k;
    if (!strcmp(type, "rows")) return OC_ROWS;
    for (size_t i = 0; i < sizeof ROW_WIDGETS / sizeof ROW_WIDGETS[0]; i++)
        if (!strcmp(ROW_WIDGETS[i], type)) return OC_WIDGET;
    return -1;
}

// "LO..HI": 1 read, 0 absent, -1 the error set.
static int range_attr(struct udecl *d, struct udecl_node *n, int *lo, int *hi) {
    const char *s = udecl_get(d, n, "range");
    if (!s) return 0;
    char *e;
    long a = strtol(s, &e, 10);
    if (e == s || e[0] != '.' || e[1] != '.') goto bad;
    const char *t = e + 2;
    long z = strtol(t, &e, 10);
    if (e == t || *e || a > z || a < -1000000 || z > 1000000) goto bad;
    *lo = (int)a;
    *hi = (int)z;
    return 1;
bad:
    return udecl_fail(d, n->line, "range=%s is not LO..HI, two whole numbers, the first no larger", s);
}

static int build_control(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct bocontrol *c,
                         int first) {
    struct udecl *d = &b->d;
    int kind = control_kind(n->type);
    if (kind < 0)
        return udecl_fail(d, n->line, "an Options row holds check, choice, number, fontsize, rows, or a label, "
                          "button, checkbox, textbox, dropdown, segmented, spinbox, table or app -- not %s", n->type);
    c->kind = kind;
    c->line = n->line;
    c->name = n->name;
    if (kind == OC_ROWS) {
        if (!n->name) return udecl_fail(d, n->line, "rows needs a #name: the program adds those rows");
        if (!first) return udecl_fail(d, n->line, "rows is a run of rows, not a control in one");
        return no_children(b, n) < 0 ? -1 : udecl_unused(d, n);
    }
    if (kind == OC_WIDGET) {
        c->item = alloc(b, sizeof *c->item, n->line);
        if (!c->item || build_item(b, w, n, c->item, UUI_ROW) < 0) return -1;
        // The window places the row: these would be read and ignored.
        if (c->item->hidden || (c->item->flags & (UUI_CENTER | UUI_CONTROL_H | UUI_TRACK_HOVER)))
            return udecl_fail(d, n->line, "an Options row places its controls: no hidden, center, height= or "
                              "track-hover");
        if (!first && (c->item->main_cells || c->item->flags))
            return udecl_fail(d, n->line, "only a row's first control takes width= or fill");
        if (c->item->id >= UUI_PREFS_ID_BASE)
            return udecl_fail(d, n->line, "#%s's id %d is the Options window's own (%d and up)", n->name,
                              c->item->id, UUI_PREFS_ID_BASE);
        return 0;   // build_item read the rest
    }
    c->key = udecl_get(d, n, "key");
    if (!c->key) return udecl_fail(d, n->line, "%s needs key=, the setting it edits", n->type);
    if (!c->name) c->name = c->key;
    if (!first && kind != OC_CHECK && kind != OC_NUMBER)
        return udecl_fail(d, n->line, "beside a row's first control goes a check, a number or a widget, not a %s",
                          n->type);
    c->text = udecl_text(n);
    if ((kind == OC_CHECK || kind == OC_FONTSIZE) && !c->text)
        return udecl_fail(d, n->line, "%s needs its box's text, in quotes", n->type);
    if (kind == OC_NUMBER || kind == OC_FONTSIZE) {
        c->unit = udecl_get(d, n, "unit");
        if ((c->has_range = range_attr(d, n, &c->lo, &c->hi)) < 0) return -1;
        if (kind == OC_FONTSIZE && !c->has_range)
            return udecl_fail(d, n->line, "fontsize needs range=, the sizes it offers (its setting's 0 is the "
                              "desktop's)");
    }
    c->step = 1;
    if (kind == OC_NUMBER && udecl_int(d, n, "step", 1, 1000000, &c->step) < 0) return -1;
    if (kind == OC_CHOICE) {
        if (n->nkid < 2) return udecl_fail(d, n->line, "choice needs two item lines or more");
        c->count = n->nkid;
        c->labels = alloc(b, sizeof *c->labels * (size_t)c->count, n->line);
        c->words = alloc(b, sizeof *c->words * (size_t)c->count, n->line);
        c->values = alloc(b, sizeof *c->values * (size_t)c->count, n->line);
        if (!c->labels || !c->words || !c->values) return -1;
        for (int i = 0; i < c->count; i++) {
            struct udecl_node *k = &n->kid[i];
            if (strcmp(k->type, "item")) return udecl_fail(d, k->line, "choice holds item, not %s", k->type);
            if (no_children(b, k) < 0) return -1;
            if (k->name) return udecl_fail(d, k->line, "an item is known by value=, not by a #name");
            c->labels[i] = udecl_text(k);
            if (!c->labels[i]) return udecl_fail(d, k->line, "item needs its text, in quotes");
            c->words[i] = udecl_get(d, k, "value");
            if (!c->words[i])
                return udecl_fail(d, k->line, "item needs value=, what it stores -- spelt as the setting's file "
                                  "spells it");
            for (int j = 0; j < i; j++)
                if (!strcmp(c->words[j], c->words[i]))
                    return udecl_fail(d, k->line, "two items store value=%s", c->words[i]);
            if (udecl_unused(d, k) < 0) return -1;
        }
    } else if (no_children(b, n) < 0) {
        return -1;
    }
    return udecl_unused(d, n);
}

// `row "Caption" { control control [control] }`, or one control on its own
// line with caption= -- QFormLayout's addRow(label, layout) and addRow(label, field).
static int build_orow(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct borow *r) {
    struct udecl *d = &b->d;
    if (!strcmp(n->type, "row")) {
        if (n->name) return udecl_fail(d, n->line, "a row is not named: name its controls");
        r->caption = udecl_text(n);
        if (n->nkid < 2 || n->nkid > 3)
            return udecl_fail(d, n->line, "a row holds two or three controls; one goes on a line of its own, "
                              "with caption=");
        r->n = n->nkid;
        r->c = alloc(b, sizeof *r->c * (size_t)r->n, n->line);
        if (!r->c) return -1;
        for (int i = 0; i < r->n; i++) {
            if (udecl_get(d, &n->kid[i], "caption"))
                return udecl_fail(d, n->kid[i].line, "the row's text is its caption: no caption= in it");
            if (build_control(b, w, &n->kid[i], &r->c[i], i == 0) < 0) return -1;
            if (r->c[i].kind == OC_ROWS)
                return udecl_fail(d, n->kid[i].line, "rows is a run of rows, not a control in one");
        }
        return udecl_unused(d, n);
    }
    r->caption = udecl_get(d, n, "caption");
    r->n = 1;
    r->c = alloc(b, sizeof *r->c, n->line);
    if (!r->c || build_control(b, w, n, r->c, 1) < 0) return -1;
    if (r->c->kind == OC_ROWS && r->caption) return udecl_fail(d, n->line, "rows is captioned by the program");
    return 0;
}

static int build_options(struct uui_builder *b, struct udecl_node *n, struct bwin *w) {
    struct udecl *d = &b->d;
    if (!n->name) return udecl_fail(d, n->line, "options needs a #name");
    for (int i = 0; i < b->nwin; i++)
        if (!strcmp(b->win[i].name, n->name)) return udecl_fail(d, n->line, "two windows are called #%s", n->name);
    struct bopts *o = w->opts = alloc(b, sizeof *w->opts, n->line);
    if (!o) return -1;
    w->name = n->name;
    w->line = n->line;
    w->title = udecl_get(d, n, "title");
    if (udecl_pair(d, n, "size", 1, 1000, &o->cells_w, &o->cells_h) < 0) return -1;
    if (!n->nkid) return udecl_fail(d, n->line, "options holds no page");
    if (n->nkid > UUI_PREFS_PAGES_MAX)
        return udecl_fail(d, n->kid[UUI_PREFS_PAGES_MAX].line, "an Options window has at most %d pages",
                          UUI_PREFS_PAGES_MAX);
    o->npages = n->nkid;
    o->pages = alloc(b, sizeof *o->pages * (size_t)o->npages, n->line);
    o->side = alloc(b, sizeof *o->side * (size_t)o->npages, n->line);
    if (!o->pages || !o->side) return -1;
    int rows = 0, notes = 0;
    for (int p = 0; p < o->npages; p++) {
        struct udecl_node *pn = &n->kid[p];
        struct bopage *pg = &o->pages[p];
        if (strcmp(pn->type, "page")) return udecl_fail(d, pn->line, "options holds pages, not %s", pn->type);
        const char *label = udecl_text(pn);
        if (!label) return udecl_fail(d, pn->line, "page needs its title, in quotes");
        for (int q = 0; q < p; q++)
            if (pn->name && o->pages[q].name && !strcmp(o->pages[q].name, pn->name))
                return udecl_fail(d, pn->line, "two pages are called #%s", pn->name);
        pg->name = pn->name;
        pg->line = pn->line;
        pg->note = udecl_get(d, pn, "note");
        notes |= pg->note != 0;
        o->side[p] = (struct uui_sidebar_row){ .label = label, .kind = UUI_SIDEBAR_TOP, .id = p };
        if (!pn->nkid) return udecl_fail(d, pn->line, "page %s holds no row", label);
        pg->nrows = pn->nkid;
        pg->rows = alloc(b, sizeof *pg->rows * (size_t)pg->nrows, pn->line);
        if (!pg->rows) return -1;
        for (int r = 0; r < pg->nrows; r++) {
            if (build_orow(b, w, &pn->kid[r], &pg->rows[r]) < 0) return -1;
            rows += pg->rows[r].c[0].kind != OC_ROWS;
        }
        if (udecl_unused(d, pn) < 0) return -1;
    }
    if (rows > UUI_PREFS_ROWS_MAX)
        return udecl_fail(d, n->line, "an Options window has at most %d rows; this one has %d", UUI_PREFS_ROWS_MAX,
                          rows);
    if (notes) {
        o->notes = alloc(b, sizeof *o->notes * (size_t)o->npages, n->line);
        if (!o->notes) return -1;
        for (int p = 0; p < o->npages; p++) o->notes[p] = o->pages[p].note;
    }
    // A ROW'S NAME IS WHAT ITS LAYOUT LINES AND A TEST CALL IT, so two
    // controls answering to one would be two rows a test cannot tell apart.
    for (int p = 0; p < o->npages; p++)
        for (int r = 0; r < o->pages[p].nrows; r++)
            for (int i = 0; i < o->pages[p].rows[r].n; i++) {
                const struct bocontrol *c = &o->pages[p].rows[r].c[i];
                if (c->kind == OC_WIDGET || c->kind == OC_ROWS) continue;   // build_item checked those
                if (find_named(w, c->name)) return udecl_fail(d, c->line, "options #%s has two #%s", w->name, c->name);
                for (int q = 0; q <= p; q++)
                    for (int s = 0; s < o->pages[q].nrows && (q < p || s <= r); s++)
                        for (int k = 0; k < o->pages[q].rows[s].n; k++) {
                            const struct bocontrol *e = &o->pages[q].rows[s].c[k];
                            if (e == c) goto next;
                            if (e->kind != OC_WIDGET && e->kind != OC_ROWS && !strcmp(e->name, c->name))
                                return udecl_fail(d, c->line, "options #%s has two #%s (key= names one that "
                                                  "has no #name)", w->name, c->name);
                        }
            next:;
            }
    return udecl_unused(d, n);
}

static struct bopts *options_of(struct uui_builder *b, const char *options) {
    struct bwin *w = window_of(b, options);
    if (w && !w->opts) udecl_fail(&b->d, w->line, "#%s is a window, not an options", options);
    return w ? w->opts : 0;
}

static const char *const ONOFF[] = { "off", "on" };

// One control's setting, found in the program's tables.
static int bind_control(struct uui_builder *b, struct bocontrol *c, const struct uui_builder_prefs *t, int nt) {
    struct udecl *d = &b->d;
    const struct upref *k = 0;
    size_t base = 0;
    for (int i = 0; i < nt; i++)
        for (int j = 0; j < t[i].prefs->count; j++) {
            if (strcmp(t[i].prefs->keys[j].key, c->key)) continue;
            if (k) return udecl_fail(d, c->line, "two of the program's settings files have key=%s", c->key);
            k = &t[i].prefs->keys[j];
            base = t[i].base;
        }
    if (!k) return udecl_fail(d, c->line, "the program has no setting key=%s", c->key);
    static const char *const TYPE_NAMES[] = { "on/off", "number", "word", "text" };
    int want_ok = c->kind == OC_CHECK ? k->type == UPREF_BOOL
                : c->kind == OC_CHOICE ? k->type != UPREF_TEXT
                : k->type == UPREF_INT;
    if (!want_ok)
        return udecl_fail(d, c->line, "%s edits %s, and key=%s is a %s setting", CONTROL_WORDS[c->kind],
                          c->kind == OC_CHECK ? "an on/off setting" : c->kind == OC_CHOICE
                          ? "an on/off, number or word setting" : "a number setting", c->key,
                          TYPE_NAMES[k->type & 3]);
    if (k->type == UPREF_INT && (c->kind == OC_NUMBER || c->kind == OC_FONTSIZE)) {
        if (!c->has_range) { c->lo = k->lo; c->hi = k->hi; }
        else if (c->lo < k->lo || c->hi > k->hi || (c->kind == OC_FONTSIZE && c->lo < 1))
            return udecl_fail(d, c->line, "range=%d..%d reaches past key=%s's own %d..%d%s", c->lo, c->hi, c->key,
                              c->kind == OC_FONTSIZE && k->lo < 1 ? 1 : k->lo, k->hi,
                              c->kind == OC_FONTSIZE ? " (its 0 is the desktop's, the box)" : "");
        if (c->kind == OC_FONTSIZE && k->lo > 0)
            return udecl_fail(d, c->line, "fontsize stores 0 for the desktop's size, which key=%s cannot hold", c->key);
    }
    for (int i = 0; c->kind == OC_CHOICE && i < c->count; i++) {
        const char *v = c->words[i];
        int got = -1;
        if (k->type == UPREF_BOOL) {
            for (int x = 0; x < 2; x++) if (!strcmp(v, ONOFF[x])) got = x;
        } else if (k->type == UPREF_WORD) {
            for (int x = 0; x < k->hi; x++) if (!strcmp(v, k->words[x])) got = x;
        } else {
            char *e;
            long x = strtol(v, &e, 10);
            if (e != v && !*e && x >= k->lo && x <= k->hi) got = (int)x;
        }
        if (got < 0) {
            char said[160] = "";
            if (k->type == UPREF_BOOL) snprintf(said, sizeof said, "on or off");
            else if (k->type == UPREF_INT) snprintf(said, sizeof said, "a number in %d..%d", k->lo, k->hi);
            else
                for (int x = 0; x < k->hi; x++)
                    snprintf(said + strlen(said), sizeof said - strlen(said), "%s%s", x ? ", " : "", k->words[x]);
            return udecl_fail(d, c->line, "key=%s cannot store value=%s: it takes %s", c->key, v, said);
        }
        c->values[i] = got;
    }
    c->field = base + k->off;
    return 0;
}

int uui_builder_options(struct uui_builder *b, const char *options, const struct uui_builder_prefs *tables,
                        int count) {
    struct bopts *o = options_of(b, options);
    if (!o) return 0;
    for (int p = 0; p < o->npages; p++)
        for (int r = 0; r < o->pages[p].nrows; r++)
            for (int i = 0; i < o->pages[p].rows[r].n; i++) {
                struct bocontrol *c = &o->pages[p].rows[r].c[i];
                if (c->key && bind_control(b, c, tables, count) < 0) return 0;
            }
    o->bound = 1;
    return 1;
}

// The row's first control; a widget's own name and id, a bound one's row.
static int place_first(const struct borow *r, int page) {
    const struct bocontrol *c = &r->c[0];
    switch (c->kind) {
    case OC_CHECK: return uui_prefs_check(page, r->caption, c->text, c->field, c->name);
    case OC_CHOICE: return uui_prefs_choice(page, r->caption, c->labels, c->values, c->count, c->field, c->name);
    case OC_NUMBER: return uui_prefs_number(page, r->caption, c->lo, c->hi, c->step, c->unit, c->field, c->name);
    case OC_FONTSIZE:
        return uui_prefs_fontsize(page, r->caption, c->lo, c->hi, c->unit, c->text, c->field, c->name);
    default: {
        const struct uui_item *it = c->item;
        int row = uui_prefs_row(page, r->caption, it->ops, it->widget, it->id, it->name, it->flags);
        if (it->main_cells) uui_prefs_widen(row, ugfx_char_advance('n') * it->main_cells);
        return row;
    }
    }
}

int uui_builder_options_open(struct uui_builder *b, const char *options, struct uui_prefs_desc *d,
                             struct uapp *a, const char *first_page, void (*rows)(const char *name, int page)) {
    if (uui_prefs_is_open()) return 0;
    struct bopts *o = options_of(b, options);
    if (!o) {
        ulogf("uui_builder: %s\n", uui_builder_error(b));
        return 0;
    }
    if (!o->bound) {
        ulogf("uui_builder: options #%s opened before uui_builder_options() bound it\n", options);
        return 0;
    }
    for (int p = 0; p < o->npages; p++)
        for (int r = 0; r < o->pages[p].nrows; r++)
            for (int i = 0; i < o->pages[p].rows[r].n; i++) {
                const struct bocontrol *c = &o->pages[p].rows[r].c[i];
                if (c->kind == OC_WIDGET && !c->item->ops) {
                    ulogf("uui_builder: options #%s: the program gave app #%s no widget\n", options, c->name);
                    return 0;
                }
                if (c->kind == OC_ROWS && !rows) {
                    ulogf("uui_builder: options #%s: rows #%s, and the program adds none\n", options, c->name);
                    return 0;
                }
            }
    struct bwin *w = window_of(b, options);
    if (w->title) d->title = w->title;
    d->pages = o->side;
    d->page_count = o->npages;
    if (o->cells_w) {
        d->w = ugfx_char_advance('n') * o->cells_w;
        d->h = ugfx_char_h() * o->cells_h;
    }
    d->notes = o->notes;
    d->first_page = 0;
    for (int p = 0; first_page && p < o->npages; p++)
        if (o->pages[p].name && !strcmp(o->pages[p].name, first_page)) d->first_page = p;
    uui_prefs_begin(d);
    for (int p = 0; p < o->npages; p++)
        for (int r = 0; r < o->pages[p].nrows; r++) {
            const struct borow *row = &o->pages[p].rows[r];
            if (row->c[0].kind == OC_ROWS) {
                rows(row->c[0].name, p);
                continue;
            }
            int at = place_first(row, p);
            for (int i = 1; i < row->n; i++) {
                const struct bocontrol *c = &row->c[i];
                if (c->kind == OC_CHECK) uui_prefs_also_check(at, c->text, c->field, c->name);
                else if (c->kind == OC_NUMBER) uui_prefs_also_number(at, c->lo, c->hi, c->step, c->unit, c->field, c->name);
                else uui_prefs_also(at, c->item->ops, c->item->widget, c->item->id, c->item->name);
            }
        }
    return uui_prefs_open(a);
}

static struct uui_builder *build(struct uui_builder *b, int parsed) {
    if (parsed < 0) return b;
    struct udecl_node *top = &b->d.top;
    if (!top->nkid) { udecl_fail(&b->d, 0, "the file declares no window, menu or options"); return b; }
    b->win = alloc(b, sizeof *b->win * (size_t)top->nkid, 0);
    b->menu = alloc(b, sizeof *b->menu * (size_t)top->nkid, 0);
    if (!b->win || !b->menu) return b;
    b->win_cap = top->nkid;
    // THE MENUS' NAMES FIRST, so a window's `menubar menu=` finds one
    // declared below it; then everything IN FILE ORDER, so the error kept --
    // the first -- is the one nearest the top, the one a person fixes first.
    for (int i = 0; i < top->nkid; i++) {
        struct udecl_node *n = &top->kid[i];
        if (!strcmp(n->type, "menu")) {
            if (!n->name) { udecl_fail(&b->d, n->line, "a menu needs a #name"); return b; }
            if (menu_named(b, n->name)) { udecl_fail(&b->d, n->line, "two menus are called #%s", n->name); return b; }
            b->menu[b->nmenu++] = (struct bmenu){ .name = n->name, .node = n };
        } else if (strcmp(n->type, "window") && strcmp(n->type, "options")) {
            udecl_fail(&b->d, n->line, "the file holds windows, menus and options, not %s", n->type);
            return b;
        }
    }
    for (int i = 0; i < top->nkid; i++) {
        struct udecl_node *n = &top->kid[i];
        if (!strcmp(n->type, "menu")) {
            struct bmenu *m = menu_named(b, n->name);
            if (!m->items && build_menu(b, n, m) < 0) break;   // a bar above may have built it
            continue;
        }
        int rc = !strcmp(n->type, "options") ? build_options(b, n, &b->win[b->nwin])
                                             : build_window(b, n, &b->win[b->nwin]);
        if (rc < 0) break;
        b->nwin++;
    }
    return b;
}

static struct uui_builder *start(const struct uui_builder_name *names, int count) {
    struct uui_builder *b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->names = names;
    b->nnames = count;
    return b;
}

static void finish(struct uui_builder *b, unsigned long long t0) {
    b->us = (unsigned)((sys_monotonic_ns() - t0) / 1000);
    if (b->d.error[0]) ulogf("uui_builder: %s\n", b->d.error);
    else ulogf("uui_builder: %s: %d windows, %d menus, %d widgets, %u us\n", b->d.what, b->nwin, b->nmenu,
               b->widgets, b->us);   // an options counts as a window
}

struct uui_builder *uui_builder_load(const char *app, const struct uui_builder_name *names, int count) {
    unsigned long long t0 = sys_monotonic_ns();
    struct uui_builder *b = start(names, count);
    if (!b) return 0;
    size_t len = sizeof UUI_BUILDER_DIR "/" ".ui" + strlen(app);
    char *path = malloc(len);
    if (!path) { free(b); return 0; }
    snprintf(path, len, UUI_BUILDER_DIR "/%s.ui", app);
    build(b, udecl_load(&b->d, path));
    free(path);
    finish(b, t0);
    return b;
}

struct uui_builder *uui_builder_parse(const char *what, const char *src, unsigned long len,
                                      const struct uui_builder_name *names, int count) {
    unsigned long long t0 = sys_monotonic_ns();
    struct uui_builder *b = start(names, count);
    if (!b) return 0;
    build(b, udecl_parse(&b->d, what, src, len));
    finish(b, t0);
    return b;
}

void uui_builder_free(struct uui_builder *b) {
    if (!b) return;
    for (int i = 0; i < b->win_cap; i++) {
        free(b->win[i].named);
        free(b->win[i].focus);
    }
    for (int i = 0; i < b->nmem; i++) free(b->mem[i]);
    free(b->mem);
    udecl_free(&b->d);
    free(b);
}

const char *uui_builder_error(const struct uui_builder *b) {
    if (!b) return "out of memory";
    return b->d.error[0] ? b->d.error : 0;
}

unsigned uui_builder_build_us(const struct uui_builder *b) { return b ? b->us : 0; }

static struct bwin *window_of(struct uui_builder *b, const char *window) {
    if (!b || b->d.error[0]) return 0;
    for (int i = 0; i < b->nwin; i++)
        if (!strcmp(b->win[i].name, window)) return &b->win[i];
    udecl_fail(&b->d, 0, "the app asked for window #%s, which the file lacks", window);
    return 0;
}

static struct named *named_of(struct uui_builder *b, const char *window, const char *name) {
    struct bwin *w = window_of(b, window);
    if (!w) return 0;
    struct named *nm = find_named(w, name);
    if (!nm) udecl_fail(&b->d, w->line, "the app needs #%s in window #%s", name, window);
    return nm;
}

struct uui_item *uui_builder_item(struct uui_builder *b, const char *window, const char *name) {
    struct named *nm = named_of(b, window, name);
    return nm ? nm->item : 0;
}

void *uui_builder_widget(struct uui_builder *b, const char *window, const char *name, const char *type) {
    struct named *nm = named_of(b, window, name);
    if (!nm) return 0;
    if (type && strcmp(nm->type->word, type)) {
        udecl_fail(&b->d, 0, "#%s in window #%s is a %s; the app expects a %s", name, window,
                   nm->type->word, type);
        return 0;
    }
    return nm->obj;
}

static struct bmenu *menu_named(struct uui_builder *b, const char *name) {
    for (int i = 0; i < b->nmenu; i++)
        if (!strcmp(b->menu[i].name, name)) return &b->menu[i];
    return 0;
}

const struct uui_menu_item *uui_builder_menu(struct uui_builder *b, const char *menu, int *count) {
    if (count) *count = 0;
    if (!b || b->d.error[0]) return 0;
    struct bmenu *m = menu_named(b, menu);
    if (m) {
        if (count) *count = m->count;
        return m->items;
    }
    udecl_fail(&b->d, 0, "the app asked for menu #%s, which the file lacks", menu);
    return 0;
}

struct uui_wizard *uui_builder_wizard(struct uui_builder *b, const char *window, const char *name, int steps) {
    struct uui_wizard *wz = uui_builder_widget(b, window, name, "wizard");
    if (wz && wz->d.count != steps) {
        udecl_fail(&b->d, named_of(b, window, name)->line, "#%s has %d steps; the program has %d -- steps may be "
                   "edited, not added or removed", name, wz->d.count, steps);
        return 0;
    }
    return wz;
}

struct uui_item *uui_builder_page(struct uui_builder *b, const char *window, const char *stack, const char *page) {
    struct uui_stack *s = uui_builder_widget(b, window, stack, "stack");
    struct uui_item *it = s ? uui_builder_item(b, window, page) : 0;
    if (!it) return 0;
    for (int i = 0; i < s->count; i++)
        if (&s->pages[i] == it) return it;
    udecl_fail(&b->d, 0, "#%s in window #%s is not a page of #%s", page, window, stack);
    return 0;
}

static struct named *chooser(struct uui_builder *b, const char *window, const char *name) {
    struct named *nm = named_of(b, window, name);
    if (nm && !nm->ids) {
        udecl_fail(&b->d, 0, "#%s in window #%s has no items to choose from", name, window);
        return 0;
    }
    return nm;
}

int uui_builder_choice(struct uui_builder *b, const char *window, const char *name, const char *id) {
    struct named *nm = chooser(b, window, name);
    if (!nm) return -1;
    for (int i = 0; i < nm->nids; i++)
        if (nm->ids[i] && !strcmp(nm->ids[i], id)) return i;
    udecl_fail(&b->d, 0, "the app needs an item with id=%s in #%s, window #%s", id, name, window);
    return -1;
}

const char *uui_builder_choice_id(struct uui_builder *b, const char *window, const char *name, int index) {
    struct named *nm = chooser(b, window, name);
    return nm && index >= 0 && index < nm->nids ? nm->ids[index] : 0;
}

int uui_builder_window(struct uui_builder *b, const char *window, struct uapp_desc *desc) {
    struct bwin *w = window_of(b, window);
    if (!w) return 0;
    if (w->opts) {
        udecl_fail(&b->d, w->line, "#%s is an options: uui_builder_options_open() opens it", window);
        return 0;
    }
    if (w->title) desc->title = w->title;
    if (w->app_id) desc->app_id = w->app_id;
    if (w->cells_w) { desc->cells_w = w->cells_w; desc->cells_h = w->cells_h; }
    if (w->min_w || w->min_h) { desc->min_w = w->min_w; desc->min_h = w->min_h; }
    if (w->resizable) desc->flags |= UAPP_RESIZABLE;
    for (int i = 0; i < w->nnamed; i++)
        if (!strcmp(w->named[i].type->word, "app") && (!w->named[i].item->ops || !w->named[i].item->widget)) {
            udecl_fail(&b->d, w->named[i].line, "the program gave app #%s no widget", w->named[i].name);
            return 0;
        }
    desc->layout = w->root;
    if (w->ntop > 1) {
        desc->widgets = w->top;
        desc->widget_count = w->ntop;
    } else {
        desc->widgets = w->root->items;
        desc->widget_count = w->root->count;
    }
    if (w->nfocus) {
        int room = 0;
        for (int i = 0; i < w->nfocus; i++) {
            const struct btype *t = w->named[w->focus[i]].type;
            room += t->focusables ? t->nfocus : 1;
        }
        struct uui_focusable *f = alloc(b, sizeof *f * (size_t)room, w->line);
        const struct uui_item **pg = alloc(b, sizeof *pg * (size_t)room, w->line);
        if (!f || !pg) return 0;
        int k = 0;
        for (int i = 0; i < w->nfocus; i++) {
            const struct named *nm = &w->named[w->focus[i]];
            if (nm->item->hidden) continue;
            if (!strcmp(nm->type->word, "app") && !nm->item->ops->key) {
                udecl_fail(&b->d, w->line, "#%s takes no keys, so it cannot be focused", nm->name);
                return 0;
            }
            int at = k;
            if (nm->type->focusables) k += nm->type->focusables(nm->obj, f + k, room - k);
            else f[k++] = (struct uui_focusable){ nm->item->widget,
                                                  nm->type->focus_ops ? nm->type->focus_ops : nm->item->ops };
            while (at < k) pg[at++] = nm->page;
        }
        uui_focus_init(&w->ring, f, k);
        w->ring.page = pg;
        if (k && w->autofocus) uui_focus_set(&w->ring, 0);
        desc->focus = &w->ring;
    }
    return 1;
}

int uui_builder_run_error(struct uui_builder *b) {
    static struct uui_label head, what;
    static struct uui_item items[2];
    static struct uui_layout root;
    const char *e = uui_builder_error(b);
    // A lookup's error comes after the load's line, which said "built".
    ulogf("uui_builder: %s\n", e ? e : "(no error)");
    uui_label_init(&head, "This program's window could not be built:");
    uui_label_init(&what, e ? e : "(no error)");
    what.wrap = 1;
    what.rows = 4;
    items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &head, .flags = UUI_FILL_W };
    items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &what, .flags = UUI_FILL_W | UUI_FILL_H,
                                  .name = "error" };
    root = (struct uui_layout){ .dir = UUI_COLUMN, .items = items, .count = 2 };
    struct uapp_desc desc = {
        .title = "Broken window file", .app_id = "uierror", .cells_w = 64, .cells_h = 8,
        .layout = &root, .widgets = items, .widget_count = 2,
    };
    uapp_run(&desc);
    return 1;
}
