// Software -- the optional components, as a store: DOOM, the Web page
// engine and the Browser, the compiler, Windows 3.x support, the demos
// (docs/software-design.md). Cards by default (S1, chosen on the Design
// canvas 2026-11-09), a list with a details pane as a View option (S2);
// categories on the left, search, the "Show GPL-licensed apps" switch and
// Check server along the top.
//
// CHANGES QUEUE, AS DISKS' DO: Install and Remove mark a card, the footer
// names each change and its size, Discard drops them, and Apply runs them
// in order on a worker through uui_steplist (Disks' shape, GNOME
// Software's and Windows' Optional features' one batch). Needs= is
// followed both ways when a change is queued: removing the engine queues
// the Browser too, adding the Browser queues the engine.
//
// The engine is the command's: base/ucomponent.h reads, removes and keeps
// the state; common/upd.h adds back, fetched at THIS machine's build
// (`component`, docs/commands/component.md, is the same thing typed).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "base/human.h"
#include "base/ucomponent.h"
#include "base/uinitials.h"
#include "base/ulog.h"
#include "base/usetting.h"
#include "common/upd.h"
#include "media/icon_cache.h"
#include "media/uclip.h"
#include "media/uimg.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui.h"
#include "ui/uui_appcard.h"
#include "ui/uui_builder.h"
#include "ui/uui_button.h"
#include "ui/uui_dialog.h"
#include "ui/uui_focus.h"
#include "ui/uui_menubar.h"
#include "ui/uui_primitives.h"
#include "ui/uui_props.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_steplist.h"
#include "ui/uui_switch.h"
#include "ui/uui_textbox.h"
#include "ui/uui_tree.h"
#include "keyboard.h"

enum {
    ID_MENU = 1, ID_SEARCH, ID_GPL, ID_CHECK, ID_CATS, ID_CARDS, ID_PROPS, ID_DETAIL_BTN,
    ID_SHOW_GPL, ID_DISCARD, ID_APPLY, ID_STATUS, ID_DIALOG, ID_RUN,
};
enum {
    CMD_CHECK = 1, CMD_APPLY, CMD_DISCARD, CMD_EXIT, CMD_CARDS, CMD_LIST, CMD_GPL, CMD_ABOUT,
    ASK_CANCEL = 100, ASK_APPLY, ASK_CLOSE,
};
enum { P_NONE = 0, P_REMOVE, P_ADD };
// The sidebar's fixed rows; a category is CAT_FIRST + its index.
enum { CAT_ALL = 1, CAT_INSTALLED, CAT_AVAILABLE, CAT_FIRST = 10 };

// ---- the model ------------------------------------------------------------------

static struct ucomponent_set g_set;
static int *g_pend;                  // per component: P_*
static int *g_queue, g_nqueue;       // the pending changes, in the order they run
static int g_cat = CAT_ALL;          // the sidebar's selection
static int g_show_gpl = 1;           // software.show_gpl, as last read
static int g_view = UUI_APPCARD_CARDS;

// What the view shows: a card per visible component, its strings kept here.
struct card_text { char kind[64], size[48], badge[24], status[24], glyph[12]; };
static struct uui_appcard *g_cards;
static struct card_text *g_text;
static int *g_card_comp, g_ncards, g_cards_cap;

static char g_cat_keys[16][24];      // the categories present, in the set's order
static int g_ncat_keys;

static const char *short_licence(const char *spdx, char *out, size_t cap) {
    // "GPL-2.0-or-later" -> "GPL-2.0+", "GPL-2.0-only" -> "GPL-2.0": the badge's room.
    snprintf(out, cap, "%s", spdx);
    char *s;
    if ((s = strstr(out, "-or-later"))) { s[0] = '+'; s[1] = '\0'; }
    else if ((s = strstr(out, "-only"))) *s = '\0';
    return out;
}

static void size_text(uint64_t bytes, char *out, size_t cap) { human_size(out, cap, bytes); }

static int installed(int i) { return !g_set.c[i].removed; }
static int gpl_hidden(int i) { return !g_show_gpl && ucomponent_is_gpl(&g_set.c[i]); }

// The installed components that need `i`, as names: "Web Browser".
static void needed_by(int i, char *out, size_t cap) {
    out[0] = '\0';
    size_t n = 0;
    for (int k = 0; k < g_set.count; k++)
        if (k != i && installed(k) && ucomponent_needs(&g_set.c[k], g_set.c[i].id) && n + 4 < cap)
            n += (size_t)snprintf(out + n, cap - n, "%s%s", n ? ", " : "", g_set.c[k].name);
}

static void needs_names(int i, char *out, size_t cap) {
    out[0] = '\0';
    size_t n = 0;
    for (int k = 0; k < g_set.count; k++)
        if (ucomponent_needs(&g_set.c[i], g_set.c[k].id) && n + 4 < cap)
            n += (size_t)snprintf(out + n, cap - n, "%s%s", n ? ", " : "", g_set.c[k].name);
}

// The Web page engine draws Help's and System Update's pages: said
// before it is removed (the design's warning).
static int holds_engine(int i) { return ucomponent_owner(&g_set, "/bin/webcontent") == i; }

static int matches(int i, const char *q) {
    if (!q || !q[0]) return 1;
    const struct ucomponent *c = &g_set.c[i];
    const char *fields[] = { c->name, c->summary, c->project, c->id, ucomponent_category_label(c->category) };
    for (size_t f = 0; f < sizeof fields / sizeof fields[0]; f++) {
        const char *h = fields[f];
        size_t ql = strlen(q);
        for (const char *p = h; *p; p++) {
            size_t k = 0;
            while (k < ql && p[k]) {
                char a = p[k], b = q[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
                k++;
            }
            if (k == ql) return 1;
        }
    }
    return 0;
}

static int in_cat(int i, int cat) {
    if (cat == CAT_ALL) return 1;
    if (cat == CAT_INSTALLED) return installed(i);
    if (cat == CAT_AVAILABLE) return !installed(i);
    int k = cat - CAT_FIRST;
    return k >= 0 && k < g_ncat_keys && !strcmp(g_set.c[i].category, g_cat_keys[k]);
}

static const uint32_t TILE[] = { 0x2F5D9C, 0xB23A32, 0x6B4BB8, 0xC77428, 0x555A66, 0x1F7F78 };

// ---- the queue -----------------------------------------------------------------

static void queue_drop(int i) {
    int k = 0;
    for (int q = 0; q < g_nqueue; q++) if (g_queue[q] != i) g_queue[k++] = g_queue[q];
    g_nqueue = k;
    g_pend[i] = P_NONE;
}

static void queue_put(int i, int op) {
    if (g_pend[i] == op) return;
    if (g_pend[i]) queue_drop(i);
    g_pend[i] = op;
    g_queue[g_nqueue++] = i;
}

// Whether component i is installed once the queue has run.
static int will_have(int i) {
    return g_pend[i] == P_ADD || (installed(i) && g_pend[i] != P_REMOVE);
}

// THE QUEUE IS KEPT WHOLE, BOTH WAYS: after every change, nothing that
// will be installed needs something that will not. Where the two
// disagree, the component just chosen (`pinned`) wins, and so does each
// one its choice decided: Keep on the Browser keeps the engine it needs,
// Install on the Browser takes back the engine's queued removal, Cancel
// on the engine's install cancels the Browser's. Otherwise the needed one
// is kept, the less destructive answer. Each fix decides one more
// component, so it ends.
static void queue_settle(int pinned) {
    char *fixed = calloc((size_t)g_set.count + 1, 1);
    if (!fixed) return;
    if (pinned >= 0) fixed[pinned] = 1;
    for (int again = 1; again; ) {
        again = 0;
        for (int d = 0; d < g_set.count && !again; d++) {
            if (!will_have(d)) continue;
            for (int k = 0; k < g_set.count && !again; k++) {
                if (k == d || will_have(k) || !ucomponent_needs(&g_set.c[d], g_set.c[k].id)) continue;
                if (fixed[k] && fixed[d]) continue;
                if (fixed[k]) {             // k stays out, so d goes too
                    if (g_pend[d] == P_ADD) queue_drop(d);
                    else queue_put(d, P_REMOVE);
                    fixed[d] = 1;
                } else {                    // d stays, so k does
                    if (g_pend[k] == P_REMOVE) queue_drop(k);
                    else queue_put(k, P_ADD);
                    fixed[k] = 1;
                }
                again = 1;
            }
        }
    }
    free(fixed);
}

// Install, Remove, or take back a queued one -- the card's button.
static void toggle(int i) {
    if (i < 0 || i >= g_set.count) return;
    int *tmp = malloc(sizeof *tmp * (size_t)(g_set.count + 1));
    if (!tmp) return;
    const char *verb = g_pend[i] ? "unqueued" : installed(i) ? "queued remove" : "queued add";
    if (g_pend[i]) {
        queue_drop(i);
    } else if (installed(i)) {
        int n = ucomponent_remove_closure(&g_set, i, tmp, g_set.count);
        for (int k = 0; k < n; k++) queue_put(tmp[k], P_REMOVE);
    } else {
        int n = ucomponent_add_closure(&g_set, i, tmp, g_set.count);
        for (int k = 0; k < n; k++) queue_put(tmp[k], P_ADD);
    }
    queue_settle(i);
    ulogf("software: %s %s, %d pending\n", verb, g_set.c[i].id, g_nqueue);
    free(tmp);
}

static void discard(void) {
    for (int i = 0; i < g_set.count; i++) g_pend[i] = P_NONE;
    g_nqueue = 0;
    ulogf("software: discarded the changes\n");
}

// ---- widgets ----------------------------------------------------------------------

static struct uui_menubar g_menu;
static struct uui_textbox g_search;
static struct uui_switch g_gpl;
static struct uui_button g_check, g_show, g_discard, g_apply, g_detail_btn;
static struct uui_tree g_cats;
// Three fixed rows, a heading, and a row per Category= -- which the build
// holds to components.py's CATEGORIES, nine words: a bound the data has.
static struct uui_tree_node g_cat_nodes[24];
static char g_cat_notes[24][12];
static int g_ncat_nodes;
static struct uui_appcards g_view_w;
static struct uui_props g_props;
static struct uui_statusbar g_sb;
static struct uui_dialog g_ask;
static struct uui_focus g_focus;
static struct uui_focusable g_ring[3];
static char g_st_count[64], g_st_server[96], g_st_build[48], g_note[160];
static char g_banner[128], g_foot_head[24], g_foot_text[256];
static int g_body_y, g_body_h, g_side_w, g_pane_x, g_foot_y, g_banner_y;

static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,       .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_textbox_ops,   .widget = &g_search,     .id = ID_SEARCH,  .name = "search" },
    { .ops = &uui_switch_ops,    .widget = &g_gpl,        .id = ID_GPL,     .name = "gpl" },
    { .ops = &uui_button_ops,    .widget = &g_check,      .id = ID_CHECK,   .name = "check" },
    { .ops = &uui_tree_ops,      .widget = &g_cats,       .id = ID_CATS,    .name = "cats" },
    { .ops = &uui_appcards_ops,  .widget = &g_view_w,     .id = ID_CARDS,   .name = "cards" },
    { .ops = &uui_props_ops,     .widget = &g_props,      .id = ID_PROPS,   .name = "details", .hidden = 1 },
    { .ops = &uui_button_ops,    .widget = &g_detail_btn, .id = ID_DETAIL_BTN, .name = "detailbutton", .hidden = 1 },
    { .ops = &uui_button_ops,    .widget = &g_show,       .id = ID_SHOW_GPL, .name = "showgpl", .hidden = 1 },
    { .ops = &uui_button_ops,    .widget = &g_discard,    .id = ID_DISCARD, .name = "discard", .hidden = 1 },
    { .ops = &uui_button_ops,    .widget = &g_apply,      .id = ID_APPLY,   .name = "apply", .hidden = 1 },
    { .ops = &uui_statusbar_ops, .widget = &g_sb,         .id = ID_STATUS,  .name = "status" },
    { .ops = &uui_dialog_ops,    .widget = &g_ask,        .id = ID_DIALOG,  .name = "dialog" },
};
enum { W_PROPS = 6, W_DETAIL_BTN, W_SHOW, W_DISCARD, W_APPLY };

static int detail_shown(void) { return g_view == UUI_APPCARD_ROWS; }

static int sel_comp(void) {
    int s = g_view_w.selected;
    return s >= 0 && s < g_ncards ? g_card_comp[s] : -1;
}

// ---- building what is shown -------------------------------------------------------

static void build_cats(void) {
    int count[3] = { 0, 0, 0 }, per[16] = { 0 };
    g_ncat_keys = 0;
    for (int i = 0; i < g_set.count; i++) {
        int k = 0;
        while (k < g_ncat_keys && strcmp(g_cat_keys[k], g_set.c[i].category)) k++;
        if (k == g_ncat_keys && g_ncat_keys < 16) snprintf(g_cat_keys[g_ncat_keys++], sizeof g_cat_keys[0], "%s",
                                                           g_set.c[i].category);
        if (gpl_hidden(i)) continue;
        count[0]++;
        count[installed(i) ? 1 : 2]++;
        if (k < 16) per[k]++;
    }
    static const char *const FIXED[] = { "All", "Installed", "Available" };
    static const char *const ICON[] = { "cat-all", "cat-system", "sysupdate" };
    g_ncat_nodes = 0;
    for (int f = 0; f < 3; f++) {
        snprintf(g_cat_notes[g_ncat_nodes], sizeof g_cat_notes[0], "%d", count[f]);
        g_cat_nodes[g_ncat_nodes] = (struct uui_tree_node){ .label = FIXED[f], .id = CAT_ALL + f,
                                                            .icon = ICON[f], .note = g_cat_notes[g_ncat_nodes] };
        g_ncat_nodes++;
    }
    g_cat_nodes[g_ncat_nodes++] = (struct uui_tree_node){ .label = "Categories", .kind = UUI_TREE_HEADER, .id = 0 };
    static const char *const CAT_ICON[][2] = { { "games", "cat-games" }, { "internet", "cat-network" },
        { "devtools", "cat-development" }, { "win16", "win16" }, { "demos", "uidemo" }, { "system", "cat-system" },
        { "graphics", "cat-graphics" }, { "multimedia", "cat-multimedia" }, { "utility", "cat-utility" } };
    for (int k = 0; k < g_ncat_keys && g_ncat_nodes < 24; k++) {
        const char *icon = "cat-all";
        for (size_t t = 0; t < sizeof CAT_ICON / sizeof CAT_ICON[0]; t++)
            if (!strcmp(CAT_ICON[t][0], g_cat_keys[k])) icon = CAT_ICON[t][1];
        snprintf(g_cat_notes[g_ncat_nodes], sizeof g_cat_notes[0], "%d", per[k]);
        g_cat_nodes[g_ncat_nodes] = (struct uui_tree_node){ .label = ucomponent_category_label(g_cat_keys[k]),
                                                            .id = CAT_FIRST + k, .icon = icon,
                                                            .note = g_cat_notes[g_ncat_nodes] };
        g_ncat_nodes++;
    }
    uui_tree_set_nodes_keep(&g_cats, g_cat_nodes, g_ncat_nodes);
    // select_id() answers 0 for "already selected" as well as "no such
    // id" -- a click has selected the row already, so ask what is
    // selected, or every category click snaps back to All.
    uui_tree_select_id(&g_cats, g_cat);
    if (uui_tree_selected_id(&g_cats) != g_cat) { g_cat = CAT_ALL; uui_tree_select_id(&g_cats, g_cat); }
}

static void fill_card(int n, int i) {
    const struct ucomponent *c = &g_set.c[i];
    struct card_text *t = &g_text[n];
    struct uui_appcard *a = &g_cards[n];
    memset(a, 0, sizeof *a);
    const char *cat = ucomponent_category_label(c->category);
    if (c->project[0]) snprintf(t->kind, sizeof t->kind, "%s \xc2\xb7 %s", cat, c->project);
    else snprintf(t->kind, sizeof t->kind, "%s", cat);
    char sz[24], by[96];
    size_text(c->size, sz, sizeof sz);
    needed_by(i, by, sizeof by);
    if (by[0] && installed(i) && g_view == UUI_APPCARD_CARDS)   // the list's details pane says it
        snprintf(t->size, sizeof t->size, "%s \xc2\xb7 needed by %s", sz, by);
    else snprintf(t->size, sizeof t->size, "%s", sz);
    short_licence(c->license, t->badge, sizeof t->badge);
    snprintf(t->status, sizeof t->status, "%s", installed(i) ? "Installed" : "Not installed");
    a->name = c->name;
    a->kind = t->kind;
    a->summary = c->summary;
    a->badge = t->badge;
    a->badge_warm = ucomponent_is_gpl(c);
    a->size = t->size;
    a->status = t->status;
    a->icon = c->icon[0] ? c->icon : 0;
    uinitials(c->name, t->glyph, sizeof t->glyph);   // the tile's room: a letter or two
    a->glyph = t->glyph;
    int ci = 0;
    for (int k = 0; k < g_ncat_keys; k++) if (!strcmp(g_cat_keys[k], c->category)) ci = k;
    a->tile = TILE[ci % (int)(sizeof TILE / sizeof TILE[0])];
    if (g_pend[i] == P_REMOVE) {
        a->button = "Keep"; a->button_kind = UUI_APPCARD_BTN_PLAIN;
        a->pending = "Will be removed"; a->pending_danger = 1;
    } else if (g_pend[i] == P_ADD) {
        a->button = "Cancel"; a->button_kind = UUI_APPCARD_BTN_PLAIN;
        a->pending = "Will be installed";
    } else if (installed(i)) {
        a->button = "Remove"; a->button_kind = UUI_APPCARD_BTN_REMOVE;
    } else {
        a->button = "Install"; a->button_kind = UUI_APPCARD_BTN_ADD;
    }
}

static void build_cards(void) {
    int keep = sel_comp();
    const char *q = uui_textbox_text(&g_search);
    if (g_cards_cap < g_set.count + 1) {
        int cap = g_set.count + 1;
        struct uui_appcard *a = realloc(g_cards, sizeof *a * (size_t)cap);
        if (a) g_cards = a;
        struct card_text *t = realloc(g_text, sizeof *t * (size_t)cap);
        if (t) g_text = t;
        int *m = realloc(g_card_comp, sizeof *m * (size_t)cap);
        if (m) g_card_comp = m;
        if (!a || !t || !m) { ulogf("software: out of memory for %d cards\n", cap); return; }
        g_cards_cap = cap;
    }
    g_ncards = 0;
    int hidden = 0;
    for (int i = 0; i < g_set.count; i++) {
        if (gpl_hidden(i)) { hidden++; continue; }
        if (!in_cat(i, g_cat) || !matches(i, q)) continue;
        g_card_comp[g_ncards] = i;
        fill_card(g_ncards, i);
        g_ncards++;
    }
    uui_appcards_set(&g_view_w, g_cards, g_ncards);
    int sel = -1;
    for (int n = 0; n < g_ncards; n++) if (g_card_comp[n] == keep) sel = n;
    if (sel < 0 && detail_shown() && g_ncards) sel = 0;
    g_view_w.selected = sel;
    // The GPL switch's banner: what is hidden, and that it still works.
    if (hidden)
        snprintf(g_banner, sizeof g_banner, "%d GPL-licensed component%s hidden. What %s still works.", hidden,
                 hidden == 1 ? " is" : "s are", hidden == 1 ? "needs it" : "needs them");
    else
        g_banner[0] = '\0';
    g_view_w.empty = g_set.count ? (q && q[0] ? "Nothing matches the search." : "Nothing here.")
                                 : "This build has no optional components.";
}

static void build_props(void) {
    uui_props_begin(&g_props, 1);
    int i = sel_comp();
    if (i < 0) return;
    const struct ucomponent *c = &g_set.c[i];
    int s = uui_props_section(&g_props, "Details", UUI_PROPS_PLAIN);
    uui_props_set_action(&g_props, s, "Copy details");
    uui_props_row(&g_props, "", "%s", c->summary);
    uui_props_row(&g_props, "Licence", "%s", c->license);
    char sz[24], names[160];
    size_text(c->size, sz, sizeof sz);
    uui_props_row(&g_props, "Size", "%s, %d file%s", sz, c->files, c->files == 1 ? "" : "s");
    uui_props_row(&g_props, "Status", "%s", g_pend[i] == P_REMOVE ? "Installed; will be removed"
                                           : g_pend[i] == P_ADD ? "Not installed; will be installed"
                                           : installed(i) ? "Installed" : "Not installed");
    needed_by(i, names, sizeof names);
    if (holds_engine(i)) {
        size_t n = strlen(names);
        snprintf(names + n, sizeof names - n, "%sHelp, System Update", n ? ", " : "");
    }
    uui_props_row(&g_props, "Needed by", "%s", names[0] ? names : "Nothing else");
    needs_names(i, names, sizeof names);
    uui_props_row(&g_props, "Needs", "%s", names[0] ? names : "Only the system");
    uui_props_row(&g_props, "Id", "%s", c->id);
    if (ucomponent_is_gpl(c)) {
        static char server[UPD_URL_MAX];
        upd_server_get(server, sizeof server);
        uui_props_row(&g_props, "Source", "%s/<channel>/source/ -- this build's, from the update server", server);
    }
    // What a removal costs, said before it happens.
    char dep[160];
    needed_by(i, dep, sizeof dep);
    if (installed(i) && (dep[0] || holds_engine(i))) {
        int w = uui_props_section(&g_props, "", UUI_PROPS_NOTICE);
        (void)w;
        if (dep[0]) uui_props_row(&g_props, "", "Removing it also removes %s.", dep);
        if (holds_engine(i))
            uui_props_row(&g_props, "", "Help and System Update's notes then show their pages as plain text.");
    }
}

static void build_status(void) {
    int n = 0, inst = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < g_set.count; i++) {
        if (gpl_hidden(i)) continue;
        n++;
        if (installed(i)) { inst++; bytes += g_set.c[i].size; }
    }
    char sz[24];
    size_text(bytes, sz, sizeof sz);
    snprintf(g_st_count, sizeof g_st_count, "%d of %d installed \xc2\xb7 %s", inst, n, sz);
    char commit[24];
    upd_machine_commit(commit, sizeof commit);
    snprintf(g_st_build, sizeof g_st_build, "Build %s", commit);
}

static void build_footer(void) {
    snprintf(g_foot_head, sizeof g_foot_head, g_nqueue == 1 ? "%d change" : "%d changes", g_nqueue);
    size_t n = 0;
    g_foot_text[0] = '\0';
    for (int q = 0; q < g_nqueue && n + 8 < sizeof g_foot_text; q++) {
        int i = g_queue[q];
        char sz[24];
        size_text(g_set.c[i].size, sz, sizeof sz);
        n += (size_t)snprintf(g_foot_text + n, sizeof g_foot_text - n, "%s%s %s (%s%s)", q ? " \xc2\xb7 " : "",
                              g_pend[i] == P_REMOVE ? "Remove" : "Add", g_set.c[i].name, sz,
                              g_pend[i] == P_ADD ? " to download" : "");
    }
}

static void rebuild(void) {
    g_show_gpl = ucomponent_show_gpl();
    g_gpl.on = g_show_gpl;
    build_cats();
    build_cards();
    build_props();
    build_status();
    build_footer();
}

static void load(void) {
    ucomponent_load(&g_set);
    free(g_pend);
    free(g_queue);
    g_pend = calloc((size_t)g_set.count + 1, sizeof *g_pend);
    g_queue = calloc((size_t)g_set.count + 1, sizeof *g_queue);
    g_nqueue = 0;
    ulogf("software: %d components\n", g_set.count);
}

// ---- the worker: Check server, and Apply ------------------------------------------

enum { EV_CHECKED = 1, EV_STEP, EV_STEP_DONE, EV_DONE, EV_PROGRESS };
static struct uapp *g_app;
static struct {
    int running, kind;               // JOB_*
    int count, cur, failed;
    int *comp, *op, *rc;
    char (*why)[200];
    struct uui_step *rows;
    char (*text)[96];
    int build_gone;
    int at_boot;                     // something finishes at the next restart
    char from[UPD_URL_MAX];          // the channel an add or a check used
    uint64_t last_post_ns;
} g_job;
enum { JOB_CHECK = 1, JOB_APPLY };
static struct uui_steplist g_run;
static struct uui_item g_run_item = { .ops = &uui_table_ops, .widget = &g_run.table, .id = ID_RUN, .name = "applysteps" };
static char g_check_why[160];
static int g_check_rc = 1;           // 1: not checked yet

// Both go where the machine's build came from (upd.h), not the newest
// build of whatever channel is configured.
static void *check_worker(void *arg) {
    (void)arg;
    static struct upd_plan p;
    g_check_why[0] = '\0';
    g_check_rc = upd_component_check(NULL, &p, 0);
    g_job.build_gone = p.build_gone;
    snprintf(g_job.from, sizeof g_job.from, "%s", p.from);
    if (g_check_rc) snprintf(g_check_why, sizeof g_check_why, "%s", p.error);
    upd_plan_free(&p);
    uapp_post(g_app, EV_CHECKED, 0);
    return 0;
}

static void progress_post(int i, int permille) {
    g_job.rows[i].permille = permille;
    uint64_t now = sys_monotonic_ns();
    if (now - g_job.last_post_ns > 100000000ull) {   // a tenth of a second
        g_job.last_post_ns = now;
        uapp_post(g_app, EV_PROGRESS, i);
    }
}

static void rm_file(void *ctx, int done, int total) {
    progress_post((int)(intptr_t)ctx, total ? done * 1000 / total : -1);
}

// One run fetches every addition: file `idx` moves the row of the
// component that holds it, by that component's own bytes.
static void add_progress(void *ctx, const struct upd_plan *p, int idx) {
    (void)ctx;
    if (idx < 0 || idx >= p->count) return;
    const char *id = p->files[idx].component;
    for (int s = 0; s < g_job.count; s++) {
        if (g_job.op[s] != P_ADD || strcmp(g_set.c[g_job.comp[s]].id, id) != 0) continue;
        uint64_t want = 0, got = 0;
        for (int i = 0; i < p->count; i++) {
            const struct upd_file *f = &p->files[i];
            if ((f->change != UPD_NEW && f->change != UPD_CHANGED) || strcmp(f->component, id) != 0) continue;
            want += f->size;
            got += f->got;
        }
        progress_post(s, want ? (int)(got * 1000 / want) : -1);
        return;
    }
}

// The removals one by one, offline, then EVERY addition in one run: one
// manifest fetched, and a set holding a library staged for the restart
// as a whole -- one add per component would fetch the manifest each time,
// and every add after one that staged for the boot would be refused.
static void *apply_worker(void *arg) {
    (void)arg;
    static struct upd_plan p;
    int failed = 0, nadd = 0;
    for (int s = 0; s < g_job.count && !failed; s++) {
        if (g_job.op[s] != P_REMOVE) { nadd++; continue; }
        uapp_post(g_app, EV_STEP, s);
        struct ucomponent_progress pr = { (void *)(intptr_t)s, 0, rm_file };
        int boot = 0;
        g_job.rc[s] = ucomponent_remove(&g_set, g_job.comp[s], &pr, &boot, g_job.why[s], (int)sizeof g_job.why[s]);
        g_job.at_boot |= boot;
        failed = g_job.rc[s] != 0;
        uapp_post(g_app, EV_STEP_DONE, s);
    }
    if (nadd && !failed) {
        const char **ids = calloc((size_t)nadd, sizeof *ids);
        int n = 0, rc;
        const char *why = 0;
        for (int s = 0; s < g_job.count && ids; s++)
            if (g_job.op[s] == P_ADD) {
                ids[n++] = g_set.c[g_job.comp[s]].id;
                uapp_post(g_app, EV_STEP, s);
            }
        if (!ids) {
            rc = -1;
            why = "out of memory";
        } else if (g_job.at_boot) {
            // A removal waits for the restart, and an add now would be
            // refused as "an update is already staged": said plainly.
            rc = -1;
            why = "a removal finishes at the restart -- restart, then install the rest";
        } else {
            struct upd_hooks h = { .progress = add_progress };
            rc = upd_component_add(NULL, (const char *const *)ids, n, &p, &h);
            g_job.at_boot |= p.at_boot;
            g_job.build_gone |= p.build_gone;
            snprintf(g_job.from, sizeof g_job.from, "%s", p.from);
            why = p.error;
        }
        // One run: its rows share its outcome.
        for (int s = 0; s < g_job.count; s++) {
            if (g_job.op[s] != P_ADD) continue;
            g_job.rc[s] = rc;
            if (rc) snprintf(g_job.why[s], sizeof g_job.why[s], "%s", why ? why : "");
            uapp_post(g_app, EV_STEP_DONE, s);
        }
        upd_plan_free(&p);
        free(ids);
    }
    uapp_post(g_app, EV_DONE, 0);
    return 0;
}

static int start_thread(void *(*fn)(void *)) {
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    g_job.running = 1;
    if (pthread_create(&th, &at, fn, 0) != 0) { g_job.running = 0; return -1; }
    return 0;
}

static void job_free(void) {
    free(g_job.comp); free(g_job.op); free(g_job.rc); free(g_job.why); free(g_job.rows); free(g_job.text);
    memset(&g_job, 0, sizeof g_job);
}

static void say(const char *msg) {
    snprintf(g_note, sizeof g_note, "%s", msg);
    ulogf("software: note %s\n", msg);
}

static void do_check(void) {
    if (g_job.running) return;
    job_free();
    g_job.kind = JOB_CHECK;
    snprintf(g_st_server, sizeof g_st_server, "Checking the update server...");
    if (start_thread(check_worker) != 0) say("Could not start the check");
}

// The confirm: every step in its order.
static int apply_load(void) {
    job_free();
    int n = g_nqueue;
    g_job.comp = calloc((size_t)n + 1, sizeof *g_job.comp);
    g_job.op = calloc((size_t)n + 1, sizeof *g_job.op);
    g_job.rc = calloc((size_t)n + 1, sizeof *g_job.rc);
    g_job.why = calloc((size_t)n + 1, sizeof *g_job.why);
    g_job.rows = calloc((size_t)n + 1, sizeof *g_job.rows);
    g_job.text = calloc((size_t)n + 1, sizeof *g_job.text);
    if (!g_job.comp || !g_job.op || !g_job.rc || !g_job.why || !g_job.rows || !g_job.text) { job_free(); return -1; }
    // Removals first, then the additions: an add fetches, and a removal
    // never has to wait on the network. A removal another queued removal
    // needs goes after it (the Browser, then the engine), so one that
    // fails part of the way never leaves an app without what it needs.
    for (int pass = 0; pass < 3; pass++)
        for (int q = 0; q < n; q++) {
            int i = g_queue[q];
            int needed = 0;
            for (int r = 0; r < n; r++)
                needed |= g_pend[g_queue[r]] == P_REMOVE && g_queue[r] != i &&
                          ucomponent_needs(&g_set.c[g_queue[r]], g_set.c[i].id);
            int want = g_pend[i] == P_ADD ? 2 : needed ? 1 : 0;
            if (want != pass) continue;
            int s = g_job.count++;
            g_job.comp[s] = i;
            g_job.op[s] = g_pend[i];
            char sz[24];
            size_text(g_set.c[i].size, sz, sizeof sz);
            snprintf(g_job.text[s], sizeof g_job.text[s], "%s (%s%s)", g_set.c[i].name, sz,
                     g_pend[i] == P_ADD ? " to download" : "");
            g_job.rows[s] = (struct uui_step){ g_pend[i] == P_REMOVE ? "Remove" : "Install", g_job.text[s],
                                               UUI_STEP_WAITING, -1 };
        }
    g_job.cur = g_job.failed = -1;
    uui_steplist_set(&g_run, g_job.rows, g_job.count);
    return 0;
}

static const char *g_ask_rows[3];
static char g_ask_line[3][200];

static void open_steps(struct uapp *a, const char *title, const struct uui_dialog_button *btns, int nb, int def) {
    int rows = g_job.count < 8 ? g_job.count : 8;
    uui_dialog_set_body(&g_ask, &g_run_item, ugfx_char_w() * 64, uui_steplist_height(&g_run, rows));
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_ask, title, g_ask_rows, 1, btns, nb, def, nb ? (def >= 0 ? btns[nb - 1].code : ASK_CLOSE)
                                                                    : ASK_CLOSE);
    g_ask.body_full = 1;
}

static void confirm_apply(struct uapp *a) {
    if (!g_nqueue || g_job.running) return;
    if (apply_load() < 0) { say("Out of memory"); return; }
    static char title[48];
    snprintf(title, sizeof title, g_job.count == 1 ? "Apply %d change?" : "Apply %d changes?", g_job.count);
    int adds = 0;
    for (int s = 0; s < g_job.count; s++) adds += g_job.op[s] == P_ADD;
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s",
             adds ? "They run in this order. An install downloads at this machine's own build."
                  : "They run in this order, and free the space at once.");
    g_ask_rows[0] = g_ask_line[0];
    static struct uui_dialog_button btns[2];
    btns[0] = (struct uui_dialog_button){ "Apply", ASK_APPLY, UUI_DLG_PRIMARY };
    btns[1] = (struct uui_dialog_button){ "Cancel", ASK_CANCEL, 0 };
    open_steps(a, title, btns, 2, 0);
    ulogf("software: ask apply %d\n", g_job.count);
}

static void start_apply(struct uapp *a) {
    g_job.kind = JOB_APPLY;
    static char title[48];
    snprintf(title, sizeof title, g_job.count == 1 ? "Applying %d change" : "Applying %d changes", g_job.count);
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s", "Wait for the changes to finish.");
    open_steps(a, title, 0, 0, -1);
    if (start_thread(apply_worker) != 0) { say("Could not apply the changes"); uui_dialog_close(&g_ask); return; }
    ulogf("software: apply %d started\n", g_job.count);
}

static void apply_done(struct uapp *a) {
    g_job.running = 0;
    int ok = 0;
    for (int s = 0; s < g_job.count; s++) ok += g_job.rows[s].state == UUI_STEP_DONE;
    // What ran is no longer pending; the rest stays queued.
    int *done = calloc((size_t)g_set.count + 1, sizeof *done);
    for (int s = 0; s < g_job.count && done; s++) if (g_job.rows[s].state == UUI_STEP_DONE) done[g_job.comp[s]] = 1;
    int *pend = calloc((size_t)g_set.count + 1, sizeof *pend), *queue = calloc((size_t)g_set.count + 1, sizeof *queue);
    int nq = 0;
    for (int q = 0; q < g_nqueue && done && pend && queue; q++)
        if (!done[g_queue[q]]) { queue[nq++] = g_queue[q]; pend[g_queue[q]] = g_pend[g_queue[q]]; }
    // Reload: the state changed. The same definitions, so indices hold.
    ucomponent_load(&g_set);
    if (done && pend && queue) {
        memcpy(g_pend, pend, sizeof *pend * (size_t)g_set.count);
        memcpy(g_queue, queue, sizeof *queue * (size_t)nq);
        g_nqueue = nq;
    }
    free(done); free(pend); free(queue);
    static char title[64];
    int failed = g_job.failed >= 0;
    snprintf(title, sizeof title, failed ? "%d of %d done" : "Done", ok, g_job.count);
    if (failed) {
        // The engine's own words, which say "update first" for a pruned build.
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "%.190s.", g_job.why[g_job.failed]);
        g_ask_line[0][0] = (char)(g_ask_line[0][0] >= 'a' && g_ask_line[0][0] <= 'z' ? g_ask_line[0][0] - 32
                                                                                     : g_ask_line[0][0]);
    } else if (g_job.at_boot) {
        // A library in the set: the kernel finishes it before init, as an update's.
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s", "Restart to finish: the changes take effect "
                 "when the machine starts again.");
    } else {
        snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s", ok == 1 ? "The change is made." : "The changes are made.");
    }
    if (g_job.at_boot) say("Restart to finish the changes");
    if (g_job.from[0]) snprintf(g_st_server, sizeof g_st_server, "From %s", g_job.from);
    static struct uui_dialog_button close[1];
    close[0] = (struct uui_dialog_button){ "Close", ASK_CLOSE, 0 };
    open_steps(a, title, close, 1, 0);
    ulogf("software: apply done %d of %d%s%s%s\n", ok, g_job.count, g_job.at_boot ? " (restart to finish)" : "",
          failed ? ": " : "", failed ? g_ask_line[0] : "");
    rebuild();
}

// ---- commands -----------------------------------------------------------------------

static void set_view(int v) {
    g_view = v;
    uui_appcards_set_layout(&g_view_w, v);
    g_widgets[W_PROPS].hidden = g_widgets[W_DETAIL_BTN].hidden = !detail_shown();
    rebuild();
    ulogf("software: view %s\n", v == UUI_APPCARD_ROWS ? "list" : "cards");
}

static void set_gpl(int on) {
    int r = usetting_set(UCOMPONENT_GPL_SETTING, on ? "on" : "off");
    ulogf("software: show_gpl %s -> %d\n", on ? "on" : "off", r);
    rebuild();
}

static void about(struct uapp *a) {
    snprintf(g_ask_line[0], sizeof g_ask_line[0], "%s", "Adds and removes the system's optional parts. A removed one "
             "stays removed through every update, and comes back at this machine's own build.");
    snprintf(g_ask_line[1], sizeof g_ask_line[1], "%s", "The command line is `component` (doc component).");
    g_ask_rows[0] = g_ask_line[0];
    g_ask_rows[1] = g_ask_line[1];
    static struct uui_dialog_button ok[1];
    ok[0] = (struct uui_dialog_button){ "Close", ASK_CLOSE, 0 };
    uui_dialog_set_body(&g_ask, 0, 0, 0);
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_ask, "Software", g_ask_rows, 2, ok, 1, 0, ASK_CLOSE);
}

static void command(struct uapp *a, int code) {
    switch (code) {
    case CMD_CHECK:   do_check(); break;
    case CMD_APPLY:   confirm_apply(a); break;
    case CMD_DISCARD: if (!g_job.running) { discard(); rebuild(); } break;
    case CMD_EXIT:    uapp_quit(a, 0); break;
    case CMD_CARDS:   set_view(UUI_APPCARD_CARDS); break;
    case CMD_LIST:    set_view(UUI_APPCARD_ROWS); break;
    case CMD_GPL:     set_gpl(!g_show_gpl); break;
    case CMD_ABOUT:   about(a); break;
    default: break;
    }
}

static unsigned item_flags(int code) {
    switch (code) {
    case CMD_APPLY: case CMD_DISCARD: return g_nqueue && !g_job.running ? 0 : UUI_MI_DISABLED;
    case CMD_CHECK: return g_job.running ? UUI_MI_DISABLED : 0;
    case CMD_CARDS: return g_view == UUI_APPCARD_CARDS ? UUI_MI_CHECKED : 0;
    case CMD_LIST:  return g_view == UUI_APPCARD_ROWS ? UUI_MI_CHECKED : 0;
    case CMD_GPL:   return g_show_gpl ? UUI_MI_CHECKED : 0;
    default: return 0;
    }
}

static const struct uui_builder_name MENU_NAMES[] = {
    { "check", CMD_CHECK }, { "apply", CMD_APPLY }, { "discard", CMD_DISCARD }, { "exit", CMD_EXIT },
    { "cards", CMD_CARDS }, { "list", CMD_LIST }, { "gpl", CMD_GPL }, { "about", CMD_ABOUT },
};

// ---- layout and drawing --------------------------------------------------------------

static void layout(int cw, int ch) {
    int pad = ugfx_char_w(), lh = ugfx_char_h(), bh = lh + pad;
    int mh = uui_menubar_height(&g_menu);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mh);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    // The command row: search, then the switch and Check server at the right.
    int rowh = bh + pad, ry = mh + (rowh - bh) / 2;
    uui_textbox_set_geometry(&g_search, pad, ry, pad * 36 < cw / 3 ? pad * 36 : cw / 3, bh);
    int kw = ugfx_text_width("Check server") + pad * 4;
    uui_button_set_geometry(&g_check, cw - pad - kw, ry, kw, bh);
    int sw, sh;
    uui_switch_natural_size(&g_gpl, &sw, &sh);
    uui_switch_set_geometry(&g_gpl, cw - pad * 3 - kw - sw, mh + (rowh - sh) / 2);
    int sbh = uui_statusbar_height(&g_sb);
    uui_statusbar_set_geometry(&g_sb, 0, ch - sbh, cw, sbh);
    g_body_y = mh + rowh + 1;
    int foot = g_nqueue ? bh + pad * 2 : 0;
    g_foot_y = ch - sbh - foot;
    g_body_h = g_foot_y - g_body_y;
    g_widgets[W_DISCARD].hidden = g_widgets[W_APPLY].hidden = !g_nqueue;
    if (g_nqueue) {
        int aw = ugfx_text_width("Apply changes") + pad * 4, dw = ugfx_text_width("Discard") + pad * 4;
        uui_button_set_geometry(&g_apply, cw - pad * 2 - aw, g_foot_y + pad, aw, bh);
        uui_button_set_geometry(&g_discard, cw - pad * 3 - aw - dw, g_foot_y + pad, dw, bh);
    }
    g_side_w = pad * 16;
    uui_tree_ops.set_geometry(&g_cats, 0, g_body_y, g_side_w, g_body_h);
    // The banner, over the content, while the switch hides something.
    int cx = g_side_w + 1, cwid = cw - cx;
    g_banner_y = 0;
    int top = g_body_y;
    g_widgets[W_SHOW].hidden = !g_banner[0];
    if (g_banner[0]) {
        g_banner_y = g_body_y;
        int shw = ugfx_text_width("Show them") + pad * 3;
        uui_button_set_geometry(&g_show, cw - pad - shw, g_banner_y + pad / 2, shw, bh);
        top += bh + pad;
    }
    int pane_w = detail_shown() ? pad * 28 : 0;
    if (pane_w > cwid / 2) pane_w = cwid / 2;
    g_pane_x = cw - pane_w;
    uui_appcards_ops.set_geometry(&g_view_w, cx, top, cwid - pane_w - (pane_w ? 1 : 0), g_foot_y - top);
    if (detail_shown()) {
        int head = lh * 3 + pad * 2;
        int by = g_foot_y - bh - pad;
        uui_props_set_geometry(&g_props, g_pane_x, top + head, pane_w, by - pad - (top + head));
        int i = sel_comp();
        g_widgets[W_DETAIL_BTN].hidden = i < 0;
        if (i >= 0) {
            g_detail_btn.label = g_pend[i] == P_REMOVE ? "Keep it" : g_pend[i] == P_ADD ? "Do not install"
                               : installed(i) ? "Remove" : "Install";
            int dw = ugfx_text_width(g_detail_btn.label) + pad * 4;
            uui_button_set_geometry(&g_detail_btn, cw - pad - dw, by, dw, bh);
            g_detail_btn.bg = installed(i) && !g_pend[i] ? UUI_COLOR_UNSET : UUI_ROLE(accent);
            g_detail_btn.fg = installed(i) && !g_pend[i] ? utheme_action(UTHEME_ACT_DANGER) : UUI_ROLE(accent_text);
            if (g_pend[i]) { g_detail_btn.bg = UUI_COLOR_UNSET; g_detail_btn.fg = UUI_ROLE(text); }
        }
    }
    uui_dialog_set_bounds(&g_ask, 0, 0, cw, ch);
}

static void draw_detail_head(struct ugfx_surface *s, int cw) {
    int i = sel_comp(), pad = ugfx_char_w(), lh = ugfx_char_h();
    int top = g_view_w.y;
    ugfx_fill_rect(s, g_pane_x - 1, top, 1, g_foot_y - top, UTHEME_SEPARATOR);
    ugfx_fill_rect(s, g_pane_x, top, cw - g_pane_x, g_foot_y - top, UTHEME_PANEL_BG);
    if (i < 0) {
        ugfx_draw_string_clipped(s, g_pane_x + pad, top + pad, cw - g_pane_x - pad * 2, "Select a component",
                                 utheme_current()->text_muted, UTHEME_PANEL_BG);
        return;
    }
    const struct ucomponent *c = &g_set.c[i];
    int t = lh * 2 + 4, x = g_pane_x + pad * 2, y = top + pad;
    struct uui_appcard tile = g_cards[g_view_w.selected];
    // The tile, as the card draws it.
    const struct uimg *im = tile.icon ? icon_get(tile.icon, t) : 0;
    if (im) ugfx_blit_alpha(s, x, y, im->w, im->h, im->px, im->w);
    else uui_fill_round_rect(s, x, y, t, t, ugfx_px(8), tile.tile);
    int nx = x + t + pad, nw = cw - pad * 2 - nx;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, nx, y, nw, c->name, UTHEME_TEXT, UTHEME_PANEL_BG);
    ugfx_set_font(was);
    char line[96];
    snprintf(line, sizeof line, "%s%s%s", c->project[0] ? c->project : ucomponent_category_label(c->category),
             " \xc2\xb7 ", installed(i) ? "installed" : "not installed");
    ugfx_draw_string_elided(s, nx, y + lh, nw, line, utheme_current()->text_muted, UTHEME_PANEL_BG);
}

static void on_draw(struct uapp *a, struct uapp_draw *dr) {
    g_app = a;
    struct ugfx_surface *s = dr->surface;
    layout(s->w, s->h);
    int pad = ugfx_char_w(), lh = ugfx_char_h();
    ugfx_fill(s, UTHEME_WINDOW_BG);
    // The command row on the chrome, a hairline under it.
    int mh = uui_menubar_height(&g_menu);
    ugfx_fill_rect(s, 0, mh, s->w, g_body_y - mh - 1, UTHEME_CHROME);
    ugfx_fill_rect(s, 0, g_body_y - 1, s->w, 1, UTHEME_SEPARATOR);
    ugfx_fill_rect(s, g_side_w, g_body_y, 1, g_foot_y - g_body_y, UTHEME_SEPARATOR);
    if (g_banner[0]) {
        int bh = lh + pad;
        uint32_t bg = ugfx_blend(UTHEME_WINDOW_BG, utheme_severity(UTHEME_SEV_WARNING), 0x30);
        ugfx_fill_rect(s, g_side_w + 1, g_banner_y, s->w - g_side_w - 1, bh + pad, bg);
        ugfx_draw_string_clipped(s, g_side_w + 1 + pad, g_banner_y + (bh + pad - lh) / 2, g_show.x - g_side_w - pad * 3,
                                 g_banner, UTHEME_TEXT, bg);
    }
    if (detail_shown()) draw_detail_head(s, s->w);
    if (g_nqueue) {
        ugfx_fill_rect(s, 0, g_foot_y, s->w, g_sb.y - g_foot_y, UTHEME_CHROME);
        ugfx_fill_rect(s, 0, g_foot_y, s->w, 1, UTHEME_SEPARATOR);
        int ty = g_apply.y + (g_apply.h - lh) / 2;
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, pad * 2, ty, g_discard.x - pad * 3, g_foot_head, UTHEME_TEXT, UTHEME_CHROME);
        int hw = ugfx_text_width(g_foot_head) + pad * 2;
        ugfx_set_font(was);
        if (pad * 2 + hw < g_discard.x - pad)
            ugfx_draw_string_elided(s, pad * 2 + hw, ty, g_discard.x - pad * 2 - (pad * 2 + hw), g_foot_text,
                                    utheme_current()->text_muted, UTHEME_CHROME);
    }
    g_sb.panes[0].text = g_note[0] ? g_note : g_st_count;
    // What the test drives by: each visible card's component and state.
    uapp_log_layout(a, "software");
    for (int n = 0; n < g_ncards; n++)
        uapp_logf_layout("software: card %d %s %s\n", n, g_set.c[g_card_comp[n]].id,
                         g_pend[g_card_comp[n]] == P_REMOVE ? "remove" : g_pend[g_card_comp[n]] == P_ADD ? "add"
                         : installed(g_card_comp[n]) ? "installed" : "available");
    // ...and each sidebar row, by its key (nothing in it collapses, so a
    // node's index is its row).
    static const char *const FIXED_KEY[] = { "all", "installed", "available" };
    for (int r = 0; r < g_cats.count; r++) {
        int id = g_cats.nodes[r].id;
        const char *key = id >= CAT_FIRST && id - CAT_FIRST < g_ncat_keys ? g_cat_keys[id - CAT_FIRST]
                        : id >= CAT_ALL && id <= CAT_AVAILABLE ? FIXED_KEY[id - CAT_ALL] : 0;
        if (key)
            uapp_logf_layout("software: layout cats.%s %d %d %d %d\n", key, g_cats.x,
                             g_cats.y + (r - g_cats.top) * uui_tree_row_h(&g_cats), g_cats.w,
                             uui_tree_row_h(&g_cats));
    }
    uapp_logf_layout("software: state view %s cat %d cards %d queue %d gpl %d counts %s %s %s\n",
                     g_view == UUI_APPCARD_ROWS ? "list" : "cards", g_cat, g_ncards, g_nqueue, g_show_gpl,
                     g_cat_notes[0], g_cat_notes[1], g_cat_notes[2]);
}

// ---- input ----------------------------------------------------------------------------

static void answer(struct uapp *a, int code) {
    (void)a;
    if (code == ASK_APPLY) { start_apply(a); return; }
    if (g_job.running && g_job.kind == JOB_APPLY) return;   // the steps' dialog stays while they run
    uui_dialog_close(&g_ask);
    if (code == ASK_CANCEL) job_free();
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    g_app = a;
    int code;
    switch (id) {
    case ID_MENU: if ((code = uui_menubar_take_code(&g_menu)) > 0) command(a, code); break;
    case ID_GPL:  set_gpl(g_gpl.on); break;
    case ID_CATS: {
        int c = uui_tree_selected_id(&g_cats);
        if (c > 0 && c != g_cat) { g_cat = c; g_view_w.top = 0; g_view_w.selected = -1; rebuild(); }
        break;
    }
    case ID_CARDS: {
        int i = uui_appcards_take_button(&g_view_w);
        if (i >= 0 && i < g_ncards && !g_job.running) { toggle(g_card_comp[i]); rebuild(); }
        if (uui_appcards_take_activate(&g_view_w) >= 0 && g_view == UUI_APPCARD_CARDS) set_view(UUI_APPCARD_ROWS);
        build_props();
        break;
    }
    case ID_PROPS:
        if (uui_props_take_action(&g_props) >= 0) {
            static char text[2048];
            int n = uui_props_text(&g_props, text, sizeof text);
            say(n > 0 && uclip_set_text(text, n) ? "Copied the details" : "The details did not fit on the clipboard");
        }
        break;
    case ID_DIALOG:
        if ((code = uui_dialog_take_code(&g_ask)) >= 0) answer(a, code);
        break;
    default: break;
    }
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    g_app = a;
    switch (code) {
    case ID_CHECK:      do_check(); break;
    case ID_SHOW_GPL:   set_gpl(1); break;
    case ID_DISCARD:    command(a, CMD_DISCARD); break;
    case ID_APPLY:      confirm_apply(a); break;
    case ID_DETAIL_BTN: { int i = sel_comp(); if (i >= 0 && !g_job.running) { toggle(i); rebuild(); } break; }
    default: break;
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    int code;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code > 0) command(a, code);
        uapp_redraw(a);
        return;
    }
    if (key == KEY_F5) command(a, CMD_CHECK);
    else if ((mods & KEY_MOD_CTRL) && key == '1') command(a, CMD_CARDS);
    else if ((mods & KEY_MOD_CTRL) && key == '2') command(a, CMD_LIST);
    else if (key == 0x13) command(a, CMD_APPLY);    // Ctrl+S
    // A key the search took: the cards follow it.
    if (g_focus.current == 0) { build_cards(); build_props(); }
    uapp_redraw(a);
}

static int on_user(struct uapp *a, int a0, int a1) {
    switch (a0) {
    case EV_CHECKED:
        g_job.running = 0;
        snprintf(g_st_server, sizeof g_st_server, "From %s", g_job.from[0] ? g_job.from : "no server");
        if (g_check_rc == 0) snprintf(g_note, sizeof g_note, "%s", "The server has this build: components can be added");
        else if (g_job.build_gone) snprintf(g_note, sizeof g_note, "%s", "The server no longer has this build: update first, then add components");
        else snprintf(g_note, sizeof g_note, "Could not check the server: %s", g_check_why);
        ulogf("software: checked %d %s\n", g_check_rc, g_note);
        break;
    case EV_STEP:
        if (a1 >= 0 && a1 < g_job.count) { g_job.cur = a1; g_job.rows[a1].state = UUI_STEP_RUNNING; }
        ulogf("software: step %d %s %s\n", a1 + 1, g_job.rows[a1].verb, g_set.c[g_job.comp[a1]].id);
        break;
    case EV_STEP_DONE:
        if (a1 < 0 || a1 >= g_job.count) break;
        g_job.rows[a1].state = g_job.rc[a1] ? UUI_STEP_FAILED : UUI_STEP_DONE;
        g_job.rows[a1].permille = -1;
        if (g_job.rc[a1]) {
            g_job.failed = a1;
            for (int k = a1 + 1; k < g_job.count; k++) g_job.rows[k].state = UUI_STEP_NOT_RUN;
        }
        ulogf("software: step %d -> %d %s\n", a1 + 1, g_job.rc[a1], g_job.why[a1]);
        break;
    case EV_DONE: apply_done(a); break;
    case EV_PROGRESS: break;
    default: return 0;
    }
    return 1;
}

// Redraws while a job's progress moves; nothing is read from disk here.
static int on_tick(struct uapp *a) {
    (void)a;
    return g_job.running;
}

// The GPL switch flipped elsewhere (System Settings, `config`): the
// settings signal, not a poll. A change during a job is read when it ends.
static void on_setting(struct uapp *a) {
    if (!g_job.running && ucomponent_show_gpl() != g_show_gpl) {
        rebuild();
        uapp_redraw(a);
    }
}

static void default_size(int *w, int *h) {
    *w = ugfx_char_w() * 84;
    *h = ugfx_char_h() * 34;
}

int main(void) {
    if (!ugfx_font_init()) return 2;
    struct uui_builder *menus = uui_builder_load("software", MENU_NAMES,
                                                (int)(sizeof MENU_NAMES / sizeof MENU_NAMES[0]));
    int bar_count;
    const struct uui_menu_item *bar_rows = uui_builder_menu(menus, "bar", &bar_count);
    if (uui_builder_error(menus)) return uui_builder_run_error(menus);
    uui_menubar_init(&g_menu, bar_rows, bar_count);
    g_menu.item_flags = item_flags;
    g_app = 0;
    uui_textbox_init(&g_search, "");
    g_search.placeholder = "Search apps";
    uui_switch_init(&g_gpl, 1);
    g_gpl.on_text = g_gpl.off_text = "Show GPL-licensed apps";
    uui_button_init(&g_check, 0, 0, 0, 0, "Check server", UUI_COLOR_UNSET, UUI_ROLE(text), ID_CHECK);
    g_check.outlined = 1;
    uui_button_init(&g_show, 0, 0, 0, 0, "Show them", UUI_COLOR_UNSET, UUI_ROLE(text), ID_SHOW_GPL);
    g_show.outlined = 1;
    uui_button_init(&g_discard, 0, 0, 0, 0, "Discard", UUI_COLOR_UNSET, UUI_ROLE(text), ID_DISCARD);
    g_discard.outlined = 1;
    uui_button_init(&g_apply, 0, 0, 0, 0, "Apply changes", UUI_ROLE(accent), UUI_ROLE(accent_text), ID_APPLY);
    uui_button_init(&g_detail_btn, 0, 0, 0, 0, "Remove", UUI_COLOR_UNSET, UUI_ROLE(text), ID_DETAIL_BTN);
    g_detail_btn.outlined = 1;
    uui_tree_init(&g_cats, 0, 0, 0, 0, 0, 0);
    g_cats.sel_style = UUI_SEL_ROUNDED;
    g_cats.bg = UUI_ROLE(panel_bg);
    uui_appcards_init(&g_view_w, UUI_APPCARD_CARDS);
    uui_props_init(&g_props);
    uui_dialog_init(&g_ask);
    uui_steplist_init(&g_run, UUI_STEPLIST_STATE);
    uui_statusbar_init(&g_sb);
    g_sb.count = 3;
    g_sb.panes[0].text = g_st_count;  g_sb.panes[0].chars = 0;
    g_sb.panes[1].text = g_st_server; g_sb.panes[1].chars = 40;
    g_sb.panes[2].text = g_st_build;  g_sb.panes[2].chars = 18;
    snprintf(g_st_server, sizeof g_st_server, "%s", "Check server: not asked yet");
    g_ring[0] = (struct uui_focusable){ &g_search, &uui_textbox_focus_ops };
    g_ring[1] = (struct uui_focusable){ &g_cats, &uui_tree_ops };
    g_ring[2] = (struct uui_focusable){ &g_view_w, &uui_appcards_ops };
    uui_focus_init(&g_focus, g_ring, 3);
    load();
    rebuild();

    struct uapp_desc desc = {
        .title        = "Software",
        .app_id       = "software",
        .on_size      = default_size,
        .flags        = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE,
        .min_w        = 640,
        .min_h        = 420,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .focus        = &g_focus,
        .tick_ms      = 250,
        .on_tick      = on_tick,
        .on_setting   = on_setting,
        .on_user      = on_user,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_key       = on_key,
    };
    return uapp_run(&desc);
}
