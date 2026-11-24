// ui/uvterm.h's grid as UTF-8 (docs/utf8-design.md, step 2): what the
// terminal does with the bytes a program writes, without a window, a pty
// or a reader thread -- uvterm_write() is the parser's whole input.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: every ASCII check anyone writes,
// and a whole `ä` written in ONE call (a decoder that kept no state
// between calls decodes that fine). So the sequence is split across two
// writes, as a pty read splits it; a wide character must take two cells
// and its copy one character; and a stray Latin-1 byte must still be its
// letter (the transition rule), not dropped and not `?`.
#include <stdio.h>
#include <string.h>
#include "ui/uvterm.h"
#include "uitest.h"
#include "ansiregion_seq.h"

static struct uvterm g_t;   // ~1 KiB of struct; its buffers are malloc'd

static const struct uvterm_cell *row0(void) { return uvterm_line(&g_t, g_t.sb_count); }

// Line `line` of the virtual buffer as ASCII up to its last non-blank, a
// wide character as '#', its continuation skipped.
static const char *text_of(const struct uvterm *t, int line) {
    static char bufs[4][128];   // a check's message reads several at once
    static int next;
    char *buf = bufs[next++ & 3];
    const struct uvterm_cell *r = uvterm_line(t, line);
    int n = 0, end = 0;
    for (int c = 0; c < t->cols && n < 127; c++) {
        if (r[c].wide == UVTERM_CONT) continue;
        buf[n++] = r[c].wide == UVTERM_WIDE ? '#' : r[c].ch < 128 ? (char)r[c].ch : '?';
        if (r[c].ch != ' ') end = n;
    }
    buf[end] = 0;
    return buf;
}

// A WIDTH CHANGE REFLOWS: what a soft wrap split is one line again, a
// hard newline is not, the cursor keeps its place, and it round-trips.
// A broken version that crops passes every "the text is still there"
// check at a wider size -- so the checks read WHERE each letter lands.
static void reflow_checks(void) {
    static struct uvterm t;
    if (!uvterm_init(&t, 4, 10, 16, 7, 0)) { utest_check(0, "uvterm_init (reflow)"); return; }
    uvterm_write(&t, "abcdefghijklmnopqrstuvwxy", 25);   // 10 + 10 + 5 at 10 columns
    utest_checkf(t.cr == 2 && t.cc == 5 && !strcmp(text_of(&t, 0), "abcdefghij"),
                 "25 letters wrap over three rows at 10 columns (cursor %d,%d)", t.cr, t.cc);
    uvterm_resize(&t, 4, 30);
    utest_checkf(!strcmp(text_of(&t, 0), "abcdefghijklmnopqrstuvwxy") && t.cr == 0 && t.cc == 25 &&
                 !strcmp(text_of(&t, 1), ""),
                 "widening joins them into one row, the cursor after the last letter (got '%s', %d,%d)",
                 text_of(&t, 0), t.cr, t.cc);
    uvterm_write(&t, "\r\nxy", 4);
    uvterm_resize(&t, 4, 8);
    utest_checkf(!strcmp(text_of(&t, 0), "abcdefgh") && !strcmp(text_of(&t, 3), "y") &&
                 !strcmp(text_of(&t, 4), "xy") && t.sb_count == 1 && t.cr == 3 && t.cc == 2,
                 "narrowing resplits at 8, a hard newline stays one, the top row into history "
                 "(sb %d, cursor %d,%d, row 4 '%s')", t.sb_count, t.cr, t.cc, text_of(&t, 4));
    uvterm_resize(&t, 4, 30);
    utest_checkf(!strcmp(text_of(&t, 0), "abcdefghijklmnopqrstuvwxy") && !strcmp(text_of(&t, 1), "xy") &&
                 t.sb_count == 0 && t.cr == 1 && t.cc == 2,
                 "widening again takes the line back out of the history, whole (sb %d, cursor %d,%d)",
                 t.sb_count, t.cr, t.cc);

    // A WIDTH CHANGE ALONE PULLS NO HISTORY BACK: a cleared screen stays clear.
    uvterm_init(&t, 4, 10, 16, 7, 0);
    uvterm_write(&t, "1\r\n2\r\n3\r\n4\r\n5\r\n6\x1b[2J\x1b[H", 24);
    int sb = t.sb_count;
    uvterm_resize(&t, 4, 12);
    utest_checkf(sb == 2 && t.sb_count == 2 && t.cr == 0 && !strcmp(text_of(&t, t.sb_count), ""),
                 "after a clear, widening leaves the screen clear (sb %d -> %d, cursor row %d)",
                 sb, t.sb_count, t.cr);

    // The cursor ON a character that lands at a new row's start is there.
    uvterm_init(&t, 4, 20, 16, 7, 0);
    uvterm_write(&t, "abcdefghijklmnop\x1b[8D", 20);   // back onto 'i'
    uvterm_resize(&t, 4, 8);
    utest_checkf(t.cr == 1 && t.cc == 0 && uvterm_line(&t, t.sb_count + 1)[0].ch == 'i',
                 "a cursor on the first letter of a new row stays on it (cursor %d,%d)", t.cr, t.cc);

    // A tab that runs past the edge wraps softly too.
    uvterm_init(&t, 4, 8, 16, 7, 0);
    uvterm_write(&t, "abcdefg\tXYZ", 11);
    uvterm_resize(&t, 4, 30);
    utest_checkf(!strcmp(text_of(&t, 0), "abcdefg XYZ"),
                 "a tab past the edge rejoins when widened (got '%s')", text_of(&t, 0));

    // A wide character that did not fit left a blank at the edge, which
    // is not part of the line.
    uvterm_init(&t, 4, 10, 16, 7, 0);
    uvterm_write(&t, "abcdefghi\xE4\xB8\xAD", 12);   // nine letters, then U+4E2D
    utest_checkf(!strcmp(text_of(&t, 1), "#"), "a wide character that does not fit starts the next row");
    uvterm_resize(&t, 4, 20);
    utest_checkf(!strcmp(text_of(&t, 0), "abcdefghi#"),
                 "...and widening puts it straight after the letters, no gap (got '%s')", text_of(&t, 0));
    uvterm_free(&t);
}

int main(void) {
    uitest_begin("uvterm", "the terminal grid holds characters, not bytes");
    if (!uvterm_init(&g_t, 4, 10, 8, 7, 0)) {
        utest_check(0, "uvterm_init");
        return uitest_end();
    }

    // a-umlaut is C3 A4, written as two separate writes.
    uvterm_write(&g_t, "K\xC3", 2);
    utest_checkf(g_t.cc == 1, "half a sequence draws nothing yet (cursor at %d)", g_t.cc);
    uvterm_write(&g_t, "\xA4", 1);
    const struct uvterm_cell *r = row0();
    utest_checkf(r[1].ch == 0xE4 && g_t.cc == 2 && r[2].ch == ' ',
                 "a sequence split across two writes is ONE cell, U+00E4 (got %x, cursor %d)",
                 (unsigned)r[1].ch, g_t.cc);

    // A wide character: two cells, the second a continuation.
    uvterm_write(&g_t, "\xE4\xB8\xAD", 3);   // U+4E2D
    utest_checkf(r[2].ch == 0x4E2D && r[2].wide == UVTERM_WIDE && r[3].wide == UVTERM_CONT &&
                 g_t.cc == 4, "an East Asian Wide character takes two cells (cursor %d)", g_t.cc);

    // A stray Latin-1 byte is its letter, and the byte after it is itself.
    uvterm_write(&g_t, "\xE4s", 2);
    utest_checkf(r[4].ch == 0xE4 && r[5].ch == 's' && g_t.cc == 6,
                 "a stray Latin-1 byte is its own letter (got %x %x)", (unsigned)r[4].ch,
                 (unsigned)r[5].ch);

    // The copy is UTF-8, a character at a time.
    g_t.sel_a.line = g_t.sel_b.line = g_t.sb_count;
    g_t.sel_a.col = 0;
    g_t.sel_b.col = 6;
    g_t.sel_on = 1;
    char out[32];
    int n = uvterm_sel_text(&g_t, out, sizeof out);
    const char *want = "K\xC3\xA4\xE4\xB8\xAD\xC3\xA4s";
    utest_checkf(n == (int)strlen(want) && !memcmp(out, want, (size_t)n),
                 "a selection copies UTF-8 (%d bytes)", n);
    struct uvterm_point p = { g_t.sb_count, 1 };
    utest_check(uvterm_sel_word(&g_t, p) && uvterm_sel_text(&g_t, 0, 0) == (int)strlen(want),
                "a double-click selects letters past ASCII as one word");

    // A wide character that does not fit at the edge starts the next row.
    uvterm_write(&g_t, "\r\nabcdefghi", 11);   // nine cells of ten
    uvterm_write(&g_t, "\xE4\xB8\xAD", 3);
    const struct uvterm_cell *r2 = uvterm_line(&g_t, g_t.sb_count + 2);
    utest_checkf(g_t.cr == 2 && r2 && r2[0].ch == 0x4E2D && g_t.cc == 2,
                 "a wide character never straddles the edge (row %d col %d)", g_t.cr, g_t.cc);

    uvterm_free(&g_t);

    // --- a line feed, and the scrolling region ------------------------
    //
    // xterm's LF: down a row, the COLUMN KEPT. The carriage return that
    // starts a program's next line at the left comes from the tty's
    // ONLCR, which this grid never sees -- so a grid that still returned
    // the carriage would put `cd` under `ab` instead of after it.
    static struct uvterm t;
    if (!uvterm_init(&t, 16, 40, 8, 7, 0)) {
        utest_check(0, "uvterm_init (16x40)");
        return uitest_end();
    }
    uvterm_write(&t, "ab\ncd\r\nx", 8);
    const struct uvterm_cell *l0 = uvterm_line(&t, t.sb_count);
    const struct uvterm_cell *l1 = uvterm_line(&t, t.sb_count + 1);
    const struct uvterm_cell *l2 = uvterm_line(&t, t.sb_count + 2);
    utest_checkf(l0[0].ch == 'a' && l1[0].ch == ' ' && l1[1].ch == ' ' &&
                 l1[2].ch == 'c' && l1[3].ch == 'd',
                 "a bare LF keeps the column (row 2 starts '%c%c%c')",
                 (int)l1[0].ch, (int)l1[1].ch, (int)l1[2].ch);
    utest_checkf(l2[0].ch == 'x', "CR LF starts the next row at column 1 (got '%c')",
                 (int)l2[0].ch);

    // The NUMBERED ROWS: row n holds the letter 'A' + n - 1, then
    // ansiregion_seq.h's steps move them inside rows 3..10.
    uvterm_write(&t, "\033[2J", 4);
    char b[24];
    for (int n = 1; n <= AR_ROWS; n++) {
        int k = snprintf(b, sizeof b, "\033[%d;1H%c", n, 'A' + n - 1);
        uvterm_write(&t, b, k);
    }
    uvterm_write(&t, AR_STEPS, (int)sizeof AR_STEPS - 1);
    char got[AR_ROWS + 1], expect[AR_ROWS + 1];
    for (int n = 1; n <= AR_ROWS; n++) {
        const struct uvterm_cell *row = uvterm_line(&t, t.sb_count + n - 1);
        got[n - 1] = row ? (char)row[0].ch : '?';
        expect[n - 1] = AR_WANT[n - 1] ? (char)('A' + AR_WANT[n - 1] - 1) : ' ';
    }
    got[AR_ROWS] = expect[AR_ROWS] = '\0';
    utest_checkf(!strcmp(got, expect), "the region's steps leave rows 1-14 as \"%s\" (got \"%s\")",
                 expect, got);
    utest_checkf(t.sb_count == 0, "a region's scroll feeds no scrollback (%d lines)", t.sb_count);
    utest_checkf(t.cr == 0 && t.cc == 0, "resetting the region homes the cursor (%d,%d)",
                 t.cr, t.cc);

    // A line INSERTED under a coloured background takes that colour, as
    // the console's does (xterm's BCE) -- the two terminals must agree.
    uvterm_write(&t, "\033[5;1H\033[44m\033[L\033[0m", 18);
    const struct uvterm_cell *ins = uvterm_line(&t, t.sb_count + 4);
    utest_checkf(ins && ins[0].bg == VGA_BLUE && ins[0].ch == ' ',
                 "an inserted line is blanked in the current background (bg %d)",
                 ins ? ins[0].bg : -1);
    uvterm_free(&t);

    // A CLUSTER IS ONE CELL'S (api/kgrapheme.h): a mark goes into the cell of
    // its letter, a ZWJ family into one wide cell, and a copy gets every
    // byte back.
    static struct uvterm c;   // zeroed: uvterm_init() frees what a reused struct held
    if (!uvterm_init(&c, 4, 20, 8, 7, 0)) {
        utest_check(0, "uvterm_init (4x20)");
        return uitest_end();
    }
    static const char FAMILY[] = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    uvterm_write(&c, "q\xCC\x81x", 4);              // q, U+0301 (no precomposed form), x
    const struct uvterm_cell *cr = uvterm_line(&c, c.sb_count);
    const uint32_t *cps;
    utest_checkf(ugfx_cluster_cps(cr[0].ch, &cps) == 2 && cps[0] == 'q' && cps[1] == 0x301 && cr[1].ch == 'x' &&
                 c.cc == 2, "a mark joins its letter's cell (x in cell %d, cursor %d)", cr[1].ch == 'x' ? 1 : -1, c.cc);
    uvterm_write(&c, "e\xCC\x81", 3);
    utest_checkf(cr[2].ch == 0xE9 && c.cc == 3, "e and U+0301 compose in their cell (U+%04X)", (unsigned)cr[2].ch);
    uvterm_write(&c, FAMILY, (int)sizeof FAMILY - 1);
    utest_checkf(ugfx_cluster_cps(cr[3].ch, &cps) == 5 && cr[3].wide == UVTERM_WIDE && cr[4].wide == UVTERM_CONT &&
                 c.cc == 5, "a ZWJ family is ONE cluster in one wide cell (cursor %d)", c.cc);
    uvterm_sel_all(&c);
    char copied[64];
    int gn = uvterm_sel_text(&c, copied, sizeof copied);
    static const char WANT[] = "q\xCC\x81x\xC3\xA9\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    utest_checkf(gn >= (int)sizeof WANT - 1 && !memcmp(copied, WANT, sizeof WANT - 1),
                 "a copy gives every code point of each cell back (%d bytes)", gn);
    // DRAWN, a cluster cell shows ITS letter: a cell value that is a token is
    // not a code point, and in the batched string it came out as another
    // glyph entirely. Every pixel of a plain 'q' cell must be inked in the
    // cell holding q and U+0301.
    {
        enum { SW = 400, SH = 60 };
        static uint32_t px[SW * SH], plain[SW * SH];
        static const uint32_t pal[16] = { 0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
                                          0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF };
        // In the MONO face, as the Terminal draws: there a letter's advance
        // IS the cell, which is what sends a cell down the batched path.
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
        int cw = ugfx_char_w(), chh = ugfx_char_h();
        struct uvterm_look look = { .cw = cw, .ch = chh, .pal = pal, .cursor_rgb = 0xFFFFFF, .caret = 0 };
        static struct uvterm q;
        uvterm_init(&q, 4, 20, 8, 7, 0);
        uvterm_write(&q, "qx", 2);
        struct ugfx_surface a = { .pixels = px, .w = SW, .h = SH }, b = { .pixels = plain, .w = SW, .h = SH };
        uvterm_sel_clear(&c);
        uvterm_draw(&c, &a, 0, 0, &look);
        uvterm_draw(&q, &b, 0, 0, &look);
        int need = 0, missing = 0;
        for (int y = 0; y < chh && y < SH; y++)
            for (int x = 0; x < cw; x++)
                if (plain[y * SW + x] & 0xFFFFFF) { need++; missing += !(px[y * SW + x] & 0xFFFFFF); }
        utest_checkf(need > 10 && missing == 0, "cell 0 (q and U+0301, one cluster) draws its q: %d of %d pixels missing",
                     missing, need);
        ugfx_set_font(was);
        uvterm_free(&q);
    }
    uvterm_free(&c);
    reflow_checks();
    return uitest_end();
}
