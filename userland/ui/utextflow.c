// Proportional text in lines -- see ui/utextflow.h.
#include "ui/utextflow.h"
#include "ui/ugfx.h"

void utextflow_line(const char *s, int n, int start, int wrap_w, struct utextflow_line *out) {
    int x = 0, run = -1, run_x = 0, cand_end = -1, cand_next = -1, cand_w = 0;
    for (int i = start; i < n; i++) {
        char c = s[i];
        if (c == '\n') {
            *out = (struct utextflow_line){ start, i, i + 1, x, 0 };
            return;
        }
        int adv = ugfx_char_advance(c);
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

static int width_of(const char *s, int from, int to) {
    int w = 0;
    for (int i = from; i < to; i++) w += ugfx_char_advance(s[i]);
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
            *x = width_of(s, l.start, index < l.end ? index : l.end);
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
    for (int pen = 0; i < end; i++) {
        int adv = ugfx_char_advance(s[i]);
        if (x < pen + adv / 2) return i;
        pen += adv;
    }
    return end;
}
