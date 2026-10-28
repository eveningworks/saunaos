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
#include <string.h>
#include "ui/uvterm.h"
#include "lib/utest.h"

static struct uvterm g_t;   // ~1 KiB of struct; its buffers are malloc'd

static const struct uvterm_cell *row0(void) { return uvterm_line(&g_t, g_t.sb_count); }

int main(void) {
    utest_begin("uvterm_test", "the terminal grid holds characters, not bytes", 0);
    if (!uvterm_init(&g_t, 4, 10, 8, 7, 0)) {
        utest_check(0, "uvterm_init");
        return utest_end();
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
    return utest_end();
}
