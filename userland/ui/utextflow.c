// Proportional text in lines -- see ui/utextflow.h.
#include "ui/utextflow.h"
#include "ui/ugfx.h"

// Every index is a BYTE offset on a character boundary; the text is
// UTF-8, decoded a character at a time (docs/utf8-design.md).
void utextflow_line(const char *s, int n, int start, int wrap_w, struct utextflow_line *out) {
    int x = 0, run = -1, run_x = 0, cand_end = -1, cand_next = -1, cand_w = 0;
    for (int i = start, nx; i < n; i = nx) {
        uint32_t c;
        nx = ugfx_text_decode(s, i, n, &c);
        if (nx <= i) nx = i + 1;   // a NUL inside the run: one byte, as before
        if (c == '\n') {
            *out = (struct utextflow_line){ start, i, i + 1, x, 0 };
            return;
        }
        int adv = ugfx_cp_advance(c);
        if (c == ' ') {
            // A SPACE NEVER BREAKS A LINE: trailing blanks are invisible,
            // so letting them overflow costs nothing a reader sees.
            if (run < 0) { run = i; run_x = x; }
            x += adv;
            continue;
        }
        if (run > start) { cand_end = run; cand_next = i; cand_w = run_x; }
        run = -1;
        if (wrap_w > 0 && x + adv > wrap_w && i > start) {
            if (cand_end > start) *out = (struct utextflow_line){ start, cand_end, cand_next, cand_w, 0 };
            else *out = (struct utextflow_line){ start, i, i, x, 0 };   // one word wider than the width
            return;
        }
        x += adv;
    }
    *out = (struct utextflow_line){ start, n, n, x, 1 };
}

int utextflow_measure(const char *s, int n, int wrap_w, int *widest) {
    struct utextflow_line l = { 0 };
    int lines = 0, w = 0;
    for (int at = 0;; at = l.next) {
        utextflow_line(s, n, at, wrap_w, &l);
        lines++;
        if (l.width > w) w = l.width;
        if (l.last) break;
    }
    if (widest) *widest = w;
    return lines;
}

int utextflow_width(const char *s, int from, int to) {
    int w = 0;
    uint32_t cp;
    for (int i = from, nx; i < to; i = nx) {
        nx = ugfx_text_decode(s, i, to, &cp);
        if (nx <= i) nx = i + 1;
        w += ugfx_cp_advance(cp);
    }
    return w;
}

void utextflow_point(const char *s, int n, int wrap_w, int index, int *x, int *line) {
    struct utextflow_line l = { 0 };
    int k = 0;
    for (int at = 0;; at = l.next, k++) {
        utextflow_line(s, n, at, wrap_w, &l);
        // A HARD BREAK'S END IS THE NEXT LINE'S START, and the caret
        // there belongs to the next line, as in every editor.
        if (index < l.next || l.last) {
            *x = utextflow_width(s, l.start, index < l.end ? index : l.end);
            *line = k;
            return;
        }
    }
}

int utextflow_line_bounds(const char *s, int n, int wrap_w, int line, int *end) {
    struct utextflow_line l = { 0 };
    for (int at = 0, k = 0;; at = l.next, k++) {
        utextflow_line(s, n, at, wrap_w, &l);
        if (k >= line || l.last) {
            if (end) *end = l.end;
            return l.start;
        }
    }
}

int utextflow_index(const char *s, int n, int wrap_w, int x, int line) {
    int end, i = utextflow_line_bounds(s, n, wrap_w, line < 0 ? 0 : line, &end);
    uint32_t cp;
    for (int pen = 0, nx; i < end; i = nx) {
        nx = ugfx_text_decode(s, i, end, &cp);
        if (nx <= i) nx = i + 1;
        int adv = ugfx_cp_advance(cp);
        if (x < pen + adv / 2) return i;
        pen += adv;
    }
    return end;
}
