// uitest-app: userland/apps/apps/mines
//
// Minesweeper's rules, by its REAL main() headless on the host
// (tools/lib/uitest/uitest_app.h, stage 2). Moved down from
// tools/tests/gui/mines_test.py, which keeps what needs the real stack:
// the secondary click reaching a ring-3 client past the window manager's
// menu, the Best Times dialog, sound, and what a second run remembers.
//
// THE RULES ARE ASSERTED ON THE APP'S OWN STATE LINE (`mines: state ...`),
// and that a change is DRAWN on pixels -- a flagged cell must change and an
// untouched one must not, the SAME cells before and after (comparing two
// different cells passed once because one happened to be open).
//
// The board is fixed: sys_getrandom() is a fixed sequence on the host.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uitest_app.h"

struct state { int ok, phase, cols, rows, mines, flags, revealed, boom, elapsed, best; };

static int field(const char *line, const char *key) {
    char k[24];
    snprintf(k, sizeof k, " %s=", key);
    const char *p = strstr(line, k);
    return p ? atoi(p + strlen(k)) : -999;
}

// The last state line logged since `mark`.
static struct state state_since(size_t mark) {
    struct state s = { 0 };
    char line[256];
    if (!uitest_log_find(mark, "mines: state ", line, sizeof line)) return s;
    s.ok = 1;
    s.phase = field(line, "phase"); s.cols = field(line, "cols"); s.rows = field(line, "rows");
    s.mines = field(line, "mines"); s.flags = field(line, "flags"); s.revealed = field(line, "revealed");
    s.boom = field(line, "boom"); s.elapsed = field(line, "elapsed"); s.best = field(line, "best");
    return s;
}

static int bx, by, cell, fx, fy, fw, fh;

static void cell_centre(int col, int row, int *x, int *y) {
    *x = bx + col * cell + cell / 2;
    *y = by + row * cell + cell / 2;
}

static struct state click_cell(int col, int row) {
    int x, y;
    cell_centre(col, row, &x, &y);
    size_t m = uitest_log_mark();
    uitest_app_click_at(x, y);
    return state_since(m);
}

static struct state rclick_cell(int col, int row) {
    int x, y;
    cell_centre(col, row, &x, &y);
    size_t m = uitest_log_mark();
    uitest_app_rclick_at(x, y);
    return state_since(m);
}

static struct state new_game(void) {
    size_t m = uitest_log_mark();
    uitest_app_click_at(fx + fw / 2, fy + fh / 2);
    return state_since(m);
}

// A cell's interior, inside its border, as one checksum: enough to tell
// "changed" from "the same", which is all these checks ask.
static uint32_t cell_sum(int col, int row) {
    uint32_t h = 2166136261u;
    for (int y = by + row * cell + 2; y < by + (row + 1) * cell - 2; y++)
        for (int x = bx + col * cell + 2; x < bx + (col + 1) * cell - 2; x++)
            h = (h ^ uitest_app_pixel(x, y)) * 16777619u;
    return h;
}

int main(void) {
    uitest_begin("mines_app", "Minesweeper's own main, headless: the rules and what they draw");
    uitest_setting("desktop.layout_log", "on");   // its layout and minefield lines
    size_t start = uitest_log_mark();
    char *argv[] = { "mines", NULL };
    if (!uitest_app_start(1, argv)) return uitest_end();

    char line[256];
    int bw = 0, bh = 0;
    if (uitest_log_find(start, "mines: layout board ", line, sizeof line))
        sscanf(strstr(line, "board ") + 6, "%d %d %d %d", &bx, &by, &bw, &bh);
    if (uitest_log_find(start, "mines: layout cell ", line, sizeof line))
        sscanf(strstr(line, "cell ") + 5, "%d", &cell);
    if (uitest_log_find(start, "mines: layout face ", line, sizeof line))
        sscanf(strstr(line, "face ") + 5, "%d %d %d %d", &fx, &fy, &fw, &fh);
    utest_checkf(cell > 0 && bw > 0 && fw > 0, "it reports its board, cell and face (cell %d)", cell);
    if (cell <= 0) return uitest_end();

    struct state s = state_since(start);
    utest_check(s.ok && s.phase == 0 && s.cols == 9 && s.rows == 9 && s.mines == 10 && s.revealed == 0 &&
                s.flags == 0, "a fresh board is Beginner, covered, with no mines placed yet");

    // --- the board is DRAWN, not merely laid out ------------------------------------
    uint32_t face = uitest_app_pixel(bx + cell / 2, by + 2), ground = uitest_app_pixel(bx - 3, by + bh / 2);
    utest_check(face != ground, "covered cells are painted, not just clickable");

    // --- the clock waits for the first click ------------------------------------------
    uitest_advance_ms(3000);
    uitest_app_tick(3);
    s = rclick_cell(8, 8);   // a flag: the poke that logs the state
    uitest_check_int("the clock does not start until the first click", s.elapsed, 0);
    s = rclick_cell(8, 8);   // and off again

    // --- the first click never loses, and always opens its 3x3 -----------------------
    s = click_cell(4, 4);
    utest_checkf(s.ok && (s.phase == 1 || s.phase == 2) && s.boom == -1 && s.revealed >= 9,
                 "the first click never hits a mine and opens an area (revealed %d)", s.revealed);
    // The clock: elapsed counts whole seconds while a game runs.
    int t1 = s.elapsed;
    uitest_advance_ms(3000);
    uitest_app_tick(3);
    s = rclick_cell(4, 4);   // an open cell: the sampling poke, it changes nothing
    // A game still RUNNING: a first click that won outright would pass a
    // stopped clock here (the fixed board does not, and must not start to).
    utest_checkf(s.ok && s.phase == 1 && s.elapsed > t1, "the clock runs while a game is in progress (%d -> %d)",
                 t1, s.elapsed);

    // --- a right-click flags, and the flag is DRAWN ------------------------------------
    s = new_game();
    utest_check(s.ok && s.phase == 0 && s.flags == 0 && s.revealed == 0, "the face starts a new, covered game");
    uint32_t flag_before = cell_sum(0, 0), ctrl_before = cell_sum(0, 2);
    s = rclick_cell(0, 0);
    uitest_check_int("a right-click on the board flags a cell", s.flags, 1);
    utest_check(cell_sum(0, 0) != flag_before && cell_sum(0, 2) == ctrl_before,
                "the flag is drawn, and an untouched cell is not");
    s = click_cell(0, 0);
    utest_check(s.ok && s.revealed == 0 && s.phase == 0, "a left-click on a flagged cell does nothing");
    s = rclick_cell(0, 0);
    uitest_check_int("a second right-click removes the flag", s.flags, 0);

    // --- a press dragged OFF its cell does not dig it -----------------------------------
    int x, y;
    cell_centre(0, 0, &x, &y);
    size_t m = uitest_log_mark();
    uitest_app_press_at(x, y);
    uitest_app_move_to(uitest_app_w() + 40, y, 1);
    uitest_app_release_at(uitest_app_w() + 40, y);
    s = state_since(m);
    utest_check(!s.ok || (s.revealed == 0 && s.phase == 0), "a press dragged off its cell does NOT dig it");

    // --- a won game: open every safe cell ---------------------------------------------
    s = click_cell(4, 4);   // places the mines, sparing this 3x3
    // The LAST minefield line: this game's, one line for Beginner's ten
    // mines (the app writes twenty to a line).
    int mine[81] = { 0 }, nmines = 0;
    if (uitest_log_find(start, "mines: layout minefield 0", line, sizeof line)) {
        char *p = strstr(line, "minefield 0") + strlen("minefield 0");
        int v, n;
        while (sscanf(p, "%d%n", &v, &n) == 1) {
            if (v >= 0 && v < 81) { mine[v] = 1; nmines++; }
            p += n;
        }
    }
    uitest_check_int("the app reports where its ten mines are", nmines, 10);
    if (nmines == 10) {
        for (int i = 0; i < 81 && s.phase == 1; i++)
            if (!mine[i]) {
                struct state t = click_cell(i % 9, i / 9);
                if (t.ok) s = t;
            }
        utest_checkf(s.phase == 2 && s.boom == -1 && s.flags == 10 && s.best > 0,
                     "opening every safe cell wins, flags every mine and sets a best time (phase %d flags %d best %d)",
                     s.phase, s.flags, s.best);
    }

    uitest_app_keep("won");
    uitest_check_int("closing the window ends the app cleanly", uitest_app_close(), 0);
    return uitest_end();
}
