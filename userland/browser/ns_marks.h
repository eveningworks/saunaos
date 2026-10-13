#ifndef BROWSER_NS_MARKS_H
#define BROWSER_NS_MARKS_H

// Bookmarks and history for the sidebar -- H1 on the stage 4 canvas.
// Bookmarks are NetSurf's hotlist, kept in NetSurf's own HTML file;
// history is its URL database, the pages browsed to and when. Read
// here into rows the toolkit's uui_entrylist draws, with toy-os's look
// rather than NetSurf's tree windows.
#include "ui/uui_entrylist.h"

enum { NS_MARKS_BOOKMARKS, NS_MARKS_HISTORY };
#define NS_MARKS_MAX 256

// Loads the bookmarks from `path` (made empty when missing -- not
// NetSurf's default links) and saves there as they change.
int ns_marks_init(const char *path);
void ns_marks_fini(void);

int ns_bookmarked(const char *url);
// Adds the page, or removes it when it is bookmarked. Returns whether
// it is bookmarked after.
int ns_bookmark_toggle(const char *url, const char *title);

// The rows for one list, filtered by `filter` (in a title or an
// address, any case): bookmarks as kept, history newest first under a
// heading per day. Returns the count; ns_marks_url(i) is row i's address.
int ns_marks_rows(int which, const char *filter, struct uui_entry *rows, int cap);
const char *ns_marks_url(int row);
// Takes row i out of its list: a bookmark deleted, a page forgotten.
void ns_marks_remove(int which, int row);

#endif
