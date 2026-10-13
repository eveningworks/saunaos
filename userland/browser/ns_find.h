#ifndef BROWSER_NS_FIND_H
#define BROWSER_NS_FIND_H

// Find in page: NetSurf's own search (desktop/search.h), which finds,
// highlights and scrolls to a match, plus the "N of M" it keeps to
// itself -- its match list is private to content/textsearch.c.
struct gui_window;

// `dir` 0 searches `query` afresh from the top of the page, +1 and -1
// step to the next and previous match, WRAPPING at either end as
// Chrome's and Firefox's do (NetSurf's own stops). An empty query
// clears the highlights. Returns the match count.
int ns_find(struct gui_window *gw, const char *query, int dir);
void ns_find_clear(struct gui_window *gw);

// The last search's count, and the match shown (1-based; 0 for none).
int ns_find_matches(void);
int ns_find_current(void);

#endif
