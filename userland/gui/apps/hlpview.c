// Help Viewer -- Windows Help (.HLP) files in a window.
//
// **IT PARSES NOTHING AND DRAWS NOTHING OF ITS OWN.** `lib/uhlp.h` owns
// what a .HLP file means and `ui/uui_helpview.h` owns what a topic looks
// like; this app owns only WHICH topic you are reading: the files it has
// open, the history, the Contents / Index / Find pane, and what a link or
// a macro does. Contents, Index and Find are WinHelp's three ways in;
// Back, History and the browse sequence its ways around.
//
// **IT PARSES UNTRUSTED INPUT, SO IT PLEDGES FIRST.** A help file is a
// compiled binary from anywhere; before the first byte is read the
// process gives up spawning, the network and everything but reading
// (`hlpview_sandbox()`), and refuses to run if it cannot.
//
// Run as `hlpview [-c MAP] [-k KEYWORD] [-m MACRO] FILE.HLP`: -c opens the
// topic a program's WinHelp(HELP_CONTEXT) names, -k a keyword, -m runs a
// macro first. w16 starts it that way (userland/w16/usermisc16.c).
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_helpview.h"
#include "ui/uui_tree.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_textbox.h"
#include "ui/uui_listbox.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_focus.h"
#include "ui/uui_route.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "lib/ufile.h"
#include "lib/uhlp.h"
#include "lib/uhlpmac.h"
#include "ui/uui_button.h"
#include "lib/uclip.h"
#include "keyboard.h"
#include "sandbox_abi.h"
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define FILE_MAX_BYTES (64u << 20)
#define PATH_CAP 512

enum { ID_TOOLBAR = 1, ID_TABS, ID_TREE, ID_FILTER, ID_KEYWORDS, ID_TOPICS, ID_FIND, ID_FOUND, ID_VIEW, ID_HISTMENU };
enum { CMD_BACK = 1, CMD_FORWARD, CMD_HOME, CMD_PREV, CMD_NEXT, CMD_PANE, CMD_COPY, CMD_ZOOM_IN, CMD_ZOOM_OUT,
       CMD_HISTORY, CMD_ZOOM_RESET, CMD_HIST_CLEAR, CMD_HIST_MORE,
       CMD_HIST_BASE = 1000 };   // CMD_HIST_BASE + i: the history entry i, from the History list
enum { TAB_CONTENTS, TAB_INDEX, TAB_FIND };

// --- the open files ----------------------------------------------------------------------------

struct hfile {
    char *path;
    uint8_t *data;
    size_t len;
    struct uhlp *h;
};

static struct hfile *g_f;
static int g_nf, g_fcap;
static int g_cf = -1, g_ct = -1;           // the topic shown: file, topic
static struct uhlp_doc *g_doc, *g_pdoc;    // the topic, and the pop-up over it
static struct uapp *g_app;
static char g_status[160];

static void run_file_macros(int idx);
static void buttons_rebuild(struct uapp *a);
static int g_manchor[4];                   // where a pop-up from a macro hangs: the hotspot clicked, else the view's corner

struct hist { int file, topic, scroll; };
static struct hist *g_hist;
static int g_hn, g_hpos = -1, g_hcap;

static const struct uhlp *cur_help(void) { return g_cf >= 0 ? g_f[g_cf].h : NULL; }

// A file in the same directory as `base`, found without regard to case: a
// help file names its neighbours in upper case, and the disk may not.
static char *find_sibling(const char *base, const char *name) {
    char dir[PATH_CAP];
    snprintf(dir, sizeof dir, "%s", base);
    char *slash = strrchr(dir, '/');
    if (!slash) strcpy(dir, ".");
    else if (slash == dir) dir[1] = 0;
    else *slash = 0;
    const char *want = strrchr(name, '\\');
    want = want ? want + 1 : name;
    char path[PATH_CAP];
    snprintf(path, sizeof path, "%s/%s", dir, want);
    if (access(path, 0) == 0) return strdup(path);
    DIR *d = opendir(dir);
    char *found = NULL;
    if (d) {
        struct dirent *e;
        while (!found && (e = readdir(d)) != NULL)
            if (!strcasecmp(e->d_name, want)) {
                snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
                found = strdup(path);
            }
        closedir(d);
    }
    return found;
}

static int file_index(const char *path) {
    for (int i = 0; i < g_nf; i++) if (!strcmp(g_f[i].path, path)) return i;
    return -1;
}

static int load_file(const char *path) {
    int i = file_index(path);
    if (i >= 0) return i;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (ufile_slurp(path, FILE_MAX_BYTES, &buf, &len) != UFILE_OK) return -1;
    const char *err = "";
    struct uhlp *h = uhlp_open(buf, len, &err);
    if (!h) {
        ulogf("hlpview: %s: %s\n", path, err);
        free(buf);
        snprintf(g_status, sizeof g_status, "%s is not a help file this viewer can read (%s)", path, err);
        return -1;
    }
    if (g_nf == g_fcap) {
        int nc = g_fcap ? g_fcap * 2 : 4;
        struct hfile *g = realloc(g_f, (size_t)nc * sizeof *g);
        if (!g) { uhlp_close(h); free(buf); return -1; }
        g_f = g;
        g_fcap = nc;
    }
    g_f[g_nf] = (struct hfile){ strdup(path), buf, len, h };
    ulogf("hlpview: opened %s: \"%s\", %d topics\n", path, uhlp_title(h), uhlp_ntopics(h));
    int idx = g_nf++;
    run_file_macros(idx);                     // what the file runs when it opens: its buttons
    return idx;
}

// --- the widgets ----------------------------------------------------------------------------------

static struct uui_helpview g_view;
static struct uui_toolbar g_tb;
static struct uui_segmented g_seg;
static struct uui_tree g_tree;
static struct uui_textbox g_filter, g_find;
static struct uui_listbox g_kwlist, g_topics, g_found;
static struct uui_statusbar g_bar;
static char g_stat_file[96], g_stat_topic[48];
static int g_tab = TAB_CONTENTS, g_pane = 1;

static const char *const TAB_NAMES[] = { "Contents", "Index", "Find" };

static const struct uui_toolbar_item TOOLBAR[] = {
    { "tb-pane",     "Show or hide the contents pane", CMD_PANE, 0, 0, 0, 0 },
    { NULL, NULL, 0, 0, 0, 0, 0 },
    { "tb-back",     "Back (Alt+Left)",                CMD_BACK, 0, 0, 0, 0 },
    { "tb-forward",  "Forward (Alt+Right)",            CMD_FORWARD, 0, 0, 0, 0 },
    { "tb-home",     "Contents topic (Ctrl+Home)",     CMD_HOME, 0, 0, 0, 0 },
    { "tb-up",       "Previous topic",                 CMD_PREV, 0, 0, 0, 0 },
    { "tb-down",     "Next topic",                     CMD_NEXT, 0, 0, 0, 0 },
    { "tb-history",  "History",                        CMD_HISTORY, 0, 0, 0, 0 },
    { NULL, NULL, 0, 0, 0, 0, 0 },
    { "tb-copy",     "Copy the selection, or the topic (Ctrl+C)", CMD_COPY, 0, 0, 0, 0 },
    { "tb-zoom-in",  "Larger text (Ctrl++)",           CMD_ZOOM_IN, 0, 0, 0, 0 },
    { "tb-zoom-out", "Smaller text (Ctrl+-)",          CMD_ZOOM_OUT, 0, 0, 0, 0 },
    { "tb-actual",   "Actual size (Ctrl+0)",           CMD_ZOOM_RESET, 0, 0, 0, 0 },
};

static struct uui_item   LEFT[8], RIGHT[1], BODY[2], ITEMS[3];
static struct uui_layout LEFT_L, RIGHT_L, BODY_L, ROOT_L;

static struct uui_focusable g_focusables[] = {
    { .widget = &g_filter, .ops = &uui_textbox_ops },
    { .widget = &g_find,   .ops = &uui_textbox_ops },
    { .widget = &g_tree,   .ops = &uui_tree_ops },
};
static struct uui_focus g_focus;

// --- contents: the topics' links as a tree ----------------------------------------------------------------
//
// A help file has no outline: its contents page is a topic of jumps, and
// those topics have jumps of their own. The tree is that web with each
// topic put under the FIRST topic that links to it, so every topic is on
// it once; what nothing links to is listed last.

struct tnode {
    int topic;
    int parent;
    int first_child, last_child, next;   // -1 none
    int open;
};
static struct tnode *g_tn;
static int g_ntn;
static int *g_tn_of;                       // topic -> node, -1
static struct uui_tree_node *g_nodes;
static int g_nodes_cap, g_nnodes;

static int tn_new(int topic, int parent) {
    struct tnode *g = realloc(g_tn, ((size_t)g_ntn + 1) * sizeof *g);
    if (!g) return -1;
    g_tn = g;
    g_tn[g_ntn] = (struct tnode){ topic, parent, -1, -1, -1, 0 };
    if (parent >= 0) {
        if (g_tn[parent].last_child >= 0) g_tn[g_tn[parent].last_child].next = g_ntn;
        else g_tn[parent].first_child = g_ntn;
        g_tn[parent].last_child = g_ntn;
    }
    if (topic >= 0) g_tn_of[topic] = g_ntn;
    return g_ntn++;
}

static void tn_build(const struct uhlp *h) {
    free(g_tn); g_tn = NULL; g_ntn = 0;
    free(g_tn_of); g_tn_of = NULL;
    int n = uhlp_ntopics(h);
    g_tn_of = malloc((size_t)(n ? n : 1) * sizeof *g_tn_of);
    if (!g_tn_of) return;
    for (int i = 0; i < n; i++) g_tn_of[i] = -1;
    int root = tn_new(uhlp_contents(h), -1);
    if (root < 0) return;
    g_tn[root].open = 1;
    // breadth first, so a topic lands under the shallowest parent that names it
    for (int q = 0; q < g_ntn; q++) {
        struct uhlp_doc *d = uhlp_render(h, g_tn[q].topic);
        if (!d) continue;
        for (int i = 0; i < d->nlink; i++) {
            const struct uhlp_link *lk = &d->link[i];
            if (lk->kind != UHLP_LINK_JUMP || lk->file >= 0) continue;
            int t = lk->by_offset ? uhlp_topic_by_offset(h, lk->hash, NULL) : uhlp_topic_by_hash(h, lk->hash);
            if (t < 0 || g_tn_of[t] >= 0) continue;
            tn_new(t, q);
        }
        uhlp_doc_free(d);
    }
    // what nothing links to, in file order, under their own heading
    int other = -1;
    for (int t = 0; t < n; t++) {
        if (g_tn_of[t] >= 0 || !uhlp_topic_title(h, t)[0]) continue;
        if (other < 0) { other = tn_new(-1, -1); if (other < 0) return; g_tn[other].open = 1; }
        tn_new(t, other);
    }
}

static int nodes_push(const char *label, int depth, int id, int kind) {
    if (g_nnodes == g_nodes_cap) {
        int nc = g_nodes_cap ? g_nodes_cap * 2 : 64;
        struct uui_tree_node *g = realloc(g_nodes, (size_t)nc * sizeof *g);
        if (!g) return 0;
        g_nodes = g;
        g_nodes_cap = nc;
    }
    g_nodes[g_nnodes++] = (struct uui_tree_node){ .label = label, .depth = depth, .id = id, .kind = kind };
    return 1;
}

static void nodes_walk(const struct uhlp *h, int n, int depth) {
    for (; n >= 0; n = g_tn[n].next) {
        const struct tnode *t = &g_tn[n];
        const char *label = t->topic < 0 ? "Other topics" : uhlp_topic_title(h, t->topic);
        if (!label[0]) label = "(untitled)";
        int parent = t->first_child >= 0;
        nodes_push(label, depth, n, parent ? (t->open ? UUI_TREE_OPEN : UUI_TREE_CLOSED) : UUI_TREE_AUTO);
        if (parent && t->open) nodes_walk(h, t->first_child, depth + 1);
    }
}

static void tree_rebuild(void) {
    const struct uhlp *h = cur_help();
    g_nnodes = 0;
    if (!h || !g_tn) { uui_tree_set_nodes_keep(&g_tree, g_nodes, 0); return; }
    // the roots are the contents topic and, if any, the "other" heading
    for (int n = 0; n >= 0 && n < g_ntn; n++) if (g_tn[n].parent < 0) nodes_walk(h, n, 0);
    // nodes_walk walks siblings; roots have none linked, so each call above covers one root
    uui_tree_set_nodes_keep(&g_tree, g_nodes, g_nnodes);
    if (g_ct >= 0 && g_tn_of && g_tn_of[g_ct] >= 0) uui_tree_select_id(&g_tree, g_tn_of[g_ct]);
}

static void on_toggle(void *ctx, int id, int expand) {
    (void)ctx;
    if (id >= 0 && id < g_ntn) g_tn[id].open = expand;
    tree_rebuild();
}

// --- index and find ---------------------------------------------------------------------------------------

static const char **g_kw_items;            // the keywords that pass the filter
static int *g_kw_idx, g_nkw_shown;
static const char **g_top_items;           // the topics of the chosen keyword
static int *g_top_idx, g_ntop;
static const char **g_fd_items;            // Find's results
static int *g_fd_idx, g_nfd;
static char **g_fts;                       // each topic's text, lowercased, for Find
static int g_fts_n;
static int g_fts_for = -1;

static void free_find_index(void) {
    for (int i = 0; i < g_fts_n; i++) free(g_fts[i]);
    free(g_fts);
    g_fts = NULL; g_fts_n = 0; g_fts_for = -1;
}

static int contains_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

static void keywords_rebuild(void) {
    const struct uhlp *h = cur_help();
    free(g_kw_items); free(g_kw_idx);
    g_kw_items = NULL; g_kw_idx = NULL; g_nkw_shown = 0;
    if (!h) { uui_listbox_set_items(&g_kwlist, NULL, 0); return; }
    int nk = uhlp_nkeywords(h);
    g_kw_items = malloc((size_t)(nk ? nk : 1) * sizeof *g_kw_items);
    g_kw_idx = malloc((size_t)(nk ? nk : 1) * sizeof *g_kw_idx);
    if (!g_kw_items || !g_kw_idx) return;
    const char *f = uui_textbox_text(&g_filter);
    for (int i = 0; i < nk; i++) {
        const char *k = uhlp_keyword(h, i);
        if (f[0] && !contains_ci(k, f)) continue;
        g_kw_items[g_nkw_shown] = k;
        g_kw_idx[g_nkw_shown++] = i;
    }
    uui_listbox_set_items(&g_kwlist, g_kw_items, g_nkw_shown);
    g_kwlist.selected = g_nkw_shown ? 0 : -1;
}

static void topics_for_keyword(int row) {
    const struct uhlp *h = cur_help();
    free(g_top_items); free(g_top_idx);
    g_top_items = NULL; g_top_idx = NULL; g_ntop = 0;
    if (h && row >= 0 && row < g_nkw_shown) {
        int n = 0;
        const int *t = uhlp_keyword_topics(h, g_kw_idx[row], &n);
        g_top_items = malloc((size_t)(n ? n : 1) * sizeof *g_top_items);
        g_top_idx = malloc((size_t)(n ? n : 1) * sizeof *g_top_idx);
        if (g_top_items && g_top_idx)
            for (int i = 0; i < n; i++) {
                g_top_items[g_ntop] = uhlp_topic_title(h, t[i]);
                g_top_idx[g_ntop++] = t[i];
            }
    }
    uui_listbox_set_items(&g_topics, g_top_items, g_ntop);
    g_topics.selected = g_ntop ? 0 : -1;
}

static void find_build_index(void) {
    const struct uhlp *h = cur_help();
    if (!h || g_fts_for == g_cf) return;
    free_find_index();
    int n = uhlp_ntopics(h);
    g_fts = calloc((size_t)(n ? n : 1), sizeof *g_fts);
    if (!g_fts) return;
    g_fts_n = n;
    g_fts_for = g_cf;
    for (int t = 0; t < n; t++) {
        char *s = uhlp_topic_text(h, t);
        if (s) for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p |= 0x20;
        g_fts[t] = s;
    }
}

static void find_run(void) {
    const struct uhlp *h = cur_help();
    free(g_fd_items); free(g_fd_idx);
    g_fd_items = NULL; g_fd_idx = NULL; g_nfd = 0;
    char q[96];
    snprintf(q, sizeof q, "%s", uui_textbox_text(&g_find));
    for (char *p = q; *p; p++) if (*p >= 'A' && *p <= 'Z') *p |= 0x20;
    if (h && q[0]) {
        find_build_index();
        int n = uhlp_ntopics(h);
        g_fd_items = malloc((size_t)(n ? n : 1) * sizeof *g_fd_items);
        g_fd_idx = malloc((size_t)(n ? n : 1) * sizeof *g_fd_idx);
        for (int t = 0; t < n && g_fd_items && g_fd_idx && g_fts; t++) {
            const char *text = g_fts[t];
            if (!text) continue;
            int ok = 1;
            const char *w = q;
            while (*w && ok) {                              // every word, anywhere in the topic
                while (*w == ' ') w++;
                size_t wn = strcspn(w, " ");
                if (!wn) break;
                char word[64];
                snprintf(word, sizeof word, "%.*s", (int)wn, w);
                ok = strstr(text, word) != NULL;
                w += wn;
            }
            if (!ok) continue;
            const char *title = uhlp_topic_title(h, t);
            g_fd_items[g_nfd] = title[0] ? title : "(untitled)";
            g_fd_idx[g_nfd++] = t;
        }
    }
    uui_listbox_set_items(&g_found, g_fd_items, g_nfd);
    g_found.selected = g_nfd ? 0 : -1;
}

// --- showing a topic ------------------------------------------------------------------------------------------

static void reap_popup(void) {
    if (g_pdoc && !uui_helpview_popup_open(&g_view)) { uhlp_doc_free(g_pdoc); g_pdoc = NULL; }
}

static void set_status(const char *msg) { snprintf(g_status, sizeof g_status, "%s", msg); }

static void update_status(void) {
    const struct uhlp *h = cur_help();
    if (!h) { g_stat_file[0] = g_stat_topic[0] = 0; return; }
    const char *slash = strrchr(g_f[g_cf].path, '/');
    snprintf(g_stat_file, sizeof g_stat_file, "%s", slash ? slash + 1 : g_f[g_cf].path);
    snprintf(g_stat_topic, sizeof g_stat_topic, "Topic %d of %d", g_ct + 1, uhlp_ntopics(h));
}

static void run_topic_macros(const struct uhlp_doc *d);

// Shows `topic` of `file` at `scroll`. Does not touch the history.
static void show(int file, int topic, int scroll) {
    if (file < 0 || file >= g_nf) return;
    const struct uhlp *h = g_f[file].h;
    if (topic < 0 || topic >= uhlp_ntopics(h)) topic = uhlp_contents(h);
    struct uhlp_doc *d = uhlp_render(h, topic);
    if (!d) { set_status("That topic could not be read"); return; }
    uui_helpview_popup_close(&g_view);
    uui_helpview_set_doc(&g_view, h, d, scroll);   // the view lets go of the old document
    reap_popup();
    uhlp_doc_free(g_doc);
    g_doc = d;
    if (file != g_cf) {
        g_cf = file;
        tn_build(h);
        keywords_rebuild();
        topics_for_keyword(0);
        free_find_index();
        find_run();
        uapp_set_title(g_app, uhlp_title(h)[0] ? uhlp_title(h) : "Help Viewer");
    }
    g_ct = topic;
    g_status[0] = 0;
    if (g_tn_of && g_tn_of[topic] >= 0) {                  // reveal it in the contents
        for (int n = g_tn_of[topic]; g_tn[n].parent >= 0; n = g_tn[n].parent) g_tn[g_tn[n].parent].open = 1;
    }
    tree_rebuild();
    update_status();
    buttons_rebuild(g_app);
    ulogf("hlpview: showing \"%s\" (topic %d)\n", uhlp_topic_title(h, topic), topic);
    run_topic_macros(d);
}

static void go(int file, int topic) {
    if (g_hpos >= 0) g_hist[g_hpos].scroll = g_view.scroll;
    g_hn = g_hpos + 1;
    if (g_hn == g_hcap) {
        int nc = g_hcap ? g_hcap * 2 : 32;
        struct hist *g = realloc(g_hist, (size_t)nc * sizeof *g);
        if (!g) return;
        g_hist = g;
        g_hcap = nc;
    }
    g_hist[g_hn] = (struct hist){ file, topic, 0 };
    g_hpos = g_hn++;
    show(file, topic, 0);
}

static void step_history(int by) {
    int to = g_hpos + by;
    if (to < 0 || to >= g_hn) return;
    g_hist[g_hpos].scroll = g_view.scroll;
    g_hpos = to;
    show(g_hist[to].file, g_hist[to].topic, g_hist[to].scroll);
}

// --- the History list: a popup under its button -----------------------------------------------------------
//
// The topics visited, newest first, the current one ticked: a pick goes there with Back and Forward
// still working from it (the history is a trail with a position, as a browser's is). A popup is as
// tall as the window, so it shows the newest HIST_ROWS and says how many it left out. Kept for the
// session; Clear history empties the trail to the topic shown.

#define HIST_ROWS 20
#define TB_HISTORY 7                      // the History button's place in TOOLBAR, separators counted

static struct uui_menubar g_hmenu;
static struct uui_menu_item g_hitems[HIST_ROWS + 4];
static char g_hlabel[HIST_ROWS][112], g_hmore[48];

static unsigned hmenu_flags(int code) {
    if (code == CMD_HIST_MORE) return UUI_MI_DISABLED;
    return code == CMD_HIST_BASE + g_hpos ? UUI_MI_CHECKED : 0;
}

static void open_history(struct uapp *a) {
    int n = 0, shown = 0;
    for (int i = g_hn - 1; i >= 0 && shown < HIST_ROWS; i--, shown++) {
        const struct hist *e = &g_hist[i];
        const char *t = uhlp_topic_title(g_f[e->file].h, e->topic);
        snprintf(g_hlabel[shown], sizeof g_hlabel[shown], "%s", t[0] ? t : "(untitled)");
        const char *base = strrchr(g_f[e->file].path, '/');
        g_hitems[n++] = (struct uui_menu_item)UUI_MENU(g_hlabel[shown], CMD_HIST_BASE + i,
                                                       g_nf > 1 ? (base ? base + 1 : g_f[e->file].path) : 0);
    }
    if (g_hn > shown) {
        snprintf(g_hmore, sizeof g_hmore, "... and %d earlier", g_hn - shown);
        g_hitems[n++] = (struct uui_menu_item)UUI_MENU(g_hmore, CMD_HIST_MORE, 0);
    }
    g_hitems[n++] = (struct uui_menu_item)UUI_MENU_SEP;
    g_hitems[n++] = (struct uui_menu_item)UUI_MENU("Clear history", CMD_HIST_CLEAR, 0);
    uui_menubar_set_bounds(&g_hmenu, 0, 0, uapp_width(a), uapp_height(a));
    int x, y, w, h;
    uui_toolbar_item_rect(&g_tb, TB_HISTORY, &x, &y, &w, &h);
    uui_menubar_open_below(&g_hmenu, g_hitems, n, x, y, w, h);
}

// A row of that menu was chosen.
static void history_pick(int code) {
    if (code >= CMD_HIST_BASE && code - CMD_HIST_BASE < g_hn) {
        step_history(code - CMD_HIST_BASE - g_hpos);
    } else if (code == CMD_HIST_CLEAR && g_hpos >= 0) {
        g_hist[g_hpos].scroll = g_view.scroll;
        g_hist[0] = g_hist[g_hpos];
        g_hn = 1;
        g_hpos = 0;
    }
}

// --- macros ------------------------------------------------------------------------------------------------------

static void run_macro(const char *m);

static void run_topic_macros(const struct uhlp_doc *d) {
    for (int i = 0; i < d->nmacro; i++) run_macro(d->macro[i]);
}

// --- following a link --------------------------------------------------------------------------------------------

static int resolve(const struct uui_helpview_link *lk, int *file, int *topic) {
    *file = g_cf;
    if (lk->file[0]) {
        char *p = find_sibling(g_f[g_cf].path, lk->file);
        if (!p) { char m[160]; snprintf(m, sizeof m, "Cannot find the help file %s", lk->file); set_status(m); return 0; }
        *file = load_file(p);
        free(p);
        if (*file < 0) return 0;
    }
    const struct uhlp *h = g_f[*file].h;
    *topic = lk->by_offset ? uhlp_topic_by_offset(h, lk->hash, NULL) : uhlp_topic_by_hash(h, lk->hash);
    if (*topic < 0) { set_status("That topic is not in the help file"); return 0; }
    return 1;
}

static void follow(const struct uui_helpview_link *lk) {
    if (lk->kind == UHLP_LINK_MACRO) {
        g_manchor[0] = lk->ax; g_manchor[1] = lk->ay; g_manchor[2] = lk->aw; g_manchor[3] = lk->ah;
        run_macro(lk->macro);
        g_manchor[2] = g_manchor[3] = 0;
        return;
    }
    int file, topic;
    if (!resolve(lk, &file, &topic)) return;
    g_status[0] = 0;
    if (lk->kind == UHLP_LINK_POPUP) {
        struct uhlp_doc *d = uhlp_render(g_f[file].h, topic);
        if (!d) return;
        uui_helpview_popup(&g_view, g_f[file].h, d, lk->ax, lk->ay, lk->aw, lk->ah);
        uhlp_doc_free(g_pdoc);
        g_pdoc = d;
        return;
    }
    go(file, topic);
}

// --- commands -------------------------------------------------------------------------------------------------------

static void relayout(struct uapp *a) { uui_layout_run(&ROOT_L, 0, 0, uapp_width(a), uapp_height(a)); }

static void set_tab(struct uapp *a, int tab) {
    g_tab = tab;
    g_seg.selected = tab;
    LEFT[1].hidden = tab != TAB_CONTENTS;
    LEFT[2].hidden = tab != TAB_INDEX;
    LEFT[3].hidden = tab != TAB_INDEX;
    LEFT[4].hidden = tab != TAB_INDEX;
    LEFT[5].hidden = tab != TAB_FIND;
    LEFT[6].hidden = tab != TAB_FIND;
    BODY[0].hidden = !g_pane;
    relayout(a);
}

// What is selected in the topic, or the whole topic when nothing is.
static void copy_topic(void) {
    const struct uhlp *h = cur_help();
    if (!h || g_ct < 0) return;
    char *s = uui_helpview_selection_text(&g_view);
    if (!s) s = uhlp_topic_text(h, g_ct);
    if (!s) return;
    uclip_set_text(s, (int)strlen(s));
    free(s);
    set_status(uui_helpview_has_selection(&g_view) ? "Selection copied" : "Topic copied");
}

static void command(struct uapp *a, int code) {
    const struct uhlp *h = cur_help();
    if (code != CMD_COPY) g_status[0] = 0;
    switch (code) {
    case CMD_BACK: step_history(-1); break;
    case CMD_FORWARD: step_history(1); break;
    case CMD_HOME: if (h) go(g_cf, uhlp_contents(h)); break;
    case CMD_PREV: { int t = h ? uhlp_topic_prev(h, g_ct) : -1; if (t >= 0) go(g_cf, t); break; }
    case CMD_NEXT: { int t = h ? uhlp_topic_next(h, g_ct) : -1; if (t >= 0) go(g_cf, t); break; }
    case CMD_PANE: g_pane = !g_pane; set_tab(a, g_tab); break;
    case CMD_COPY: copy_topic(); break;
    case CMD_ZOOM_IN: uui_helpview_set_zoom(&g_view, g_view.zoom + 1); break;
    case CMD_ZOOM_OUT: uui_helpview_set_zoom(&g_view, g_view.zoom - 1); break;
    case CMD_ZOOM_RESET: uui_helpview_set_zoom(&g_view, 0); break;
    case CMD_HISTORY: open_history(a); break;
    default: break;
    }
    uapp_redraw(a);
}

static unsigned item_flags(int code) {
    const struct uhlp *h = cur_help();
    switch (code) {
    case CMD_BACK: return g_hpos > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_FORWARD: return g_hpos < g_hn - 1 ? 0 : UUI_MI_DISABLED;
    case CMD_HISTORY: return g_hn > 1 ? 0 : UUI_MI_DISABLED;      // nowhere to go but here
    case CMD_PREV: return h && uhlp_topic_prev(h, g_ct) >= 0 ? 0 : UUI_MI_DISABLED;
    case CMD_NEXT: return h && uhlp_topic_next(h, g_ct) >= 0 ? 0 : UUI_MI_DISABLED;
    case CMD_PANE: return g_pane ? UUI_MI_CHECKED : 0;
    case CMD_ZOOM_IN: return g_view.zoom >= UHV_ZOOM_MAX ? UUI_MI_DISABLED : 0;
    case CMD_ZOOM_OUT: return g_view.zoom <= UHV_ZOOM_MIN ? UUI_MI_DISABLED : 0;
    case CMD_ZOOM_RESET: return g_view.zoom == 0 ? UUI_MI_DISABLED : 0;
    default: return 0;
    }
}

// --- the macro language (lib/uhlpmac.h) -----------------------------------------------------------------------
//
// What a help file runs: its |SYSTEM macros when it opens (in 3.11's own files, one each: the Glossary button),
// a topic's when it is shown, a hotspot's when it is clicked, a button's when it is pressed. The language is
// the library's; this is what the viewer does for it. Buttons are kept per file and shown under the contents
// list for the file on screen; a macro's "file.hlp>window" names a secondary window this viewer does not have,
// so the jump goes in this window.

struct mbtn { int file; char *id, *caption, *macro; int disabled; };
static struct mbtn *g_mb;
static int g_nmb, g_mbcap;
struct mmark { char *name; int file, topic, scroll; };
static struct mmark *g_mm;
static int g_nmm, g_mmcap;
static int g_mfile = -1;                   // the file whose macro is running; -1 is the one shown
static int g_mui;                          // the widgets exist (set once main has built them)

#define MB_CODE 5000                       // a button's code is MB_CODE + its place in g_mb
static struct uui_button *g_bw;            // the buttons under the list: those of the file shown
static struct uui_item *g_bi;
static struct uui_layout BTN_L;
static int g_nbw;

static int macro_base(void) { return g_mfile >= 0 ? g_mfile : g_cf; }

static int mb_find(int file, const char *id) {
    for (int i = 0; i < g_nmb; i++)
        if (g_mb[i].file == file && !strcasecmp(g_mb[i].id, id)) return i;
    return -1;
}

static void buttons_rebuild(struct uapp *a) {
    if (!g_mui || !a) return;
    free(g_bw);
    free(g_bi);
    g_bw = 0; g_bi = 0; g_nbw = 0;
    int n = 0;
    for (int i = 0; i < g_nmb; i++) if (g_mb[i].file == g_cf) n++;
    if (n) {
        g_bw = calloc((size_t)n, sizeof *g_bw);
        g_bi = calloc((size_t)n, sizeof *g_bi);
        if (!g_bw || !g_bi) { free(g_bw); free(g_bi); g_bw = 0; g_bi = 0; n = 0; }
    }
    for (int i = 0; i < g_nmb && g_nbw < n; i++) {
        if (g_mb[i].file != g_cf) continue;
        uui_button_init(&g_bw[g_nbw], 0, 0, 0, 0, g_mb[i].caption, UUI_ROLE(control_bg), UUI_ROLE(text), MB_CODE + i);
        g_bw[g_nbw].outlined = 1;
        g_bw[g_nbw].disabled = g_mb[i].disabled;
        g_bi[g_nbw] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_bw[g_nbw], .id = MB_CODE + i, .flags = UUI_FILL_W };
        g_nbw++;
    }
    BTN_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_bi, .count = g_nbw, .gap = 4 };
    LEFT[7].hidden = g_nbw == 0;
    relayout(a);
    uapp_redraw(a);
}

// A caption without its & (the mnemonic the file marks).
static char *caption_of(const char *s) {
    char *o = malloc(strlen(s) + 1), *w = o;
    if (!o) return 0;
    for (; *s; s++) {
        if (*s == '&' && s[1] == '&') { *w++ = '&'; s++; }
        else if (*s != '&') *w++ = *s;
    }
    *w = 0;
    return o;
}

static void mac_button(void *c, int op, const char *id, const char *caption, const char *macro) {
    (void)c;
    int file = macro_base();
    if (file < 0) return;
    int i = mb_find(file, id);
    if (op == UHLPMAC_BTN_CREATE) {
        char *cap = caption_of(caption), *mac = strdup(macro);
        if (!cap || !mac) { free(cap); free(mac); return; }
        if (i < 0) {
            if (g_nmb == g_mbcap) {
                int nc = g_mbcap ? g_mbcap * 2 : 8;
                struct mbtn *g = realloc(g_mb, (size_t)nc * sizeof *g);
                if (!g) { free(cap); free(mac); return; }
                g_mb = g;
                g_mbcap = nc;
            }
            char *idc = strdup(id);
            if (!idc) { free(cap); free(mac); return; }
            g_mb[g_nmb] = (struct mbtn){ file, idc, cap, mac, 0 };
            g_nmb++;
        } else {
            free(g_mb[i].caption); free(g_mb[i].macro);
            g_mb[i].caption = cap;
            g_mb[i].macro = mac;
        }
    } else if (i >= 0) {
        if (op == UHLPMAC_BTN_DESTROY) {
            free(g_mb[i].id); free(g_mb[i].caption); free(g_mb[i].macro);
            memmove(&g_mb[i], &g_mb[i + 1], (size_t)(g_nmb - i - 1) * sizeof *g_mb);
            g_nmb--;
        } else if (op == UHLPMAC_BTN_ENABLE || op == UHLPMAC_BTN_DISABLE) {
            g_mb[i].disabled = op == UHLPMAC_BTN_DISABLE;
        } else if (op == UHLPMAC_BTN_BIND) {
            char *mac = strdup(macro);
            if (mac) { free(g_mb[i].macro); g_mb[i].macro = mac; }
        }
    }
    buttons_rebuild(g_app);
}

static int mac_mark(void *c, int op, const char *name) {
    (void)c;
    int at = -1;
    for (int i = 0; i < g_nmm; i++) if (!strcmp(g_mm[i].name, name)) at = i;
    switch (op) {
    case UHLPMAC_MARK_IS: return at >= 0;
    case UHLPMAC_MARK_GOTO:
        if (at >= 0) go(g_mm[at].file, g_mm[at].topic);
        return 0;
    case UHLPMAC_MARK_DELETE:
        if (at >= 0) {
            free(g_mm[at].name);
            memmove(&g_mm[at], &g_mm[at + 1], (size_t)(g_nmm - at - 1) * sizeof *g_mm);
            g_nmm--;
        }
        return 0;
    default:
        break;
    }
    if (at < 0) {
        if (g_nmm == g_mmcap) {
            int nc = g_mmcap ? g_mmcap * 2 : 8;
            struct mmark *g = realloc(g_mm, (size_t)nc * sizeof *g);
            if (!g) return 0;
            g_mm = g;
            g_mmcap = nc;
        }
        char *nm = strdup(name);
        if (!nm) return 0;
        at = g_nmm++;
        g_mm[at].name = nm;
    }
    ulogf("hlpview: mark \"%s\" saved at topic %d\n", name, g_ct);
    g_mm[at].file = g_cf;
    g_mm[at].topic = g_ct;
    g_mm[at].scroll = g_view.scroll;
    return 0;
}

// The topic a macro's jump names: its help file ("" is this one; a ">window" suffix is dropped), then the
// context string, number, hash or keyword. 0 with the reason in the status line when it is not there.
static int macro_target(int target, const char *file, const char *id, unsigned long num, int *f, int *t) {
    int base = macro_base();
    char name[160];
    const char *gt = strchr(file, '>');
    snprintf(name, sizeof name, "%.*s", (int)(gt ? (size_t)(gt - file) : strlen(file)), file);
    *f = base;
    if (name[0]) {
        char *p = base >= 0 ? find_sibling(g_f[base].path, name) : NULL;
        if (!p) { char m[200]; snprintf(m, sizeof m, "Cannot find the help file %s", name); set_status(m); return 0; }
        *f = load_file(p);
        free(p);
    }
    if (*f < 0) return 0;
    const struct uhlp *h = g_f[*f].h;
    *t = -1;
    switch (target) {
    case UHLPMAC_ID: *t = uhlp_topic_by_hash(h, uhlp_hash(id)); break;
    case UHLPMAC_HASH: *t = uhlp_topic_by_hash(h, (uint32_t)num); break;
    case UHLPMAC_CONTEXT: *t = uhlp_topic_by_map(h, (uint32_t)num); break;
    case UHLPMAC_TOC: *t = uhlp_contents(h); break;
    case UHLPMAC_KEYWORD:
        for (int i = 0; i < uhlp_nkeywords(h); i++) {
            if (strcasecmp(uhlp_keyword(h, i), id)) continue;
            int n;
            const int *tp = uhlp_keyword_topics(h, i, &n);
            if (n > 0) *t = tp[0];
            break;
        }
        break;
    default: break;
    }
    if (*t < 0) { set_status("That topic is not in the help file"); return 0; }
    return 1;
}

static void mac_jump(void *c, int popup, int target, const char *file, const char *id, unsigned long num) {
    (void)c;
    int f, t;
    if (!macro_target(target, file, id, num, &f, &t)) return;
    g_status[0] = 0;
    if (popup) {
        struct uhlp_doc *d = uhlp_render(g_f[f].h, t);
        if (!d) return;
        int ax = g_manchor[0], ay = g_manchor[1], aw = g_manchor[2], ah = g_manchor[3];
        if (!aw) { ax = g_view.x + 12; ay = g_view.y + 12; aw = 24; ah = 16; }
        uui_helpview_popup(&g_view, g_f[f].h, d, ax, ay, aw, ah);
        uhlp_doc_free(g_pdoc);
        g_pdoc = d;
        return;
    }
    go(f, t);
}

static void mac_command(void *c, int cmd) {
    (void)c;
    switch (cmd) {
    case UHLPMAC_CONTENTS: command(g_app, CMD_HOME); break;
    case UHLPMAC_SEARCH: set_tab(g_app, TAB_FIND); uui_focus_set(&g_focus, 1); break;
    case UHLPMAC_BACK: command(g_app, CMD_BACK); break;
    case UHLPMAC_NEXT: command(g_app, CMD_NEXT); break;
    case UHLPMAC_PREV: command(g_app, CMD_PREV); break;
    case UHLPMAC_HISTORY: command(g_app, CMD_HISTORY); break;
    case UHLPMAC_COPY: command(g_app, CMD_COPY); break;
    case UHLPMAC_EXIT: uapp_quit(g_app, 0); break;
    default: break;                        // BrowseButtons: the toolbar always has them
    }
}

static void mac_unsupported(void *c, const char *name, const char *why) {
    (void)c;
    ulogf("hlpview: macro %s: %s\n", name, why);
}

static const struct uhlpmac_ops MAC_OPS = { 0, mac_jump, mac_command, mac_button, mac_mark, mac_unsupported };

static void run_macro(const char *m) {
    if (!m || !m[0]) return;
    const char *err = "";
    if (uhlpmac_run(m, &MAC_OPS, &err) < 0) ulogf("hlpview: macro not run (%s): %s\n", err, m);
}

static void run_file_macros(int idx) {
    const struct uhlp *h = g_f[idx].h;
    int was = g_mfile;
    g_mfile = idx;
    for (int i = 0; i < uhlp_nmacros(h); i++) run_macro(uhlp_macro(h, i));
    g_mfile = was;
}

// A button of the file's was pressed.
static void on_action(struct uapp *a, int code) {
    if (code < MB_CODE || code - MB_CODE >= g_nmb) return;
    struct mbtn *b = &g_mb[code - MB_CODE];
    char *mac = strdup(b->macro);          // the macro may destroy or change this very button
    if (!mac) return;
    int was = g_mfile;
    g_mfile = b->file;
    run_macro(mac);
    g_mfile = was;
    free(mac);
    uapp_redraw(a);
}

// --- events --------------------------------------------------------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    switch (id) {
    case ID_HISTMENU: {
        int code = uui_menubar_take_code(&g_hmenu);
        if (code >= 0) history_pick(code);
        break;
    }
    case ID_TOOLBAR: {
        int code = uui_toolbar_take_code(&g_tb);
        if (code > 0) command(a, code);
        break;
    }
    case ID_TABS:
        if (g_seg.selected != g_tab) set_tab(a, g_seg.selected);
        break;
    case ID_TREE:
        if (reason == UUI_REASON_PRESS || reason == UUI_REASON_RELEASE || reason == UUI_REASON_KEY) {
            int n = uui_tree_selected_id(&g_tree);
            if (n >= 0 && n < g_ntn && g_tn[n].topic >= 0 && g_tn[n].topic != g_ct) go(g_cf, g_tn[n].topic);
        }
        break;
    case ID_FILTER:
        keywords_rebuild();
        topics_for_keyword(g_kwlist.selected);
        break;
    case ID_KEYWORDS:
        topics_for_keyword(g_kwlist.selected);
        if (reason == UUI_REASON_RELEASE && g_ntop == 1) go(g_cf, g_top_idx[0]);
        break;
    case ID_TOPICS:
        if (reason == UUI_REASON_RELEASE && g_topics.selected >= 0 && g_topics.selected < g_ntop)
            go(g_cf, g_top_idx[g_topics.selected]);
        break;
    case ID_FIND:
        find_run();
        break;
    case ID_FOUND:
        if (reason == UUI_REASON_RELEASE && g_found.selected >= 0 && g_found.selected < g_nfd) {
            go(g_cf, g_fd_idx[g_found.selected]);
            uui_helpview_set_highlight(&g_view, uui_textbox_text(&g_find));
        }
        break;
    case ID_VIEW: {
        struct uui_helpview_link lk;
        if (uui_helpview_take_link(&g_view, &lk)) follow(&lk);
        switch (uui_helpview_take_command(&g_view)) {         // a right-click menu row
        case UHV_CMD_COPY: command(a, CMD_COPY); break;
        case UHV_CMD_BACK: command(a, CMD_BACK); break;
        case UHV_CMD_FORWARD: command(a, CMD_FORWARD); break;
        case UHV_CMD_CONTENTS: command(a, CMD_HOME); break;
        default: break;
        }
        reap_popup();
        break;
    }
    default: break;
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    if (uui_menubar_is_open(&g_hmenu)) {                // the History list, while it is up
        int code;
        if (uui_menubar_key(&g_hmenu, key, &code)) {
            if (code >= 0) history_pick(code);
            uapp_redraw(a);
            return;
        }
    }
    int text_focus = g_focus.current == 0 || g_focus.current == 1;
    if (mods & KEY_MOD_ALT) {
        if (key == KEY_ARROW_LEFT) command(a, CMD_BACK);
        else if (key == KEY_ARROW_RIGHT) command(a, CMD_FORWARD);
        else return;
    } else if ((mods & KEY_MOD_CTRL) && key == KEY_HOME) {
        command(a, CMD_HOME);
    } else if ((mods & KEY_MOD_CTRL) && (key == 'c' || key == 0x03)) {
        command(a, CMD_COPY);
    } else if ((mods & KEY_MOD_CTRL) && (key == '+' || key == '=')) {
        command(a, CMD_ZOOM_IN);
    } else if ((mods & KEY_MOD_CTRL) && key == '-') {
        command(a, CMD_ZOOM_OUT);
    } else if ((mods & KEY_MOD_CTRL) && key == '0') {
        command(a, CMD_ZOOM_RESET);
    } else if ((mods & KEY_MOD_CTRL) && (key == 'a' || key == 0x01) && !text_focus) {
        uui_helpview_select_all(&g_view);
    } else if (uui_helpview_popup_open(&g_view) && key == 0x1B) {
        uui_helpview_popup_close(&g_view);
        reap_popup();
    } else if (key == 0x06) {                         // Ctrl-F: to Find
        set_tab(a, TAB_FIND);
        uui_focus_set(&g_focus, 1);
    } else if (!text_focus && uui_helpview_key(&g_view, key, mods)) {
        // scrolled
    } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        uui_helpview_key(&g_view, key, mods);
    } else if (text_focus && (key == '\n' || key == '\r')) {
        if (g_focus.current == 1 && g_nfd) { g_found.selected = 0; go(g_cf, g_fd_idx[0]); uui_helpview_set_highlight(&g_view, uui_textbox_text(&g_find)); }
        else if (g_focus.current == 0 && g_ntop) go(g_cf, g_top_idx[g_topics.selected >= 0 ? g_topics.selected : 0]);
    } else if (g_focus.current == 0) {
        keywords_rebuild();
        topics_for_keyword(g_kwlist.selected);
    } else if (g_focus.current == 1) {
        find_run();
    } else {
        return;
    }
    uapp_redraw(a);
}

static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    g_bar.panes[2].text = g_status;
    uui_menubar_set_bounds(&g_hmenu, 0, 0, uapp_width(a), uapp_height(a));
    {   // where the History button is: the toolbar's own report names no button of a strip that fits
        int bx, by, bw, bh;
        if (uui_toolbar_item_rect(&g_tb, TB_HISTORY, &bx, &by, &bw, &bh))
            uapp_logf_layout("hlpview: toolbar history %d %d %d %d\n", bx, by, bw, bh);
    }
    g_view.can_back = g_hpos > 0;                    // what the right-click menu greys by
    g_view.can_fwd = g_hpos < g_hn - 1;
    uapp_log_layout(a, "hlpview");
    uapp_logf_layout("hlpview: here \"%s\" topic %d file %d back %d fwd %d popup %d zoom %d\n",
                     g_ct >= 0 ? uhlp_topic_title(cur_help(), g_ct) : "-", g_ct, g_cf, g_hpos > 0,
                     g_hpos < g_hn - 1, uui_helpview_popup_open(&g_view), g_view.zoom);
}

static int on_tick(struct uapp *a) {
    (void)a;
    reap_popup();
    return uui_toolbar_tick(&g_tb);
}

static void on_size(int *w, int *h) {
    *w = ugfx_char_advance('n') * 120;
    *h = ugfx_char_h() * 38;
}

// In main() every font measure is 0, so the sizes that follow the font are set here.
static void size_panes(void) {
    int sw = 0, sh = 0;
    uui_segmented_natural_size(&g_seg, &sw, &sh);
    int w = ugfx_char_advance('n') * 30;
    BODY[0].main_size = sw + 8 > w ? sw + 8 : w;
    LEFT[4].main_size = ugfx_char_h() * 7;
}

static void on_font(struct uapp *a) {
    size_panes();
    relayout(a);
}

static int g_opt_context = -1;
static const char *g_opt_keyword, *g_opt_macro, *g_opt_file;

static void on_open(struct uapp *a) {
    g_app = a;
    g_mui = 1;
    size_panes();
    char *path = g_opt_file ? strdup(g_opt_file) : NULL;
    int f = path ? load_file(path) : -1;
    free(path);
    if (f < 0) {
        set_status(g_opt_file ? "That help file could not be opened" : "Open a help file: hlpview FILE.HLP");
        return;
    }
    const struct uhlp *h = g_f[f].h;
    int topic = uhlp_contents(h);
    if (g_opt_context >= 0) {
        int t = uhlp_topic_by_map((const struct uhlp *)h, (uint32_t)g_opt_context);
        if (t >= 0) topic = t;
    }
    if (g_opt_keyword) {
        for (int i = 0; i < uhlp_nkeywords(h); i++)
            if (!strcasecmp(uhlp_keyword(h, i), g_opt_keyword)) {
                int n = 0;
                const int *t = uhlp_keyword_topics(h, i, &n);
                if (n > 0) topic = t[0];
                break;
            }
    }
    go(f, topic);
    if (g_opt_macro) run_macro(g_opt_macro);
    set_tab(a, TAB_CONTENTS);
}

static int on_close(struct uapp *a) {
    (void)a;
    uui_helpview_free(&g_view);
    uhlp_doc_free(g_doc);
    uhlp_doc_free(g_pdoc);
    for (int i = 0; i < g_nf; i++) { uhlp_close(g_f[i].h); free(g_f[i].data); free(g_f[i].path); }
    return 1;
}

// A help file is untrusted input. Give up what a viewer does not do --
// spawning, the network, devices -- before the first byte is read, and
// refuse to run if that is not possible. It still READS (a help file's
// neighbours, fonts, settings) and keeps the window and shared memory
// the desktop needs.
static int hlpview_sandbox(void) {
    if (unveil("/", "r") != 0) return -1;
    if (unveil(UNVEIL_SHM_PREFIX, "r") != 0) return -1;
    if (unveil(NULL, NULL) != 0) return -1;
    return pledge("stdio rpath shm window settings query", NULL);
}

// A word from a command line that splits at spaces: %20 is a space, %25 a '%'.
static char *decode(const char *s) {
    char *o = strdup(s), *w = o;
    if (!o) return NULL;
    for (const char *r = s; *r; r++) {
        if (r[0] == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
    return o;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) g_opt_context = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) g_opt_keyword = decode(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) g_opt_macro = decode(argv[++i]);
        else if (argv[i][0] != '-') g_opt_file = decode(argv[i]);
    }
    if (hlpview_sandbox() != 0) {
        fprintf(stderr, "hlpview: could not enter its sandbox -- not reading help files\n");
        return 2;
    }

    uui_menubar_init(&g_hmenu, 0, 0);
    g_hmenu.item_flags = hmenu_flags;
    uui_toolbar_init(&g_tb, TOOLBAR, (int)(sizeof TOOLBAR / sizeof TOOLBAR[0]));
    g_tb.item_flags = item_flags;
    g_tb.accent_latch = 1;
    uui_segmented_init(&g_seg, TAB_NAMES, 3, TAB_CONTENTS);
    uui_tree_init(&g_tree, 0, 0, 0, 0, g_nodes, 0);
    uui_tree_set_on_toggle(&g_tree, on_toggle, NULL);
    g_tree.sel_style = UUI_SEL_STRONG;
    uui_textbox_init(&g_filter, "");
    g_filter.placeholder = "Type a keyword";
    uui_textbox_init(&g_find, "");
    g_find.placeholder = "Find words in the topics";
    uui_listbox_init(&g_kwlist, 0, 0, 0, 0, NULL, 0);
    uui_listbox_init(&g_topics, 0, 0, 0, 0, NULL, 0);
    uui_listbox_init(&g_found, 0, 0, 0, 0, NULL, 0);
    uui_helpview_init(&g_view);
    uui_statusbar_init(&g_bar);
    g_bar.panes[0].text = g_stat_file;
    g_bar.panes[0].chars = 20;
    g_bar.panes[1].text = g_stat_topic;
    g_bar.panes[1].chars = 16;
    g_bar.panes[2].text = g_status;
    g_bar.panes[2].chars = 0;
    g_bar.count = 3;

    uui_focus_init(&g_focus, g_focusables, (int)(sizeof g_focusables / sizeof g_focusables[0]));
    uui_focus_set(&g_focus, 2);

    LEFT[0] = (struct uui_item){ .ops = &uui_segmented_ops, .widget = &g_seg, .id = ID_TABS, .name = "tabs" };
    LEFT[1] = (struct uui_item){ .ops = &uui_tree_ops, .widget = &g_tree, .id = ID_TREE, .name = "tree",
                                 .flags = UUI_FILL_W | UUI_FILL_H };
    LEFT[2] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_filter, .id = ID_FILTER, .name = "filter",
                                 .flags = UUI_FILL_W, .hidden = 1 };
    LEFT[3] = (struct uui_item){ .ops = &uui_listbox_ops, .widget = &g_kwlist, .id = ID_KEYWORDS, .name = "keywords",
                                 .flags = UUI_FILL_W | UUI_FILL_H, .hidden = 1 };
    LEFT[4] = (struct uui_item){ .ops = &uui_listbox_ops, .widget = &g_topics, .id = ID_TOPICS, .name = "topics",
                                 .flags = UUI_FILL_W, .hidden = 1 };
    LEFT[5] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_find, .id = ID_FIND, .name = "find",
                                 .flags = UUI_FILL_W, .hidden = 1 };
    LEFT[6] = (struct uui_item){ .ops = &uui_listbox_ops, .widget = &g_found, .id = ID_FOUND, .name = "found",
                                 .flags = UUI_FILL_W | UUI_FILL_H, .hidden = 1 };
    LEFT[7] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BTN_L, .name = "macro-buttons",
                                 .flags = UUI_FILL_W, .hidden = 1 };
    LEFT_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = LEFT, .count = 8, .gap = 6 };

    RIGHT[0] = (struct uui_item){ .ops = &uui_helpview_ops, .widget = &g_view, .id = ID_VIEW, .name = "view",
                                  .flags = UUI_FILL_W | UUI_FILL_H };
    RIGHT_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = RIGHT, .count = 1 };

    BODY[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &LEFT_L, .flags = UUI_FILL_H };
    BODY[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &RIGHT_L, .flags = UUI_FILL_W | UUI_FILL_H };
    BODY_L = (struct uui_layout){ .dir = UUI_ROW, .items = BODY, .count = 2, .margin = 6, .gap = 6 };

    ITEMS[0] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &g_tb, .id = ID_TOOLBAR, .name = "toolbar",
                                  .flags = UUI_FILL_W };
    ITEMS[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BODY_L, .flags = UUI_FILL_W | UUI_FILL_H };
    ITEMS[2] = (struct uui_item){ .ops = &uui_statusbar_ops, .widget = &g_bar, .name = "status", .flags = UUI_FILL_W };
    ROOT_L = (struct uui_layout){ .dir = UUI_COLUMN, .items = ITEMS, .count = 3, .margin = 1, .gap = 1 };
    static struct uui_item root = { .ops = &uui_layout_ops, .widget = &ROOT_L };
    // The History list drops over everything, so it goes first (a popup's place in the order).
    static struct uui_item widgets[2];
    widgets[0] = (struct uui_item){ .ops = &uui_menubar_ops, .widget = &g_hmenu, .id = ID_HISTMENU, .name = "history" };
    widgets[1] = root;

    struct uapp_desc desc = {
        .title        = "Help Viewer",
        .app_id       = "hlpview",
        .on_size      = on_size,
        .on_font      = on_font,
        .layout       = &ROOT_L,
        .widgets      = widgets,
        .widget_count = 2,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
        .on_draw_over = on_draw_over,
        .on_tick      = on_tick,
        .tick_ms      = 250,
        .on_open      = on_open,
        .on_close     = on_close,
        .focus        = &g_focus,
        .flags        = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
