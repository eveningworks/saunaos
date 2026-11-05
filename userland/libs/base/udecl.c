// See base/udecl.h.
#include "base/udecl.h"
#include "base/ufile.h"
#include "base/ugrow.h"
#include "knum.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Deeper than any window wants, and the parser recurses once per level:
// the cap keeps a runaway file from walking off the stack, and says so.
#define MAX_DEPTH 64

int udecl_fail(struct udecl *d, int line, const char *fmt, ...) {
    if (d->error[0]) return -1;
    // Line 0 is the file as a whole (unreadable, or a name the app asked for).
    int n = line > 0 ? snprintf(d->error, sizeof d->error, "%s:%d: ", d->what, line)
                     : snprintf(d->error, sizeof d->error, "%s: ", d->what);
    if (n < 0 || n >= (int)sizeof d->error) n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->error + n, sizeof d->error - (size_t)n, fmt, ap);
    va_end(ap);
    return -1;
}

static void node_free(struct udecl_node *n) {
    for (int i = 0; i < n->nkid; i++) node_free(&n->kid[i]);
    free(n->kid);
    free(n->attr);
}

void udecl_free(struct udecl *d) {
    node_free(&d->top);
    free(d->buf);
    memset(&d->top, 0, sizeof d->top);
    d->buf = 0;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// A quoted string at *p (on the opening quote), unescaped IN PLACE: the
// output is never longer than the input. *p ends past the closing quote.
static const char *string_token(struct udecl *d, int line, char **p) {
    char *in = *p + 1, *out = in, *start = in;
    for (;;) {
        char c = *in++;
        if (c == '\0') { udecl_fail(d, line, "a string is not closed on its line"); return 0; }
        if (c == '"') break;
        if (c == '\\') {
            char e = *in++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == '"' || e == '\\') c = e;
            else { udecl_fail(d, line, "unknown escape \\%c in a string", e ? e : '0'); return 0; }
        }
        *out++ = c;
    }
    // The closing quote's own byte may be the terminator only once the
    // scan is past it: `out` trails `in`, so this never cuts ahead.
    *out = '\0';
    *p = in;
    return start;
}

// A bare word: up to whitespace, '=', '"' or a brace. Not terminated
// here -- the caller looks at the byte that stopped it first.
static char *bare_end(char *p) {
    while (*p && !is_space(*p) && *p != '=' && *p != '"' && *p != '{' && *p != '}') p++;
    return p;
}

static struct udecl_attr *add_attr(struct udecl *d, struct udecl_node *n, int line) {
    if (!UGROW(n->attr, n->attr_cap, n->nattr + 1)) { udecl_fail(d, line, "out of memory"); return 0; }
    struct udecl_attr *a = &n->attr[n->nattr++];
    memset(a, 0, sizeof *a);
    return a;
}

// One node's line, already cut at its newline. *opens is set when the
// line ends in '{'.
static int parse_line(struct udecl *d, struct udecl_node *n, char *p, int line, int *opens) {
    *opens = 0;
    // The type.
    char *e = bare_end(p);
    if (e == p || *p == '#' || *p == '{' || *p == '}' || *e == '=' || *e == '"')
        return udecl_fail(d, line, "a line starts with the node's type, a word");
    n->type = p;
    n->line = line;
    char stop = *e;
    *e = '\0';
    p = stop ? e + 1 : e;
    if (stop && !is_space(stop)) return udecl_fail(d, line, "a space must follow '%s'", n->type);

    for (;;) {
        while (is_space(*p)) p++;
        if (!*p) return 0;
        if (*p == '/' && p[1] == '/') return 0;
        if (*p == '{') {
            char *q = p + 1;
            while (is_space(*q)) q++;
            if (*q && !(q[0] == '/' && q[1] == '/'))
                return udecl_fail(d, line, "'{' opens a block only at the end of a line");
            *opens = 1;
            return 0;
        }
        if (*p == '}') return udecl_fail(d, line, "'}' closes a block on a line of its own");
        if (*p == '"') {
            if (n->text) return udecl_fail(d, line, "%s has two texts", n->type);
            const char *s = string_token(d, line, &p);
            if (!s) return -1;
            n->text = s;
            if (*p && !is_space(*p)) return udecl_fail(d, line, "a space must follow a string");
            continue;
        }
        if (*p == '#') {
            if (n->name) return udecl_fail(d, line, "%s has two names", n->type);
            char *s = p + 1;
            e = bare_end(s);
            if (e == s || (*e && !is_space(*e))) return udecl_fail(d, line, "'#' is followed by a name, a word");
            stop = *e;
            *e = '\0';
            n->name = s;
            p = stop ? e + 1 : e;
            continue;
        }
        // key or key=value
        char *k = p;
        e = bare_end(k);
        if (*e == '"') return udecl_fail(d, line, "a string follows '=', as key=\"text\"");
        if (*e == '{' || *e == '}') return udecl_fail(d, line, "a space must come before '%c'", *e);
        struct udecl_attr *a = add_attr(d, n, line);
        if (!a) return -1;
        a->key = k;
        if (*e != '=') {
            stop = *e;
            *e = '\0';
            a->value = 0;   // a bare flag
            p = stop ? e + 1 : e;
            continue;
        }
        *e = '\0';
        if (!*k) return udecl_fail(d, line, "'=' needs a key before it");
        p = e + 1;
        if (*p == '"') {
            const char *s = string_token(d, line, &p);
            if (!s) return -1;
            a->value = s;
            if (*p && !is_space(*p)) return udecl_fail(d, line, "a space must follow a string");
            continue;
        }
        e = bare_end(p);
        if (e == p) return udecl_fail(d, line, "%s= has no value", k);
        if (*e && !is_space(*e)) return udecl_fail(d, line, "%s='s value runs into '%c'", k, *e);
        stop = *e;
        *e = '\0';
        a->value = p;
        p = stop ? e + 1 : e;
    }
}

struct cursor {
    char *p;
    int line;
};

// The next non-blank line, cut at its newline; NULL at the end.
static char *next_line(struct cursor *c) {
    while (*c->p) {
        char *start = c->p, *nl = strchr(start, '\n');
        if (nl) { *nl = '\0'; c->p = nl + 1; } else c->p = start + strlen(start);
        c->line++;
        char *s = start;
        while (is_space(*s)) s++;
        if (*s && !(s[0] == '/' && s[1] == '/')) return s;
    }
    return 0;
}

static int is_close(const char *s) {
    if (*s != '}') return 0;
    s++;
    while (is_space(*s)) s++;
    return !*s || (s[0] == '/' && s[1] == '/');
}

// Children of `parent` until its '}' (or the end, for the top).
static int parse_block(struct udecl *d, struct cursor *c, struct udecl_node *parent, int depth, int open_line) {
    if (depth > MAX_DEPTH) return udecl_fail(d, open_line, "blocks nest deeper than %d", MAX_DEPTH);
    for (;;) {
        char *s = next_line(c);
        if (!s) {
            if (depth == 0) return 0;
            return udecl_fail(d, open_line, "the '{' of %s is never closed", parent->type);
        }
        if (*s == '}') {
            if (!is_close(s)) return udecl_fail(d, c->line, "'}' closes a block on a line of its own");
            if (depth == 0) return udecl_fail(d, c->line, "'}' with no block open");
            return 0;
        }
        if (!UGROW(parent->kid, parent->kid_cap, parent->nkid + 1)) return udecl_fail(d, c->line, "out of memory");
        struct udecl_node *n = &parent->kid[parent->nkid++];
        memset(n, 0, sizeof *n);
        int opens;
        if (parse_line(d, n, s, c->line, &opens) < 0) return -1;
        for (int i = 1; i < n->nattr; i++)
            for (int j = 0; j < i; j++)
                if (!strcmp(n->attr[i].key, n->attr[j].key))
                    return udecl_fail(d, c->line, "%s is given twice", n->attr[i].key);
        // `n` stays valid below: only n's own arrays grow while its block
        // is read, never its parent's.
        if (opens && parse_block(d, c, n, depth + 1, n->line) < 0) return -1;
    }
}

int udecl_parse(struct udecl *d, const char *what, const char *src, size_t len) {
    memset(d, 0, sizeof *d);
    snprintf(d->what, sizeof d->what, "%s", what ? what : "?");
    d->top.type = "";
    d->buf = malloc(len + 1);
    if (!d->buf) return udecl_fail(d, 0, "out of memory");
    memcpy(d->buf, src, len);
    d->buf[len] = '\0';
    if (strlen(d->buf) != len) return udecl_fail(d, 0, "the file holds a NUL byte");
    struct cursor c = { d->buf, 0 };
    return parse_block(d, &c, &d->top, 0, 0);
}

int udecl_load(struct udecl *d, const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    uint8_t *src = 0;
    size_t len = 0;
    enum ufile_result r = ufile_slurp(path, 0, &src, &len);
    if (r != UFILE_OK && r != UFILE_EMPTY) {
        memset(d, 0, sizeof *d);
        snprintf(d->what, sizeof d->what, "%s", base);
        return udecl_fail(d, 0, "cannot read %s", path);
    }
    int rc = udecl_parse(d, base, (const char *)src, len);
    free(src);
    return rc;
}

static struct udecl_attr *find(struct udecl_node *n, const char *key) {
    for (int i = 0; i < n->nattr; i++)
        if (!strcmp(n->attr[i].key, key)) { n->attr[i].used = 1; return &n->attr[i]; }
    return 0;
}

const char *udecl_get(struct udecl *d, struct udecl_node *n, const char *key) {
    struct udecl_attr *a = find(n, key);
    if (a && !a->value) {
        udecl_fail(d, n->line, "%s needs a value, as %s=...", key, key);
        return 0;
    }
    return a ? a->value : 0;
}

int udecl_flag(struct udecl *d, struct udecl_node *n, const char *key) {
    struct udecl_attr *a = find(n, key);
    if (a && a->value) {
        udecl_fail(d, n->line, "%s is a flag and takes no value: write %s, or leave it out", key, key);
        return 0;
    }
    return a != 0;
}

const char *udecl_text(struct udecl_node *n) {
    n->text_used = 1;
    return n->text;
}

int udecl_int(struct udecl *d, struct udecl_node *n, const char *key, int lo, int hi, int *out) {
    if (!find(n, key)) return 0;
    const char *v = udecl_get(d, n, key);
    if (!v) return -1;
    int64_t x;
    if (!k_parse_i64(v, &x)) return udecl_fail(d, n->line, "%s=%s is not a number", key, v);
    if (x < lo || x > hi) return udecl_fail(d, n->line, "%s=%s is outside %d..%d", key, v, lo, hi);
    *out = (int)x;
    return 1;
}

int udecl_pair(struct udecl *d, struct udecl_node *n, const char *key, int lo, int hi, int *w, int *h) {
    if (!find(n, key)) return 0;
    const char *v = udecl_get(d, n, key);
    if (!v) return -1;
    const char *x = strchr(v, 'x');
    int64_t a, b;
    if (!x || !k_parse_i64_n(v, (size_t)(x - v), &a) || !k_parse_i64(x + 1, &b))
        return udecl_fail(d, n->line, "%s=%s is not WIDTHxHEIGHT", key, v);
    if (a < lo || a > hi || b < lo || b > hi)
        return udecl_fail(d, n->line, "%s=%s is outside %d..%d", key, v, lo, hi);
    *w = (int)a;
    *h = (int)b;
    return 1;
}

int udecl_word(struct udecl *d, struct udecl_node *n, const char *key,
               const char *const *words, int count, int *out) {
    if (!find(n, key)) return 0;
    const char *v = udecl_get(d, n, key);
    if (!v) return -1;
    for (int i = 0; i < count; i++)
        if (words[i] && !strcmp(words[i], v)) { *out = i; return 1; }
    return udecl_fail(d, n->line, "%s=%s is not one of its words", key, v);
}

int udecl_unused(struct udecl *d, struct udecl_node *n) {
    for (int i = 0; i < n->nattr; i++)
        if (!n->attr[i].used) return udecl_fail(d, n->line, "%s takes no '%s'", n->type, n->attr[i].key);
    if (n->text && !n->text_used) return udecl_fail(d, n->line, "%s takes no text", n->type);
    return 0;
}
