#ifndef ULIB_UBOOKMARKS_H
#define ULIB_UBOOKMARKS_H

// ubookmarks -- a list of bookmarked pages, kept in one file: a line per
// bookmark, "URL<tab>title", in the order they were added, readable with
// `less`. The Web Browser's (docs/browser-design.md, stage 5), with
// nothing in it that is the browser's.

struct ubookmark {
    char *url, *title;
};

struct ubookmarks {
    struct ubookmark *b;
    int n, cap;
};

// Reads `path` into an empty list; a missing file is an empty list.
// 0, or -1 when the file is not one of these.
int  ubookmarks_load(struct ubookmarks *bm, const char *path);
int  ubookmarks_save(const struct ubookmarks *bm, const char *path);
void ubookmarks_free(struct ubookmarks *bm);

// The bookmark for `url`, or -1.
int  ubookmarks_find(const struct ubookmarks *bm, const char *url);
// Appends one (a URL already there is left as it is). 0, or -1.
int  ubookmarks_add(struct ubookmarks *bm, const char *url, const char *title);
void ubookmarks_remove(struct ubookmarks *bm, int i);

// NetSurf's hotlist (the Netscape bookmark HTML it writes): every link
// in it, in order. Returns how many were added.
int  ubookmarks_import_netsurf(struct ubookmarks *bm, const char *path);

#endif
