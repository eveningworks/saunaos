// Help -- the system manual, in a window.
//
// **IT PARSES NOTHING AND RENDERS NOTHING.** `lib/umd.h` owns what
// Markdown means and `ui/uui_markdown.h` owns what it looks like; this
// app owns only WHICH page you are reading. That is the same split
// `/bin/doc` sits on the other side of -- it renders these exact files
// to a terminal through umd's other half -- so the two can never
// disagree about what `**` means in a page nobody would check twice.
//
// The pages are `/usr/share/doc/<dir>/<name>.md`: `cmd` is staged from
// the repository's `docs/commands/`, `guide` from `data/`. The CONTENTS
// are grouped by each page's `**Category:**` line rather than by
// directory -- one directory holds every command, and a tree with one
// 124-row branch is a list. KHelpCenter's shape: contents on the left,
// Back/Forward/Home and a search box above, the page on the right.
//
// Every page is read once, at start, and kept: search looks through the
// whole text, and the manual is a few hundred kilobytes.
//
// THE PAGE IS A WEB VIEW when the engine is installed (ui/uui_webview.h,
// docs/browser-design.md stage 5): this app answers app:help/... with
// the page as HTML (lib/umd.h's writer) in the document look, and a
// link clicked in it is followed by the page itself -- the app hears
// the new address and keeps its contents and history in step. A build
// without the engine (NOGPL=1) draws the page with uui_markdown as
// before.
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_markdown.h"
#include "ui/uui_webview.h"
#include "ui/uui_tree.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_textbox.h"
#include "ui/uui_label.h"
#include "ui/uui_focus.h"
#include "ui/uui_route.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "lib/ufile.h"
#include "lib/umd.h"
#include "keyboard.h"
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define DOC_ROOT   "/usr/share/doc"
#define HOME_DIR   "guide"
#define HOME_NAME  "overview"
#define HOME_CAT   "Getting started"   // listed first; the rest by name

#define NAME_CAP   48    // = UUI_MD_LINK_MAX: a link names a page
#define TITLE_CAP  64
#define CAT_CAP    48
#define PATH_CAP   192
#define HIST_MAX   64

enum { ID_TOOLBAR = 1, ID_SEARCH, ID_TREE, ID_DOC };
enum { POST_WEB = 1 };
enum { CMD_BACK = 1, CMD_FORWARD, CMD_HOME };

// A place in the manual: a page index (>= 0) or a category's own page.
#define CAT_TARGET(c)  (-100 - (c))
#define IS_CAT(t)      ((t) <= CAT_TARGET(0))
#define CAT_OF(t)      (CAT_TARGET(0) - (t))
#define NO_TARGET      (-1)
#define NODE_NONE      (-2)     // the "nothing matches" row

struct page {
    char dir[64];     // = sys_dirent's name: a folder under DOC_ROOT
    char name[NAME_CAP];
    char title[TITLE_CAP];
    int  cat;
    char *text;       // the whole file, kept for search and display
    int  len;
};

struct category {
    char name[CAT_CAP];
    int  open;        // the tree's expansion, owned here (a lazy tree)
    int  shown;       // pages under it that pass the search
    char head[CAT_CAP + 12]; // "name (shown)", the tree's label for it
};

// All sized to the manual: grown while load() reads it, then fixed --
// nothing is loaded after, so the tree's pointers into them hold.
static struct page     *g_page;
static int             g_npages, g_page_cap;
static struct category *g_cat;
static int             g_ncats, g_cat_cap;
static int             *g_order;   // page indices, category then name
static unsigned char   *g_pass;    // per page: matches the search

static struct uui_tree      g_tree;
static struct uui_tree_node *g_nodes; // a heading per category, a row per page, "no match"
static int                  g_node_count;

static struct uui_markdown g_md;
static struct uui_webview g_web;
static int g_web_on;          // the engine is installed: the page is g_web
static struct uapp *g_app;
static struct uui_textbox  g_search;
static struct uui_label    g_crumb;
static char                g_crumb_text[CAT_CAP + TITLE_CAP + 16];
static char                g_query[64];

// A category's page is GENERATED: its pages, each a link with the first
// sentence of its description. Rebuilt on every visit, so it is one
// buffer.
static char *g_gen;

static const char MISSING[] =
    "# No manual\n\n"
    "Nothing could be read from `" DOC_ROOT "`.\n";

// History is a list of places with the scroll each was left at, and a
// cursor into it -- a browser's, so Back returns to where you were
// reading rather than to the top.
static struct { int target, scroll; } g_hist[HIST_MAX];
static int g_hist_n, g_hist_pos = -1;
static int g_here = NO_TARGET;

static const struct uui_toolbar_item TOOLBAR[] = {
    { "tb-back",    "Back (Alt+Left)",     CMD_BACK, 0, 0, 0, 0 },
    { "tb-forward", "Forward (Alt+Right)", CMD_FORWARD, 0, 0, 0, 0 },
    { "tb-home",    "Home (Alt+Home)",     CMD_HOME, 0, 0, 0, 0 },
};
static struct uui_toolbar g_toolbar;

static struct uui_item   LEFT[2], RIGHT[2], BODY[2], ITEMS[2];
static struct uui_layout LEFT_L, RIGHT_L, BODY_L, ROOT_L;

// Search first in Tab order, where a reader looks for it; the tree has
// the keyboard on open, as the page list always has.
static struct uui_focusable g_focusables[] = {
    { .widget = &g_search, .ops = &uui_textbox_ops },
    { .widget = &g_tree,   .ops = &uui_tree_ops },
};
static struct uui_focus g_focus;

// --- loading ----------------------------------------------------------

static int by_name(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

// Doubles `*arr` (of `size`-byte elements) when `n` has reached `*cap`.
static int grow(void *arr, int n, int *cap, size_t size) {
    if (n < *cap) return 1;
    int c = *cap ? *cap * 2 : 16;
    void *g = realloc(*(void **)arr, (size_t)c * size);
    if (!g) return 0;
    *(void **)arr = g;
    *cap = c;
    return 1;
}

// -1 when out of memory.
static int cat_index(const char *name) {
    for (int i = 0; i < g_ncats; i++)
        if (!strcmp(g_cat[i].name, name)) return i;
    if (!grow(&g_cat, g_ncats, &g_cat_cap, sizeof *g_cat)) return -1;
    memset(&g_cat[g_ncats], 0, sizeof *g_cat);
    strlcpy(g_cat[g_ncats].name, name, CAT_CAP);
    return g_ncats++;
}

static void load_dir(const char *dir) {
    char (*names)[NAME_CAP] = NULL;
    int names_cap = 0;
    char path[PATH_CAP];
    snprintf(path, sizeof path, "%s/%s", DOC_ROOT, dir);
    DIR *d = opendir(path);
    if (!d) return;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int len = (int)strlen(e->d_name);
        if (e->d_type == DT_DIR || len < 4 || len - 3 >= NAME_CAP ||
            strcmp(e->d_name + len - 3, ".md") != 0)
            continue;
        if (!grow(&names, n, &names_cap, sizeof *names)) {
            ulogf("help: out of memory listing %s at %d pages\n", path, n);
            break;
        }
        memcpy(names[n], e->d_name, (size_t)len - 3);
        names[n][len - 3] = 0;
        n++;
    }
    closedir(d);
    if (n) qsort(names, (size_t)n, NAME_CAP, by_name);

    for (int i = 0; i < n; i++) {
        if (!grow(&g_page, g_npages, &g_page_cap, sizeof *g_page)) {
            ulogf("help: out of memory at %d pages; the rest are not listed\n", g_npages);
            break;
        }
        struct page *p = &g_page[g_npages];
        snprintf(path, sizeof path, "%s/%s/%s.md", DOC_ROOT, dir, names[i]);
        uint8_t *buf = NULL;
        size_t len = 0;
        if (ufile_slurp(path, 1u << 20, &buf, &len) != UFILE_OK) continue;
        strlcpy(p->dir, dir, sizeof p->dir);
        strlcpy(p->name, names[i], NAME_CAP);
        p->text = (char *)buf;
        p->len = (int)len;
        if (!umd_title(p->text, p->len, p->title, TITLE_CAP))
            strlcpy(p->title, p->name, TITLE_CAP);
        char cat[CAT_CAP];
        if (!umd_field(p->text, p->len, "Category", cat, sizeof cat))
            strlcpy(cat, "Other", sizeof cat);
        p->cat = cat_index(cat);
        if (p->cat < 0) { free(p->text); break; }
        g_npages++;
    }
    free(names);
}

static int cat_rank(int c) { return strcmp(g_cat[c].name, HOME_CAT) ? 1 : 0; }

static int is_home(int i) {
    return !strcmp(g_page[i].dir, HOME_DIR) && !strcmp(g_page[i].name, HOME_NAME);
}

static int page_cmp(const void *a, const void *b) {
    const struct page *p = &g_page[*(const int *)a], *q = &g_page[*(const int *)b];
    if (p->cat != q->cat) {
        int r = cat_rank(p->cat) - cat_rank(q->cat);
        return r ? r : strcmp(g_cat[p->cat].name, g_cat[q->cat].name);
    }
    int hp = is_home(*(const int *)a), hq = is_home(*(const int *)b);
    if (hp != hq) return hq - hp;
    return strcmp(p->name, q->name);
}

static void load(void) {
    DIR *d = opendir(DOC_ROOT);
    if (d) {
        char (*dirs)[sizeof g_page->dir] = NULL;
        int nd = 0, dirs_cap = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (e->d_type == DT_DIR && e->d_name[0] != '.' &&
                grow(&dirs, nd, &dirs_cap, sizeof *dirs))
                strlcpy(dirs[nd++], e->d_name, sizeof dirs[0]);
        closedir(d);
        if (nd) qsort(dirs, (size_t)nd, sizeof dirs[0], by_name);
        for (int i = 0; i < nd; i++) load_dir(dirs[i]);
        free(dirs);
    }
    // Sized once, now the manual's size is known; +1 so none is ever 0.
    g_order = malloc((size_t)(g_npages + 1) * sizeof *g_order);
    g_pass = malloc((size_t)g_npages + 1);
    g_nodes = malloc((size_t)(g_npages + g_ncats + 1) * sizeof *g_nodes);
    if (!g_order || !g_pass || !g_nodes) {
        ulogf("help: out of memory indexing %d pages\n", g_npages);
        g_npages = 0;   // an empty manual, which every path below handles
        if (!g_nodes) g_nodes = malloc(sizeof *g_nodes);   // "No page matches"
        if (!g_nodes) exit(1);
    }
    for (int i = 0; i < g_npages; i++) g_order[i] = i;
    if (g_npages) qsort(g_order, (size_t)g_npages, sizeof g_order[0], page_cmp);
    for (int c = 0; c < g_ncats; c++) g_cat[c].open = !cat_rank(c);
}

static int find_page(const char *name) {
    for (int i = 0; i < g_npages; i++)
        if (!strcmp(g_page[i].name, name)) return i;
    return -1;
}

// A place's name, for the log.
static const char *target_name(int t) {
    if (t >= 0 && t < g_npages) return g_page[t].name;
    if (IS_CAT(t) && CAT_OF(t) < g_ncats) return g_cat[CAT_OF(t)].name;
    return "-";
}

static int home_target(void) {
    for (int i = 0; i < g_npages; i++) if (is_home(i)) return i;
    return g_npages ? g_order[0] : NO_TARGET;
}

// --- search -----------------------------------------------------------

static int has(const char *hay, int n, const char *w, int wn) {
    for (int i = 0; i + wn <= n; i++)
        if (!strncasecmp(hay + i, w, (size_t)wn)) return 1;
    return 0;
}

// Every word of the query, somewhere in the name, the title or the text:
// "echo request" finds ping without the words being adjacent.
static int matches(int i) {
    const struct page *p = &g_page[i];
    const char *q = g_query;
    while (*q) {
        while (*q == ' ') q++;
        int wn = 0;
        while (q[wn] && q[wn] != ' ') wn++;
        if (!wn) break;
        if (!has(p->name, (int)strlen(p->name), q, wn) &&
            !has(p->title, (int)strlen(p->title), q, wn) &&
            !has(p->text, p->len, q, wn))
            return 0;
        q += wn;
    }
    return 1;
}

// --- the contents -----------------------------------------------------

static void add_node(const char *label, int depth, int id, int kind) {
    struct uui_tree_node *n = &g_nodes[g_node_count++];
    *n = (struct uui_tree_node){ label, depth, id, kind, NULL, NULL };
}

// The selection follows the PAGE, not the row it used to be on. A page
// whose category was just collapsed selects that heading, as every tree
// moves the selection to the parent -- left at -1, the arrows had no
// row to move from.
static void select_here(void) {
    if (uui_tree_select_id(&g_tree, g_here) || uui_tree_selected_id(&g_tree) == g_here)
        return;
    int up = g_here >= 0 && !g_query[0] ? CAT_TARGET(g_page[g_here].cat) : NO_TARGET;
    if (up != NO_TARGET &&
        (uui_tree_select_id(&g_tree, up) || uui_tree_selected_id(&g_tree) == up))
        return;
    g_tree.selected = -1;
}

// The tree is REBUILT whole on every expand, collapse and keystroke of
// search: it is lazy (UUI_TREE_CLOSED/OPEN), because the widget's own
// collapse state covers UUI_TREE_MAX_NODES and the manual is longer.
static void rebuild_tree(void) {
    int searching = g_query[0] != 0;
    for (int c = 0; c < g_ncats; c++) g_cat[c].shown = 0;
    for (int i = 0; i < g_npages; i++) {
        g_pass[i] = !searching || matches(i);
        if (g_pass[i]) g_cat[g_page[i].cat].shown++;
    }

    g_node_count = 0;
    for (int k = 0; k < g_npages; ) {
        int c = g_page[g_order[k]].cat;
        int end = k;
        while (end < g_npages && g_page[g_order[end]].cat == c) end++;
        if (g_cat[c].shown) {
            // Searching opens every category with a match: the results
            // ARE the tree, and a closed branch would hide them.
            int open = searching || g_cat[c].open;
            snprintf(g_cat[c].head, sizeof g_cat[c].head, "%s (%d)", g_cat[c].name, g_cat[c].shown);
            add_node(g_cat[c].head, 0, CAT_TARGET(c), open ? UUI_TREE_OPEN : UUI_TREE_CLOSED);
            if (open)
                for (int j = k; j < end; j++)
                    if (g_pass[g_order[j]])
                        add_node(g_page[g_order[j]].title, 1, g_order[j], UUI_TREE_AUTO);
        }
        k = end;
    }
    if (!g_node_count) add_node("No page matches", 0, NODE_NONE, UUI_TREE_AUTO);

    uui_tree_set_nodes_keep(&g_tree, g_nodes, g_node_count);
    select_here();
}

// Set by an expand or collapse, and cleared when the gesture ends: the
// tree reports the press, release or key that toggled as a change, and
// with the selection moved to a heading by select_here() that would open
// the category's page. Opening a branch is not choosing it.
static int g_toggled;

static void on_toggle(void *ctx, int id, int expand) {
    (void)ctx;
    g_toggled = 1;
    if (!IS_CAT(id) || g_query[0]) return;   // a search holds every match open
    g_cat[CAT_OF(id)].open = expand;
    rebuild_tree();
}

// --- reading ----------------------------------------------------------

// The first sentence of a page's description, for the category page.
static void summary(const struct page *p, char *out, int cap) {
    char para[400];
    out[0] = 0;
    if (!umd_section_para(p->text, p->len, "Description", para, sizeof para)) return;
    const char *s = para;
    // "`/bin/ping` -- send ..." opens most pages; the list already names it.
    if (*s == '/') {
        const char *dash = strstr(s, " -- ");
        const char *em = strstr(s, " \xe2\x80\x94 ");
        if (em && (!dash || em < dash)) s = em + 5;
        else if (dash && dash - s < 40) s = dash + 4;
    }
    int n = 0;
    while (s[n] && n < cap - 1 && !(s[n] == '.' && (s[n + 1] == ' ' || !s[n + 1]))) n++;
    memcpy(out, s, (size_t)n);
    out[n] = 0;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
}

// A category's page, as Markdown, into g_gen. Its length.
static int category_md(int c) {
    size_t cap = 256;
    for (int i = 0; i < g_npages; i++) if (g_page[i].cat == c) cap += 200 + NAME_CAP;
    char *buf = malloc(cap);
    if (!buf) return 0;
    int n = snprintf(buf, cap, "# %s\n\n", g_cat[c].name);
    int count = 0;
    for (int k = 0; k < g_npages; k++) if (g_page[g_order[k]].cat == c) count++;
    n += snprintf(buf + n, cap - (size_t)n, "%d page%s.\n\n", count, count == 1 ? "" : "s");
    for (int k = 0; k < g_npages && (size_t)n < cap; k++) {
        const struct page *p = &g_page[g_order[k]];
        if (p->cat != c) continue;
        char s[160];
        summary(p, s, sizeof s);
        n += snprintf(buf + n, cap - (size_t)n, "- `%s`%s%s\n", p->name,
                      s[0] ? " -- " : "", s);
    }
    if (!g_web_on) uui_markdown_set_text(&g_md, buf, n < (int)cap ? n : (int)cap - 1);
    free(g_gen);           // only once the widget points at the new one
    g_gen = buf;
    return n < (int)cap ? n : (int)cap - 1;
}

static void show_category(int c) {
    category_md(c);
    snprintf(g_crumb_text, sizeof g_crumb_text, "Help  >  %s", g_cat[c].name);
}

// --- the page as a web view -------------------------------------------

// A place's address: app:help/<dir>/<name>, or app:help/category/<n>.
static void url_of(int target, char *out, size_t cap) {
    if (target >= 0 && target < g_npages)
        snprintf(out, cap, "app:help/%s/%s", g_page[target].dir, g_page[target].name);
    else if (IS_CAT(target))
        snprintf(out, cap, "app:help/category/%d", CAT_OF(target));
    else
        snprintf(out, cap, "app:help/missing");
}

static int target_of(const char *url) {
    const char *p = !strncmp(url, "app:help/", 9) ? url + 9 : NULL;
    if (!p) return NO_TARGET;
    if (!strncmp(p, "category/", 9)) {
        int c = atoi(p + 9);
        return c >= 0 && c < g_ncats ? CAT_TARGET(c) : NO_TARGET;
    }
    const char *slash = strchr(p, '/');
    if (!slash) return NO_TARGET;
    for (int i = 0; i < g_npages; i++)
        if (!strncmp(g_page[i].dir, p, (size_t)(slash - p)) && !g_page[i].dir[slash - p] &&
            !strcmp(g_page[i].name, slash + 1))
            return i;
    return NO_TARGET;
}

// A code span naming another page is a link, written with its title.
static int html_link(void *ctx, const char *word, char *href, int hcap, char *text, int tcap) {
    (void)ctx;
    int i = find_page(word);
    if (i < 0 || i == g_here) return 0;
    url_of(i, href, (size_t)hcap);
    snprintf(text, (size_t)tcap, "%s", g_page[i].title);
    return 1;
}

static void wv_scheme(struct uui_webview *v, uint32_t id, const char *url) {
    int t = target_of(url);
    const char *md = MISSING;
    int len = (int)sizeof MISSING - 1;
    const char *title = "Help";
    if (t >= 0) {
        md = g_page[t].text, len = g_page[t].len, title = g_page[t].title;
    } else if (IS_CAT(t)) {
        len = category_md(CAT_OF(t));
        md = g_gen;
        title = g_cat[CAT_OF(t)].name;
    }
    // The page's own name is not a link to itself: g_here is where the
    // page is GOING, which is the place being asked for.
    int was = g_here;
    g_here = t;
    struct umd_html_opts o = { html_link, NULL, UMD_HTML_DETAILS_OPEN, NULL };
    int n = 0;
    char *page = uui_webview_markdown_page(title, md, len, &o, &n);
    g_here = was;
    if (page) uui_webview_scheme_reply(v, id, 200, "text/html", page, (uint32_t)n, 1);
    else uui_webview_scheme_reply(v, id, 404, NULL, NULL, 0, 1);
    free(page);
}

static void arrive(int target);

// The page went somewhere by itself -- a link clicked in it.
static void wv_changed(struct uui_webview *v, unsigned what) {
    if (what & UUI_WV_URL) {
        int t = target_of(v->url);
        if (t != NO_TARGET && t != g_here) arrive(t);
    }
    if (g_app) uapp_redraw(g_app);
}

// An http(s) link in the manual opens in the Web Browser.
static void wv_external(struct uui_webview *v, const char *url) {
    (void)v;
    if (g_app) uapp_spawn(g_app, "/bin/wm/apps/browser", url);
}

static const struct uui_webview_host HOST = {
    .changed = wv_changed,
    .external = wv_external,
    .scheme = wv_scheme,
};

static void show_ex(int target, int scroll, int navigate) {
    if (g_web_on && navigate) {
        char url[PATH_CAP];
        url_of(target, url, sizeof url);
        uui_webview_navigate(&g_web, url);
    }
    if (target >= 0 && target < g_npages) {
        const struct page *p = &g_page[target];
        if (!g_web_on) uui_markdown_set_text(&g_md, p->text, p->len);
        snprintf(g_crumb_text, sizeof g_crumb_text, "Help  >  %s  >  %s",
                 g_cat[p->cat].name, p->title);
    } else if (IS_CAT(target) && CAT_OF(target) < g_ncats) {
        show_category(CAT_OF(target));
    } else {
        if (!g_web_on) uui_markdown_set_text(&g_md, MISSING, (int)sizeof MISSING - 1);
        strlcpy(g_crumb_text, "Help", sizeof g_crumb_text);
    }
    g_md.scroll = scroll;   // clamped by the next draw
    g_here = target;
    uui_label_set_text(&g_crumb, g_crumb_text);

    // Reveal it in the contents: a page under a closed category opens it.
    if (target >= 0 && !g_query[0] && !g_cat[g_page[target].cat].open) {
        g_cat[g_page[target].cat].open = 1;
        rebuild_tree();
    } else {
        select_here();
    }
    ulogf("help: showing %s\n", target_name(target));
}

static void show(int target, int scroll) { show_ex(target, scroll, 1); }

// Go somewhere new: forward history past here is dropped, as in a browser.
static void go(int target) {
    if (target == NO_TARGET || target == NODE_NONE || target == g_here) return;
    if (g_hist_pos >= 0) g_hist[g_hist_pos].scroll = g_md.scroll;
    if (g_hist_pos == HIST_MAX - 1) {
        memmove(g_hist, g_hist + 1, sizeof g_hist[0] * (HIST_MAX - 1));
        g_hist_pos--;
    }
    g_hist_pos++;
    g_hist_n = g_hist_pos + 1;
    g_hist[g_hist_pos].target = target;
    g_hist[g_hist_pos].scroll = 0;
    show(target, 0);
}

// Somewhere the PAGE went: history as go() keeps it, without loading it
// again.
static void arrive(int target) {
    if (g_hist_pos == HIST_MAX - 1) {
        memmove(g_hist, g_hist + 1, sizeof g_hist[0] * (HIST_MAX - 1));
        g_hist_pos--;
    }
    g_hist_pos++;
    g_hist_n = g_hist_pos + 1;
    g_hist[g_hist_pos].target = target;
    g_hist[g_hist_pos].scroll = 0;
    show_ex(target, 0, 0);
}

static void step(int by) {
    int to = g_hist_pos + by;
    if (to < 0 || to >= g_hist_n) return;
    g_hist[g_hist_pos].scroll = g_md.scroll;
    g_hist_pos = to;
    show(g_hist[to].target, g_hist[to].scroll);
}

static unsigned item_flags(int code) {
    if (code == CMD_BACK && g_hist_pos <= 0) return UUI_MI_DISABLED;
    if (code == CMD_FORWARD && g_hist_pos >= g_hist_n - 1) return UUI_MI_DISABLED;
    return 0;
}

static void command(int code) {
    if (code == CMD_BACK) step(-1);
    else if (code == CMD_FORWARD) step(1);
    else if (code == CMD_HOME) go(home_target());
}

// A code span naming another page is a link; this page's own name is not.
static int is_link(void *ctx, const char *word) {
    (void)ctx;
    int i = find_page(word);
    return i >= 0 && i != g_here;
}

// --- the app ----------------------------------------------------------

static void search_changed(void) {
    const char *q = uui_textbox_text(&g_search);
    if (!strcmp(q, g_query)) return;
    strlcpy(g_query, q, sizeof g_query);
    rebuild_tree();
    int n = 0;
    for (int c = 0; c < g_ncats; c++) n += g_cat[c].shown;
    ulogf("help: search \"%s\" matches %d\n", g_query, g_query[0] ? n : 0);
}

// Enter in the search box opens the first match, as every help viewer's
// search field does.
static void open_first_match(void) {
    for (int i = 0; i < g_node_count; i++)
        if (g_nodes[i].id >= 0) { go(g_nodes[i].id); return; }
}

static void on_widget(struct uapp *a, int id, int reason) {
    char link[NAME_CAP];
    switch (id) {
    case ID_TOOLBAR: {
        int code = uui_toolbar_take_code(&g_toolbar);
        if (code > 0) command(code);
        break;
    }
    case ID_SEARCH:
        search_changed();
        break;
    case ID_TREE:
        // A hover or a wheel reaches here too, and the selection can be a
        // heading standing in for the page (select_here()): only a press
        // or a key is a choice.
        if (g_toggled) {
            if (reason == UUI_REASON_RELEASE || reason == UUI_REASON_KEY) g_toggled = 0;
        } else if (reason == UUI_REASON_PRESS || reason == UUI_REASON_RELEASE ||
                   reason == UUI_REASON_KEY) {
            go(uui_tree_selected_id(&g_tree));
        }
        break;
    case ID_DOC:
        if (uui_markdown_take_link(&g_md, link, sizeof link)) go(find_page(link));
        break;
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    if (mods & KEY_MOD_ALT) {
        if (key == KEY_ARROW_LEFT) command(CMD_BACK);
        else if (key == KEY_ARROW_RIGHT) command(CMD_FORWARD);
        else if (key == KEY_HOME) command(CMD_HOME);
        else return;
    } else if (key == 0x06) {                        // Ctrl-F: to the search box
        uui_focus_set(&g_focus, 0);
    } else if (g_focus.current == 0 && (key == '\n' || key == '\r')) {
        open_first_match();
    } else if (g_focus.current == 0 && key == 0x1B && g_query[0]) {
        uui_textbox_set_text(&g_search, "");
        search_changed();
    } else if (g_focus.current == 0) {
        search_changed();                            // an edit the ring delivered
    } else {
        return;
    }
    uapp_redraw(a);
}

// The trail is secondary text, derived per frame so a theme change
// reaches it -- the setting row's description does the same.
static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a; (void)d;
    g_crumb.dim = 1;
}

// The report tests drive the app by. From on_draw_over, because the
// links are recorded as the page draws.
static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    uapp_log_layout(a, "help");
    int sel = uui_tree_selected_id(&g_tree);
    uapp_logf_layout("help: here \"%s\" sel \"%s\" rows %d row_h %d top %d back %d fwd %d\n",
                     target_name(g_here), target_name(sel),
                     uui_tree_visible_count(&g_tree), uui_tree_row_h(&g_tree),
                     g_tree.top, g_hist_pos > 0, g_hist_pos < g_hist_n - 1);
    for (int i = 0; i < g_md.link_n && i < 8; i++)
        if (g_md.link[i].w > 0)
            uapp_logf_layout("help: link %s %d %d %d %d\n", g_md.link[i].target,
                             g_md.link[i].x, g_md.link[i].y, g_md.link[i].w, g_md.link[i].h);
    for (int i = 0; i < (int)(sizeof TOOLBAR / sizeof TOOLBAR[0]); i++) {
        int x, y, w, h;
        if (uui_toolbar_item_rect(&g_toolbar, i, &x, &y, &w, &h))
            uapp_logf_layout("help: layout tool%d %d %d %d %d\n", i, x, y, w, h);
    }
}

static int on_tick(struct uapp *a) {
    (void)a;
    return uui_toolbar_tick(&g_toolbar);
}

// The contents column fits its widest heading, within bounds. Measured
// once the font is up (in main() every width is 0), and not on each
// rebuild, so a search cannot make the column twitch.
static void size_columns(void) {
    int em = ugfx_char_advance('n'), w = 0;
    uui_tree_natural_size(&g_tree, &w, NULL);
    if (w < em * 24) w = em * 24;
    if (w > em * 40) w = em * 40;
    BODY[0].main_size = w;
}

// **THE LAYOUT OUTRANKS `.w/.h`, AND THE TREE'S NATURAL HEIGHT IS EVERY
// ROW IT HAS**, so the window's size is stated here, after the font is up.
static void on_size(int *w, int *h) {
    size_columns();
    *w = ugfx_char_advance('n') * 110;
    *h = ugfx_char_h() * 36;
}

static void on_font(struct uapp *a) {
    size_columns();
    uui_layout_run(&ROOT_L, 0, 0, uapp_width(a), uapp_height(a));
}

static void on_open(struct uapp *a) {
    g_app = a;
    if (g_web_on && uui_webview_setup(a, POST_WEB) != 0) ulogf("help: no page channel\n");
    int home = home_target();
    if (home == NO_TARGET) show(NO_TARGET, 0);   // go() ignores "nowhere"
    else go(home);
}

static int on_user(struct uapp *a, int a0, int a1) {
    return uui_webview_on_post(a, a0, a1);
}

static int on_close(struct uapp *a) {
    (void)a;
    if (g_web_on) uui_webview_destroy(&g_web);
    uui_markdown_free(&g_md);
    for (int i = 0; i < g_npages; i++) free(g_page[i].text);
    free(g_gen);
    g_gen = NULL;
    return 1;
}

int main(void) {
    load();
    ulogf("help: %d pages in %d categories\n", g_npages, g_ncats);

    uui_tree_init(&g_tree, 0, 0, 0, 0, g_nodes, 0);
    uui_tree_set_on_toggle(&g_tree, on_toggle, NULL);
    g_tree.sel_style = UUI_SEL_STRONG;
    rebuild_tree();

    uui_toolbar_init(&g_toolbar, TOOLBAR, (int)(sizeof TOOLBAR / sizeof TOOLBAR[0]));
    g_toolbar.item_flags = item_flags;
    uui_textbox_init(&g_search, "");
    g_search.placeholder = "Search the manual";
    uui_label_init(&g_crumb, g_crumb_text);
    uui_markdown_init(&g_md);
    uui_markdown_set_links(&g_md, is_link, NULL);
    g_web_on = uui_webview_available();
    if (g_web_on) {
        uui_webview_init(&g_web, &HOST, NULL);
        g_web.scheme = 1;
        g_web.visible = 1;
    }

    uui_focus_init(&g_focus, g_focusables,
                   (int)(sizeof g_focusables / sizeof g_focusables[0]));
    uui_focus_set(&g_focus, 1);

    // Search above the contents it filters, as in System Settings.
    LEFT[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_search,
                                 .id = ID_SEARCH, .name = "search", .flags = UUI_FILL_W };
    LEFT[1] = (struct uui_item){ .ops = &uui_tree_ops, .widget = &g_tree,
                                 .id = ID_TREE, .name = "tree",
                                 .flags = UUI_FILL_W | UUI_FILL_H };
    LEFT_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = LEFT, .count = 2, .gap = 6 };

    RIGHT[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_crumb,
                                  .name = "crumb", .flags = UUI_FILL_W };
    RIGHT[1] = (struct uui_item){ .ops = &uui_markdown_ops, .widget = &g_md,
                                  .id = ID_DOC, .name = "doc",
                                  .flags = UUI_FILL_W | UUI_FILL_H };
    if (g_web_on) {
        RIGHT[1].ops = &uui_webview_ops;
        RIGHT[1].widget = &g_web;
    }
    RIGHT_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = RIGHT, .count = 2, .gap = 6 };

    BODY[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &LEFT_L,
                                 .flags = UUI_FILL_H };
    BODY[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &RIGHT_L,
                                 .flags = UUI_FILL_W | UUI_FILL_H };
    // The toolbar runs edge to edge, as in Files; the body keeps a small
    // margin of its own -- the root's default (a character cell on every
    // side) framed the whole window in panel grey.
    BODY_L = (struct uui_layout){ .dir = UUI_ROW, .items = BODY, .count = 2,
                                  .margin = 6, .gap = 6 };

    ITEMS[0] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &g_toolbar,
                                  .id = ID_TOOLBAR, .name = "toolbar", .flags = UUI_FILL_W };
    ITEMS[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BODY_L,
                                  .flags = UUI_FILL_W | UUI_FILL_H };
    ROOT_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS, .count = 2,
                                  .margin = 1, .gap = 1 };
    static struct uui_item root = { .ops = &uui_layout_ops, .widget = &ROOT_L };

    struct uapp_desc desc = {
        .title        = "Help",
        .app_id       = "help",
        .on_size      = on_size,
        .on_font      = on_font,
        .layout       = &ROOT_L,
        .widgets      = &root,
        .widget_count = 1,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_draw      = on_draw,
        .on_draw_over = on_draw_over,
        .on_tick      = on_tick,
        .tick_ms      = 250,
        .on_open      = on_open,
        .on_close     = on_close,
        .on_user      = on_user,
        .focus        = &g_focus,
        .flags        = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
