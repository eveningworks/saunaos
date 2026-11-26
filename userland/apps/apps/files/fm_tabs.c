// Tabs: several folders in one File Manager window, shown IN THE TITLE
// BAR -- Windows 11 Explorer's place for them, drawn by the compositor
// (win_proto.h's WIN_REQ_TABS; it reports clicks, this decides).
//
// A tab is the window's whole state as far as a person can tell: both
// panes' folders, their Back/Forward histories and which pane is
// active. Leaving a tab saves that; choosing one puts it back. The
// window's widgets are shared -- one pair of panes, refilled -- so a tab
// costs its paths and histories, not a second set of listings.
//
// A search's results are not a place (files.c saves a pane's folder the
// same way): a tab left on results comes back to the folder searched.
#include "fm_internal.h"
#include <string.h>
#include <stdio.h>
#include "kpath.h"
#include "base/uutf8.h"
#include "base/ulog.h"
#include "base/unavhist.h"
#include "base/udirs.h"

// A tab OWNS its folders and histories: copies, freed by tab_free(). A
// slot past g_ntabs is all zero.
struct tab {
    char *dir[2];
    int active;
    struct unavhist hist[2];
    char label[WIN_TEXT_MAX];
};

static struct tab g_tabs[WIN_TABS_MAX];
static int g_ntabs, g_cur;
static char g_sent[WIN_TABS_MAX][WIN_TEXT_MAX];   // what the compositor last got
static int g_sent_n = -1, g_sent_cur = -1;

static const char *real_dir(int pane) {
    const char *d = uui_fileview_dir(&g_pane[pane]);
    const char *sc = search_scope(d);
    return sc ? sc : d;
}

// Cut to the compositor's text bound on a character boundary, so a long
// non-Latin folder name does not end in half a character.
static void label_of(const char *dir, char *out, int cap) {
    const char *l = !strcmp(dir, FM_BIN) ? "Recycle Bin"
                  : !strcmp(dir, FM_RECENT) ? "Recent"
                  : !strcmp(dir, FM_QUICK) ? "Quick access"
                  : search_scope(dir) ? "Search results"
                  : (dir[0] == '/' && !dir[1]) ? "System"
                  : k_path_basename(dir);
    uutf8_lcpy(out, l ? l : "", cap);
}

// `*field` becomes a copy of `s`; out of memory it keeps what it had.
static void keep_dir(char **field, const char *s) {
    if (!upath_set(field, s)) ulogf("files: out of memory keeping a tab's folder %s\n", s);
}

static void tab_free(struct tab *t) {
    for (int p = 0; p < 2; p++) {
        free(t->dir[p]);
        unavhist_init(&t->hist[p]);
    }
    memset(t, 0, sizeof *t);
}

static void save(int i) {
    struct tab *t = &g_tabs[i];
    for (int p = 0; p < 2; p++) {
        keep_dir(&t->dir[p], real_dir(p));
        fm_history_save(p, &t->hist[p]);
    }
    t->active = g_active;
}

static void load(int i) {
    struct tab *t = &g_tabs[i];
    for (int p = 0; p < 2; p++) {
        fm_history_load(p, &t->hist[p]);
        fm_history_replay(p, t->dir[p] ? t->dir[p] : udir_home());
    }
    g_active = t->active;
}

// Tell the compositor -- only when what it shows would change, since
// every label is a request of its own.
void tabs_sync(struct uapp *a) {
    if (g_ntabs == 0) return;
    label_of(uui_fileview_dir(active()), g_tabs[g_cur].label, sizeof g_tabs[g_cur].label);
    int same = g_sent_n == g_ntabs && g_sent_cur == g_cur;
    for (int i = 0; same && i < g_ntabs; i++) same = !strcmp(g_sent[i], g_tabs[i].label);
    if (same) return;
    const char *labels[WIN_TABS_MAX];
    for (int i = 0; i < g_ntabs; i++) {
        labels[i] = g_tabs[i].label;
        strlcpy(g_sent[i], g_tabs[i].label, sizeof g_sent[i]);
    }
    g_sent_n = g_ntabs;
    g_sent_cur = g_cur;
    uapp_set_tabs(a, labels, g_ntabs, g_cur);
}

void tabs_init(struct uapp *a) {
    g_ntabs = 1;
    g_cur = 0;
    save(0);
    tabs_sync(a);
}

int tabs_count(void) { return g_ntabs; }

void tab_select(struct uapp *a, int i) {
    if (i < 0 || i >= g_ntabs || i == g_cur) return;
    save(g_cur);
    g_cur = i;
    load(i);
    ulogf("files: tab %d of %d\n", g_cur, g_ntabs);
    refresh_status();
    tabs_sync(a);
}

// A new tab on `dir` -- the active pane goes there, the other keeps the
// current tab's, as a browser's new tab keeps the window's other state.
void tab_new(struct uapp *a, const char *dir) {
    if (g_ntabs == 0) return;
    if (g_ntabs >= WIN_TABS_MAX) { set_note("no room for another tab"); return; }
    char *want = strdup(dir);   // `dir` may be a pane's, which save() and load() move
    if (!want) { set_note("not enough memory for another tab"); return; }
    save(g_cur);
    const struct tab *cur = &g_tabs[g_cur];
    struct tab *t = &g_tabs[g_ntabs];   // zero: past g_ntabs
    t->active = cur->active;
    // A fresh history for the pane that moved; the other keeps its own.
    for (int p = 0; p < 2; p++) {
        if (p == t->active) { t->dir[p] = want; continue; }
        keep_dir(&t->dir[p], cur->dir[p] ? cur->dir[p] : udir_home());
        unavhist_copy(&t->hist[p], &cur->hist[p]);
    }
    unavhist_init(&t->hist[t->active]);
    g_cur = g_ntabs++;
    load(g_cur);
    fm_history_record(g_active, uui_fileview_dir(active()));
    ulogf("files: tab %d of %d\n", g_cur, g_ntabs);
    refresh_status();
    tabs_sync(a);
}

// Closing the last tab closes the window, Explorer's and every browser's
// rule.
void tab_close(struct uapp *a, int i) {
    if (i < 0 || i >= g_ntabs) return;
    if (g_ntabs == 1) { fm_request_quit(a); return; }
    if (i != g_cur) save(g_cur);
    tab_free(&g_tabs[i]);
    memmove(&g_tabs[i], &g_tabs[i + 1], sizeof g_tabs[0] * (size_t)(g_ntabs - i - 1));
    g_ntabs--;
    memset(&g_tabs[g_ntabs], 0, sizeof g_tabs[0]);   // its strings moved down a slot
    int was = g_cur;
    if (i < g_cur) g_cur--;
    else if (i == g_cur) {
        if (g_cur >= g_ntabs) g_cur = g_ntabs - 1;
    }
    if (i == was) load(g_cur);
    ulogf("files: tab %d of %d\n", g_cur, g_ntabs);
    refresh_status();
    tabs_sync(a);
}

void tab_step(struct uapp *a, int delta) {
    if (g_ntabs < 2) return;
    tab_select(a, (g_cur + delta + g_ntabs) % g_ntabs);
}

int tab_current(void) { return g_cur; }
