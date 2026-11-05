// See ui/uui_builder.h; the language and every type in docs/ui-files-design.md.
//
// ONE TABLE OF TYPES, below: a type is its word, its ops table, the size
// of its struct and a function reading its own attributes. What every
// item shares (#name, fill, hidden, width/height) is read once, in
// build_item(). A widget joins the language by gaining a row.
#include "ui/uui_builder.h"
#include "ui/uui_layout.h"
#include "ui/uui_label.h"
#include "ui/uui_button.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_table.h"
#include "ui/uui_markdown.h"
#include "ui/uui_focus.h"
#include "ui/utheme.h"
#include "base/udecl.h"
#include "base/ugrow.h"
#include "base/ulog.h"
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct named {
    const char *name;
    struct uui_item *item;
};

struct bwin {
    const char *name;
    int line;
    struct uui_layout *root;
    const char *title, *app_id;
    int cells_w, cells_h, min_w, min_h, resizable;
    struct uui_item **focus;   // the focus= list, resolved at load
    int nfocus, focus_cap;
    struct uui_focus ring;
    struct named *named;
    int nnamed, named_cap;
};

struct uui_builder {
    struct udecl d;
    const struct uui_builder_name *names;
    int nnames;
    struct bwin *win;
    int nwin, win_cap;   // built; allocated (a failed window's arrays are freed too)
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
typedef int (*build_fn)(struct uui_builder *b, struct bwin *w, struct udecl_node *n,
                        struct uui_item *it, int dir);

static int build_item(struct uui_builder *b, struct bwin *w, struct udecl_node *n,
                      struct uui_item *it, int dir);

static int build_container(struct uui_builder *b, struct bwin *w, struct udecl_node *n,
                           struct uui_item *it, int dir, enum uui_dir kind) {
    (void)dir;
    struct uui_layout *l = it->widget;
    struct udecl *d = &b->d;
    l->dir = kind;
    // 0 IS uui_layout's "font-derived default", so an explicit 0 cannot be
    // said; refused rather than quietly turned into the default spacing.
    if (udecl_int(d, n, "margin", 0, 1000, &l->margin) < 0 || udecl_int(d, n, "gap", 0, 1000, &l->gap) < 0)
        return -1;
    if ((udecl_get(d, n, "margin") && !l->margin) || (udecl_get(d, n, "gap") && !l->gap))
        return udecl_fail(d, n->line, "0 means the default spacing here: leave it out, or give 1 for a hairline");
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

static int build_toolbar(struct uui_builder *b, struct bwin *w, struct udecl_node *n, struct uui_item *it, int dir) {
    (void)w; (void)dir;
    struct udecl *d = &b->d;
    if (!n->nkid) return udecl_fail(d, n->line, "toolbar holds nothing");
    struct uui_toolbar_item *items = alloc(b, sizeof *items * (size_t)n->nkid, n->line);
    if (!items) return -1;
    for (int i = 0; i < n->nkid; i++) {
        struct udecl_node *k = &n->kid[i];
        if (no_children(b, k) < 0) return -1;
        if (!strcmp(k->type, "separator")) {
            if (udecl_unused(d, k) < 0) return -1;
            continue;   // all zero: UUI_TOOLBAR_SEP
        }
        if (strcmp(k->type, "item")) return udecl_fail(d, k->line, "a toolbar holds item and separator, not %s", k->type);
        struct uui_toolbar_item *ti = &items[i];
        ti->label = udecl_text(k);
        ti->icon = udecl_get(&b->d, k, "icon");
        if (!ti->icon) return udecl_fail(d, k->line, "item needs icon=");
        ti->tip = udecl_get(&b->d, k, "tip");
        ti->accel = udecl_get(&b->d, k, "accel");
        const char *word = udecl_get(&b->d, k, "command");
        if (command(b, k, word ? word : k->name, &ti->code) < 0) return -1;
        int role = 0;
        if (udecl_word(d, k, "role", ROLES, UTHEME_ACT_COUNT, &role) < 0) return -1;
        ti->tint = (uint32_t)role;
        ti->flags = (udecl_flag(&b->d, k, "menu") ? UUI_TB_MENU : 0) | (udecl_flag(&b->d, k, "end") ? UUI_TB_END : 0);
        if (k->name) return udecl_fail(d, k->line, "a toolbar item is named by command=, not #");
        if (udecl_unused(d, k) < 0) return -1;
    }
    uui_toolbar_init(it->widget, items, n->nkid);
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

static const struct btype {
    const char *word;
    const struct uui_widget_ops *ops;
    size_t size;
    build_fn build;
} TYPES[] = {
    { "row",      &uui_layout_ops,   sizeof(struct uui_layout),   build_row },
    { "column",   &uui_layout_ops,   sizeof(struct uui_layout),   build_column },
    { "grid",     &uui_layout_ops,   sizeof(struct uui_layout),   build_grid },
    { "label",    &uui_label_ops,    sizeof(struct uui_label),    build_label },
    { "stretch",  &uui_label_ops,    sizeof(struct uui_label),    build_stretch },
    { "button",   &uui_button_ops,   sizeof(struct uui_button),   build_button },
    { "toolbar",  &uui_toolbar_ops,  sizeof(struct uui_toolbar),  build_toolbar },
    { "table",    &uui_table_ops,    sizeof(struct uui_table),    build_table },
    { "markdown", &uui_markdown_ops, sizeof(struct uui_markdown), build_markdown },
};

static const struct btype *type_of(const char *word) {
    for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
        if (!strcmp(TYPES[i].word, word)) return &TYPES[i];
    return 0;
}

static struct uui_item *find_named_n(struct bwin *w, const char *name, size_t len) {
    for (int i = 0; i < w->nnamed; i++)
        if (strlen(w->named[i].name) == len && !memcmp(w->named[i].name, name, len)) return w->named[i].item;
    return 0;
}
static struct uui_item *find_named(struct bwin *w, const char *name) {
    return find_named_n(w, name, strlen(name));
}

static int build_item(struct uui_builder *b, struct bwin *w, struct udecl_node *n,
                      struct uui_item *it, int dir) {
    struct udecl *d = &b->d;
    const struct btype *t = type_of(n->type);
    if (!t) return udecl_fail(d, n->line, "there is no widget type '%s'", n->type);
    it->ops = t->ops;
    it->widget = alloc(b, t->size, n->line);
    if (!it->widget) return -1;
    b->widgets++;
    if (n->name) {
        if (find_named(w, n->name)) return udecl_fail(d, n->line, "window %s has two #%s", w->name, n->name);
        if (!UGROW(w->named, w->named_cap, w->nnamed + 1)) return udecl_fail(d, n->line, "out of memory");
        w->named[w->nnamed++] = (struct named){ n->name, it };
        it->name = n->name;
        lookup(b, n->name, &it->id);
    }
    if (udecl_flag(&b->d, n, "fill")) it->flags |= UUI_FILL_W | UUI_FILL_H;
    if (udecl_flag(&b->d, n, "fill-w")) it->flags |= UUI_FILL_W;
    if (udecl_flag(&b->d, n, "fill-h")) it->flags |= UUI_FILL_H;
    if (udecl_flag(&b->d, n, "track-hover")) it->flags |= UUI_TRACK_HOVER;
    it->hidden = udecl_flag(&b->d, n, "hidden");
    // The pin is along the PARENT's axis, so each word is legal in one.
    int r;
    if ((r = udecl_int(d, n, "width", 1, 1000, &it->main_cells)) < 0) return -1;
    if (r && dir != UUI_ROW) return udecl_fail(d, n->line, "width= sizes a row's child; this is not in a row");
    if ((r = udecl_int(d, n, "height", 1, 1000, &it->main_cells)) < 0) return -1;
    if (r && dir != UUI_COLUMN) return udecl_fail(d, n->line, "height= sizes a column's child; this is not in a column");
    if (t->build(b, w, n, it, dir) < 0) return -1;
    return udecl_unused(d, n);
}

static int collect(struct uui_layout *l, struct uui_item ***all, int *n, int *cap) {
    for (int i = 0; i < l->count; i++) {
        struct uui_item *it = &l->items[i];
        if (!UGROW(*all, *cap, *n + 1)) return -1;
        (*all)[(*n)++] = it;
        if (it->ops == &uui_layout_ops && collect(it->widget, all, n, cap) < 0) return -1;
    }
    return 0;
}

// uapp REFUSES TO START a window with an id twice (ids_unique(), with a
// log line only), so the builder says it first, where it can be shown: two
// buttons with one command, or a #name that is also another's command.
static int ids_differ(struct uui_builder *b, struct bwin *w) {
    struct uui_item **all = 0;
    int n = 0, cap = 0, rc = 0;
    if (collect(w->root, &all, &n, &cap) < 0) rc = udecl_fail(&b->d, w->line, "out of memory");
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

static int build_window(struct uui_builder *b, struct udecl_node *n, struct bwin *w) {
    struct udecl *d = &b->d;
    if (strcmp(n->type, "window")) return udecl_fail(d, n->line, "the file holds windows, not %s", n->type);
    if (!n->name) return udecl_fail(d, n->line, "a window needs a #name");
    for (int i = 0; i < b->nwin; i++)
        if (!strcmp(b->win[i].name, n->name)) return udecl_fail(d, n->line, "two windows are called #%s", n->name);
    w->name = n->name;
    w->line = n->line;
    w->title = udecl_get(&b->d, n, "title");
    w->app_id = udecl_get(&b->d, n, "app-id");
    w->resizable = udecl_flag(&b->d, n, "resizable");
    if (udecl_pair(d, n, "size", 1, 1000, &w->cells_w, &w->cells_h) < 0) return -1;
    if (udecl_pair(d, n, "min", 0, 100000, &w->min_w, &w->min_h) < 0) return -1;
    if (n->nkid != 1 || !is_container(n->kid[0].type))
        return udecl_fail(d, n->line, "a window holds one row, column or grid");
    struct uui_item root = { 0 };
    if (build_item(b, w, &n->kid[0], &root, -1) < 0) return -1;
    w->root = root.widget;
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
            struct uui_item *it = find_named_n(w, p, (size_t)len);
            if (!it) return udecl_fail(d, n->line, "focus= names #%.*s, which this window lacks", len, p);
            if (!it->ops->key) return udecl_fail(d, n->line, "#%.*s takes no keys, so it cannot be focused", len, p);
            if (!UGROW(w->focus, w->focus_cap, w->nfocus + 1)) return udecl_fail(d, n->line, "out of memory");
            w->focus[w->nfocus++] = it;
            p = e;
        }
    }
    return udecl_unused(d, n);
}

static struct uui_builder *build(struct uui_builder *b, int parsed) {
    if (parsed < 0) return b;
    struct udecl_node *top = &b->d.top;
    if (!top->nkid) { udecl_fail(&b->d, 0, "the file declares no window"); return b; }
    b->win = alloc(b, sizeof *b->win * (size_t)top->nkid, 0);
    if (!b->win) return b;
    b->win_cap = top->nkid;
    for (int i = 0; i < top->nkid; i++) {
        if (build_window(b, &top->kid[i], &b->win[i]) < 0) break;
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
    else ulogf("uui_builder: %s: %d windows, %d widgets, %u us\n", b->d.what, b->nwin, b->widgets, b->us);
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

struct uui_item *uui_builder_item(struct uui_builder *b, const char *window, const char *name) {
    struct bwin *w = window_of(b, window);
    if (!w) return 0;
    struct uui_item *it = find_named(w, name);
    if (!it) udecl_fail(&b->d, w->line, "the app needs #%s in window #%s", name, window);
    return it;
}

void *uui_builder_widget(struct uui_builder *b, const char *window, const char *name,
                         const struct uui_widget_ops *ops) {
    struct uui_item *it = uui_builder_item(b, window, name);
    if (!it) return 0;
    if (ops && it->ops != ops) {
        udecl_fail(&b->d, 0, "#%s in window #%s is not the kind of widget the app expects",
                   name, window);
        return 0;
    }
    return it->widget;
}

int uui_builder_window(struct uui_builder *b, const char *window, struct uapp_desc *desc) {
    struct bwin *w = window_of(b, window);
    if (!w) return 0;
    if (w->title) desc->title = w->title;
    if (w->app_id) desc->app_id = w->app_id;
    if (w->cells_w) { desc->cells_w = w->cells_w; desc->cells_h = w->cells_h; }
    if (w->min_w || w->min_h) { desc->min_w = w->min_w; desc->min_h = w->min_h; }
    if (w->resizable) desc->flags |= UAPP_RESIZABLE;
    desc->layout = w->root;
    desc->widgets = w->root->items;
    desc->widget_count = w->root->count;
    if (w->nfocus) {
        struct uui_focusable *f = alloc(b, sizeof *f * (size_t)w->nfocus, w->line);
        if (!f) return 0;
        int k = 0;
        for (int i = 0; i < w->nfocus; i++)
            if (!w->focus[i]->hidden) f[k++] = (struct uui_focusable){ w->focus[i]->widget, w->focus[i]->ops };
        uui_focus_init(&w->ring, f, k);
        if (k) uui_focus_set(&w->ring, 0);
        desc->focus = &w->ring;
    }
    return 1;
}

int uui_builder_run_error(struct uui_builder *b) {
    static struct uui_label head, what;
    static struct uui_item items[2];
    static struct uui_layout root;
    const char *e = uui_builder_error(b);
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
