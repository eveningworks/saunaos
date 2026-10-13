// Find in page. See ns_find.h.
#include <string.h>

#include "utils/errors.h"
#include "webcontent/ns_find.h"
#include "webcontent/wc.h"

#include "desktop/search.h"

struct textsearch_context;
struct box;

static int g_matches, g_current, g_added;
static char g_query[256];

// THE COUNT, WITHOUT TOUCHING THE VENDORED SOURCE. NetSurf adds every
// match of a NEW search through this one function, called from each
// content handler's textsearch_find (html.c, textplain.c); the browser
// links with `--wrap` on it (the Makefile), so each call passes through
// here first. The trap: ld wraps only references from OTHER objects --
// were upstream to move the calls into content/textsearch.c itself,
// which defines the function, the count would silently stay 0.
nserror __real_content_textsearch_add_match(struct textsearch_context *c, unsigned start,
                                            unsigned end, struct box *sb, struct box *eb);
nserror __wrap_content_textsearch_add_match(struct textsearch_context *c, unsigned start,
                                            unsigned end, struct box *sb, struct box *eb) {
    g_added++;
    return __real_content_textsearch_add_match(c, start, end, sb, eb);
}

// Every match highlighted, as Chrome and Firefox do; the current one
// is NetSurf's selection.
static void step(struct gui_window *gw, int forwards) {
    browser_window_search(gw->bw, gw,
                          (forwards ? SEARCH_FLAG_FORWARDS : SEARCH_FLAG_BACKWARDS) | SEARCH_FLAG_SHOWALL,
                          g_query);
}

// A search from the top of the page: the same query twice would only
// CONTINUE the last one (content_textsearch()), so clear it first.
static void restart(struct gui_window *gw) {
    browser_window_search_clear(gw->bw);
    g_added = 0;
    step(gw, 1);
    g_matches = g_added;
    g_current = g_matches ? 1 : 0;
}

int ns_find(struct gui_window *gw, const char *query, int dir) {
    if (!gw || !gw->bw) return 0;
    if (!query || !query[0]) {
        ns_find_clear(gw);
        return 0;
    }
    if (dir == 0 || strcmp(query, g_query) != 0) {
        strlcpy(g_query, query, sizeof g_query);
        restart(gw);
    } else if (!g_matches) {
        return 0;
    } else if (dir > 0) {
        if (g_current < g_matches) {
            step(gw, 1);
            g_current++;
        } else {
            restart(gw);   // past the last: the first
        }
    } else if (g_current > 1) {
        step(gw, 0);
        g_current--;
    } else {
        // Before the first: the last. NetSurf has no "go to the end", so
        // walk there from the top.
        restart(gw);
        for (int i = 1; i < g_matches; i++) step(gw, 1);
        g_current = g_matches;
    }
    return g_matches;
}

void ns_find_clear(struct gui_window *gw) {
    if (gw && gw->bw) browser_window_search_clear(gw->bw);
    g_matches = g_current = 0;
    g_query[0] = '\0';
}

int ns_find_matches(void) { return g_matches; }
int ns_find_current(void) { return g_current; }
