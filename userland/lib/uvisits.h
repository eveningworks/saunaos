#ifndef ULIB_UVISITS_H
#define ULIB_UVISITS_H

#include <stdint.h>

// uvisits -- the pages a person browsed to (a browser's History; the
// shell's command history is lib/uhistory.h): one entry per address, with
// when it was last visited, how often, and its title. Kept in one file, a
// line per page ("when<tab>visits<tab>URL<tab>title"), the Web Browser's
// History list (docs/browser-design.md, stage 5).
//
// Bounded the way NetSurf's URL database is: entries not visited in
// UVISITS_KEEP_DAYS go at load, and past UVISITS_MAX the oldest go.

#define UVISITS_MAX       5000
#define UVISITS_KEEP_DAYS 90

struct uvisit {
    int64_t when;          // the last visit, a UTC epoch
    int visits;
    char *url, *title;
};

struct uvisits {
    struct uvisit *e;
    int n, cap;
    int dirty;             // changed since the last save
};

int  uvisits_load(struct uvisits *h, const char *path, int64_t now);
int  uvisits_save(struct uvisits *h, const char *path);
void uvisits_free(struct uvisits *h);

// A visit to `url` now: its entry made or brought up to date.
void uvisits_visit(struct uvisits *h, const char *url, const char *title, int64_t now);
// Forgets `url`. 1 if it was there.
int  uvisits_forget(struct uvisits *h, const char *url);

// NetSurf's URL file (its urldb's, format 106-107): every address it
// says was visited. Returns how many were taken.
int  uvisits_import_netsurf(struct uvisits *h, const char *path, int64_t now);

#endif
