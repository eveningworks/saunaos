// See uline.h. Extracted from /bin/tosh's redraw() when /bin/dash needed
// the same paint -- the code is that function's, unchanged.
#include "uline.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "rt/sys.h"   // sys_tcgetwinsz -- the width comes from the kernel
#include "lib/uutf8.h"     // the line is UTF-8: columns are CHARACTERS
#include "lib/uwcwidth.h"  // ...and a wide one takes two

static void put(const char *s) { write(1, s, (size_t)strlen(s)); }

int uline_cols(void) {
    struct tty_winsize ws;
    if (sys_tcgetwinsz(0, &ws) < 0 || ws.cols < 20) return 80;
    return ws.cols;
}

static void cursor_up(int n) {
    if (n <= 0) return;
    char buf[16];
    snprintf(buf, sizeof buf, "\x1b[%dA", n);
    put(buf);
}

void uline_erase(int *row_shown) {
    cursor_up(*row_shown);
    put("\r");
    put("\x1b[J");
    *row_shown = 0;
}

int uline_width(const char *s) {
    int w = 0;
    for (int i = 0; s[i]; ) {
        int n;
        uint32_t cp = uutf8_decode(s + i, -1, &n);
        if (n <= 0) break;
        w += uwcwidth(cp);
        i += n;
    }
    return w;
}

// The paint's output, batched: a write per character would be a syscall
// per cell of a long line.
struct obuf { char b[256]; int n; };
static void ob_flush(struct obuf *o) { if (o->n) write(1, o->b, (size_t)o->n); o->n = 0; }
static void ob_put(struct obuf *o, const char *s, int n) {
    if (o->n + n > (int)sizeof o->b) ob_flush(o);
    memcpy(o->b + o->n, s, (size_t)n);
    o->n += n;
}

// **COLUMNS ARE CHARACTERS, NOT BYTES** (docs/utf8-design.md, step 2):
// `ä` is two bytes and one cell, a CJK character one character and two
// cells. A character that would not fit at a row's end starts the next
// row, as the terminal itself wraps it -- so a wide one never straddles.
void uline_paint(const char *prompt, const struct kline_edit *ed, int *row_shown) {
    int w = uline_cols();
    const char *p = prompt ? prompt : "";

    cursor_up(*row_shown);
    put("\r");
    put("\x1b[J");   // this row and every row below it

    struct obuf o;
    o.n = 0;
    int row = 0, col = 0, cur_row = -1, cur_col = 0;
    for (int seg = 0; seg < 2; seg++) {
        const char *s = seg ? ed->buf : p;
        int n = seg ? ed->len : (int)strlen(p);
        for (int i = 0; i < n; ) {
            int len;
            uint32_t cp = uutf8_decode(s + i, n - i, &len);
            if (len <= 0) break;
            int cw = uwcwidth(cp);
            if (col + cw > w) { ob_put(&o, "\n", 1); row++; col = 0; }
            if (seg && i == ed->cursor && cur_row < 0) { cur_row = row; cur_col = col; }
            ob_put(&o, s + i, len);
            col += cw;
            i += len;
        }
    }
    // The caret past the last character: a row filled to its last column
    // puts it at the start of the next, which must exist to be stood on.
    if (cur_row < 0) {
        if (col >= w) { ob_put(&o, "\n", 1); row++; col = 0; }
        cur_row = row;
        cur_col = col;
    }
    ob_flush(&o);

    cursor_up(row - cur_row);
    char cs[16];
    snprintf(cs, sizeof cs, "\x1b[%dG", cur_col + 1);
    put(cs);

    *row_shown = cur_row;
}

void uline_end(const char *prompt, struct kline_edit *ed, int *row_shown) {
    int save = ed->cursor;
    ed->cursor = ed->len;
    uline_paint(prompt, ed, row_shown);
    ed->cursor = save;
    put("\n");
    *row_shown = 0;
}

void uline_clear_screen(const char *prompt, struct kline_edit *ed, int *row_shown) {
    // ESC[2J ESC[H -- erase the display, caret to the top -- then put
    // the prompt and the half-typed line back. Both ring-3 front ends
    // reach the same behaviour this way; the kernel shell calls the
    // console directly, which is the split klineedit.h describes.
    put("\x1b[2J\x1b[H");
    *row_shown = 0;
    uline_paint(prompt, ed, row_shown);
}

void uline_insert_last_arg(struct kline_edit *ed, const char *previous) {
    if (!previous || !*previous) return;
    int n = (int)strlen(previous);
    int start = kline_ws_word_start(previous, n, n);
    kline_insert_str(ed, previous + start);
}

void uline_list_candidates(const char *prompt, struct kline_edit *ed, int *row_shown,
                           const char *candidates, int count, int stride) {
    if (count <= 0 || stride <= 0 || !candidates) return;
    uline_end(prompt, ed, row_shown);

    int widest = 0;
    for (int i = 0; i < count; i++) {
        int l = uline_width(candidates + (size_t)i * stride);
        if (l > widest) widest = l;
    }
    int colw = widest + 2;
    int cols = uline_cols() / colw;
    if (cols < 1) cols = 1;   // a candidate wider than the window gets a row

    int col = 0;
    for (int i = 0; i < count; i++) {
        const char *c = candidates + (size_t)i * stride;
        put(c);
        if (++col == cols || i + 1 == count) { put("\n"); col = 0; continue; }
        for (int pad = uline_width(c); pad < colw; pad++) put(" ");
    }
    uline_paint(prompt, ed, row_shown);
}
