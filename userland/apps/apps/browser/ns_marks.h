#ifndef BROWSER_NS_MARKS_H
#define BROWSER_NS_MARKS_H

// Bookmarks and history for the sidebar -- H1 on the stage 4 canvas.
// The app's own since stage 5 (lib/ubookmarks.h, lib/uvisits.h): the
// engine runs in another process and keeps neither. NetSurf's files from
// before are read once. Shown as rows the toolkit's uui_entrylist draws.
#include "ui/uui_entrylist.h"

enum { NS_MARKS_BOOKMARKS, NS_MARKS_HISTORY };
#define NS_MARKS_MAX 256

// Loads both lists from `dir` (/var/lib/browser), taking NetSurf's old
// files the first time. 1, or 0 when the bookmarks could not be read.
int ns_marks_init(const char *dir);
// A page the person reached: History's entry for it made or moved up.
void ns_marks_visited(const char *url, const char *title);
// Saves what changed.
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
