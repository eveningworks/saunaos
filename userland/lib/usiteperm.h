#ifndef ULIB_USITEPERM_H
#define ULIB_USITEPERM_H

// usiteperm -- PER-SITE PERMISSIONS: for a host and a kind ("javascript"),
// Allow or Block, or nothing kept and the global setting decides --
// Chrome's content-settings exceptions, Firefox's permissions.sqlite. The
// Web Browser keeps its sites' JavaScript choices here; images, pop-ups
// and autoplay would be further kinds of the same table.
//
// One file, one exception per line: `host<TAB>kind<TAB>allow|block`. A
// host is matched EXACTLY, as typed into the address ("www.example.org"
// is not "example.org"). Lines this version cannot read are kept as they
// are and written back, so an older reader never loses a newer kind.

#define USITEPERM_HOST 128
#define USITEPERM_KIND 24

enum { USITEPERM_DEFAULT, USITEPERM_ALLOW, USITEPERM_BLOCK };

struct usiteperm_entry {
    char host[USITEPERM_HOST];
    char kind[USITEPERM_KIND];
    int value;                  // USITEPERM_ALLOW or _BLOCK; 0 = a line kept unread
    char *raw;                  // that line, when value is 0
};

struct usiteperm {
    char path[256];
    struct usiteperm_entry *e;
    int n, cap;
};

// Reads `path` (a missing file is an empty table). 0, or -1 out of memory.
int usiteperm_load(struct usiteperm *t, const char *path);
void usiteperm_free(struct usiteperm *t);
// The value for (host, kind): USITEPERM_DEFAULT when none is kept.
int usiteperm_get(const struct usiteperm *t, const char *host, const char *kind);
// Keeps `value` for (host, kind); USITEPERM_DEFAULT forgets it. Writes the
// file at once (atomically, a temporary then a rename). 0, or -1.
int usiteperm_set(struct usiteperm *t, const char *host, const char *kind, int value);
// One kind's exceptions as "host\tallow|block\n" lines into `out`
// (NUL-terminated), what lib/webview_proto.h's WV_PERMS carries. The
// length written, or -1 when it does not fit -- and then nothing.
int usiteperm_list(const struct usiteperm *t, const char *kind, char *out, int cap);

#endif
