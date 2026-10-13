// umd -- Markdown as HTML, for a page a web engine draws (Help and
// System Update's What's new, on ui/uui_webview.h). See lib/umd.h.
//
// The SAME line classifier and inline walk as the terminal and widget
// renderers, so the three cannot disagree about what a line is; this
// file only decides which tags a block becomes.
#include "lib/umd.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void put(struct umd_out *o, const char *s, int n) {
    if (o->overflow) return;
    if (o->len + n >= o->cap) {
        o->overflow = 1;
        return;
    }
    memcpy(o->buf + o->len, s, (size_t)n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void puts_(struct umd_out *o, const char *s) { put(o, s, (int)strlen(s)); }

static void putf(struct umd_out *o, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) put(o, b, n < (int)sizeof b ? n : (int)sizeof b - 1);
}

static void esc_char(struct umd_out *o, char c) {
    switch (c) {
    case '&': puts_(o, "&amp;"); break;
    case '<': puts_(o, "&lt;"); break;
    case '>': puts_(o, "&gt;"); break;
    case '"': puts_(o, "&quot;"); break;
    default:  put(o, &c, 1);
    }
}

static void esc(struct umd_out *o, const char *s, int n) {
    for (int i = 0; i < n; i++) esc_char(o, s[i]);
}

// --- inline: styled characters into tags --------------------------------

struct ictx {
    struct umd_out *o;
    const struct umd_html_opts *opts;
    unsigned style;           // what is open
    char code[128];           // the code span so far: it may be a link
    int ncode;
};

static void close_code(struct ictx *c) {
    char href[256], text[128];
    c->code[c->ncode] = '\0';
    href[0] = text[0] = '\0';
    if (c->opts && c->opts->link &&
        c->opts->link(c->opts->ctx, c->code, href, sizeof href, text, sizeof text)) {
        puts_(c->o, "<a href=\"");
        esc(c->o, href, (int)strlen(href));
        puts_(c->o, "\">");
        if (text[0]) esc(c->o, text, (int)strlen(text));
        else esc(c->o, c->code, c->ncode);
        puts_(c->o, "</a>");
    } else {
        puts_(c->o, "<code>");
        esc(c->o, c->code, c->ncode);
        puts_(c->o, "</code>");
    }
    c->ncode = 0;
}

static void set_style(struct ictx *c, unsigned want) {
    if ((c->style & UMD_STYLE_CODE) && !(want & UMD_STYLE_CODE)) close_code(c);
    if ((c->style & UMD_STYLE_BOLD) && !(want & UMD_STYLE_BOLD)) puts_(c->o, "</b>");
    if (!(c->style & UMD_STYLE_BOLD) && (want & UMD_STYLE_BOLD)) puts_(c->o, "<b>");
    c->style = want;
}

static void emit(void *ctx, char ch, unsigned style) {
    struct ictx *c = ctx;
    if (style != c->style) set_style(c, style);
    if (style & UMD_STYLE_CODE) {
        if (c->ncode < (int)sizeof c->code - 1) c->code[c->ncode++] = ch;
        return;
    }
    esc_char(c->o, ch);
}

// One run of inline text, everything it opened closed again.
static void inline_text(struct umd_out *o, const struct umd_html_opts *opts, const char *s, int n) {
    struct ictx c = { o, opts, 0, { 0 }, 0 };
    unsigned st = 0;
    umd_inline_walk_from(s, n, &st, emit, &c);
    set_style(&c, 0);
}

// --- blocks -----------------------------------------------------------------

// A table row's cells, split at `|` but not `\|`, trimmed.
static void table_row(struct umd_out *o, const struct umd_html_opts *opts, const char *s, int n,
                      int head) {
    puts_(o, "<tr>");
    int i = 0;
    if (i < n && s[i] == '|') i++;
    while (i < n) {
        int start = i;
        while (i < n && !(s[i] == '|' && (i == 0 || s[i - 1] != '\\'))) i++;
        int a = start, b = i;
        while (a < b && s[a] == ' ') a++;
        while (b > a && s[b - 1] == ' ') b--;
        if (i < n || b > a) {
            puts_(o, head ? "<th>" : "<td>");
            inline_text(o, opts, s + a, b - a);
            puts_(o, head ? "</th>" : "</td>");
        }
        if (i < n) i++;
        if (i == n) break;
    }
    puts_(o, "</tr>\n");
}

enum { IN_NONE, IN_PARA, IN_UL, IN_OL, IN_PRE, IN_TABLE };

static void close_block(struct umd_out *o, int *in) {
    static const char *const END[] = { "", "</p>\n", "</li></ul>\n", "</li></ol>\n", "</pre>\n",
                                       "</table>\n" };
    puts_(o, END[*in]);
    *in = IN_NONE;
}

void umd_html(const char *src, int len, const struct umd_html_opts *opts, struct umd_out *out) {
    int i = 0, in = IN_NONE, fence = 0;
    int folding = 0;              // inside <details>, and what to do with it
    int mode = opts ? opts->details : UMD_HTML_DETAILS_OPEN;
    int table_head = 0;
    const char *line;
    int n;
    if (mode == UMD_HTML_DETAILS_ONLY && opts->details_href) {
        puts_(out, "<p class=\"back\"><a href=\"");
        esc(out, opts->details_href, (int)strlen(opts->details_href));
        puts_(out, "\">&larr; Back</a></p>\n");
    }
    while (i < len && !out->overflow) {
        n = umd_next_line(src, len, &i, &line);
        int arg = 0, tn = 0;
        const char *t = line;
        enum umd_block k = umd_classify(line, n, &arg, &t, &tn);

        if (k == UMD_DETAILS) { close_block(out, &in); folding = 1; continue; }
        if (k == UMD_DETAILS_END) { close_block(out, &in); folding = 0; continue; }
        if (k == UMD_SUMMARY) {
            close_block(out, &in);
            if (mode == UMD_HTML_DETAILS_LINK) {
                puts_(out, "<p class=\"fold\"><a href=\"");
                esc(out, opts->details_href, (int)strlen(opts->details_href));
                puts_(out, "\">");
                inline_text(out, opts, t, tn);
                puts_(out, "</a></p>\n");
            } else {
                puts_(out, "<h3 class=\"fold\">");
                inline_text(out, opts, t, tn);
                puts_(out, "</h3>\n");
            }
            continue;
        }
        // What a page outside / inside the fold leaves out.
        if (mode == UMD_HTML_DETAILS_LINK && folding) continue;
        if (mode == UMD_HTML_DETAILS_ONLY && !folding) continue;

        if (fence) {
            if (k == UMD_FENCE) { close_block(out, &in); fence = 0; }
            else { esc(out, line, n); puts_(out, "\n"); }
            continue;
        }
        switch (k) {
        case UMD_FENCE:
            close_block(out, &in);
            puts_(out, "<pre>");
            in = IN_PRE;
            fence = 1;
            break;
        case UMD_PRE:
            if (in != IN_PRE) { close_block(out, &in); puts_(out, "<pre>"); in = IN_PRE; }
            esc(out, t, tn);
            puts_(out, "\n");
            break;
        case UMD_BLANK:
            if (in != IN_PRE) close_block(out, &in);
            else puts_(out, "\n");
            break;
        case UMD_HEADING:
            close_block(out, &in);
            putf(out, "<h%d>", arg);
            inline_text(out, opts, t, tn);
            putf(out, "</h%d>\n", arg);
            break;
        case UMD_BULLET:
        case UMD_NUMBERED: {
            int want = k == UMD_BULLET ? IN_UL : IN_OL;
            if (in == want) puts_(out, "</li><li>");
            else { close_block(out, &in); puts_(out, want == IN_UL ? "<ul><li>" : "<ol><li>"); in = want; }
            inline_text(out, opts, t, tn);
            break;
        }
        case UMD_TABLE:
            if (arg == 1) { table_head = 0; break; }   // the ---|--- rule
            if (in != IN_TABLE) {
                close_block(out, &in);
                puts_(out, "<table>\n");
                in = IN_TABLE;
                table_head = 1;
            }
            // A row is a header only when the rule follows it.
            {
                int j = i;
                const char *nx;
                int nn = j < len ? umd_next_line(src, len, &j, &nx) : 0, a2 = 0, tn2 = 0;
                const char *t2;
                int head = table_head && nn && umd_classify(nx, nn, &a2, &t2, &tn2) == UMD_TABLE && a2 == 1;
                table_row(out, opts, t, tn, head);
                table_head = 0;
            }
            break;
        case UMD_RULE:
            close_block(out, &in);
            puts_(out, "<hr>\n");
            break;
        default:   // a paragraph line; a list item's continuation joins its item
            while (tn > 0 && (*t == ' ' || *t == '\t')) t++, tn--;
            if (in == IN_UL || in == IN_OL || in == IN_PARA) puts_(out, " ");
            else { close_block(out, &in); puts_(out, "<p>"); in = IN_PARA; }
            inline_text(out, opts, t, tn);
            break;
        }
    }
    close_block(out, &in);
}
