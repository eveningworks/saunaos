#ifndef ULIB_UDECL_H
#define ULIB_UDECL_H

// udecl -- a small DECLARATIVE TREE file: one node per line, a block of
// children in braces. The language under ui/uui_builder.h's .ui files
// (docs/ui-files-design.md), and nothing in it names a widget:
//
//     // a comment
//     window #list title="Crash Reports" size=120x30 resizable {
//         row margin=1 gap=1 {
//             label #panel fill-h width=40
//         }
//     }
//
// A node is TYPE, then in any order at most one `#name`, at most one
// "quoted text", and `key=value` or bare `flag` attributes; a `{` ending
// the line opens its children and a `}` alone on a line closes them.
// Values are bare words or quoted strings (\" \\ \n \t).
//
// A READER REJECTS RATHER THAN GUESSES: every getter marks what it read,
// and udecl_unused() names the first attribute nobody asked for -- a
// misspelt `fil-w` is an error at the line it is on, never a control
// silently left unstretched. Errors are "<file>:<line>: <what>".
//
// The tree is allocated to fit and every string points into one owned
// copy of the source; udecl_free() releases both.

#include <stddef.h>

struct udecl_attr {
    const char *key;
    const char *value;   // NULL for a bare flag
    int used;
};

struct udecl_node {
    const char *type;    // "" for the file's top
    const char *name;    // after the '#'; NULL when unnamed
    const char *text;    // the quoted text; NULL when none
    int line;
    int text_used;
    struct udecl_attr *attr;
    int nattr, attr_cap;
    struct udecl_node *kid;
    int nkid, kid_cap;
};

struct udecl {
    struct udecl_node top;   // the file's top-level nodes are its kids
    char *buf;               // the source, tokens cut in place
    char what[64];           // the file's name in messages
    char error[224];         // "" while all is well
};

// 0, or -1 with d->error set. `what` names the source in messages. The
// tree is usable only after a 0; udecl_free() is safe either way.
int udecl_parse(struct udecl *d, const char *what, const char *src, size_t len);
int udecl_load(struct udecl *d, const char *path);   // `what` = the base name
void udecl_free(struct udecl *d);

// Records the FIRST error only (the one a person fixes first) and
// returns -1, so a caller writes `return udecl_fail(...)`.
int udecl_fail(struct udecl *d, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

// The getters mark the attribute used, and a FLAG AND A VALUE ARE NOT
// INTERCHANGEABLE: udecl_get() on a bare `title` and udecl_flag() on
// `resizable=no` are errors (d->error, the getter answering NULL / 0),
// never "" or "set". NULL when absent.
const char *udecl_get(struct udecl *d, struct udecl_node *n, const char *key);
int udecl_flag(struct udecl *d, struct udecl_node *n, const char *key);
// 0 = absent (*out untouched), 1 = read, -1 = malformed or out of lo..hi
// (d->error says which line and why).
int udecl_int(struct udecl *d, struct udecl_node *n, const char *key, int lo, int hi, int *out);
int udecl_pair(struct udecl *d, struct udecl_node *n, const char *key, int lo, int hi, int *w, int *h); // "WxH"
// An index into `words`.
int udecl_word(struct udecl *d, struct udecl_node *n, const char *key,
               const char *const *words, int count, int *out);
// The text, marked used.
const char *udecl_text(struct udecl_node *n);

// -1 naming the first attribute (or text) on `n` nothing read; else 0.
int udecl_unused(struct udecl *d, struct udecl_node *n);

#endif
