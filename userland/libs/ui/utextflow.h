#ifndef UTEXTFLOW_H
#define UTEXTFLOW_H

// utextflow -- PLAIN TEXT IN LINES, PROPORTIONALLY: where each line of a
// string breaks at a pixel width, where character `index` is drawn, and
// which index a point is nearest. utext's job in pixels instead of
// monospace cells, for text that is drawn in any face (Paint's text box).
//
// ONE WALK, utextflow_line(), answers all three, so a click lands
// exactly where the caret is drawn. The CURRENT ugfx font measures it
// (ugfx_set_font() first). A '\n' always breaks; a width <= 0 never
// wraps. Word wrap breaks at a space, and a word wider than the width
// breaks hard -- utext's rule. Nothing is stored and nothing allocated.

struct utextflow_line {
    int start, end;   // the characters drawn: [start, end)
    int next;         // where the next line begins (past a '\n' or the spaces a wrap ate)
    int width;        // pixels of [start, end)
    int last;         // 1 on the final line
};

// The line beginning at `start` (0, or a previous line's `next`).
void utextflow_line(const char *s, int n, int start, int wrap_w, struct utextflow_line *out);
// How many lines, and the widest in pixels.
int  utextflow_measure(const char *s, int n, int wrap_w, int *widest);
// Where the caret before character `index` goes: x in pixels from the
// line's left, and the line number.
void utextflow_point(const char *s, int n, int wrap_w, int index, int *x, int *line);
// The caret position nearest `x` on line `line` (clamped to the lines there are).
int  utextflow_index(const char *s, int n, int wrap_w, int x, int line);
// The first index of line `line`, and the line's drawn end -- Home and End.
int  utextflow_line_bounds(const char *s, int n, int wrap_w, int line, int *end);
// The pixel width of s[from, to), measured the way a line is (no kerning).
int  utextflow_width(const char *s, int from, int to);

#endif
