// Cursor-style name/enum plumbing. Small, but it's the exact thing that
// silently rots: VGA_CURSOR_STYLE_NAMES has to stay in lockstep with
// enum vga_cursor_style by hand (the same convention, and the same
// hazard, as debugflags.c's DBGFLAG_NAMES), and both the `cursor`
// command and cursor_config.c read it generically.
//
// Nothing here touches the framebuffer -- the drawing itself needs
// pixels, which is what the QMP screenshots cover.
#include "ktest.h"
#include "kapi.h"
#include "kutf8.h"

KTEST("vga", "every cursor style has a name, and it parses back") {
    for (int i = 0; i < VGA_CURSOR_STYLE_COUNT; i++) {
        const char *name = VGA_CURSOR_STYLE_NAMES[i];
        KTEST_ASSERT(name != 0);
        KTEST_ASSERT(name[0] != '\0'); // a NULL/empty slot means the table fell behind the enum

        enum vga_cursor_style parsed;
        KTEST_ASSERT(vga_cursor_style_parse(name, &parsed));
        KTEST_ASSERT_EQ((int)parsed, i);
    }
}

KTEST("vga", "an unknown cursor style is rejected, not guessed at") {
    enum vga_cursor_style parsed = VGA_CURSOR_REVERSE;
    KTEST_ASSERT_EQ(vga_cursor_style_parse("nonsense", &parsed), 0);
    KTEST_ASSERT_EQ((int)parsed, (int)VGA_CURSOR_REVERSE); // output untouched on failure
    KTEST_ASSERT_EQ(vga_cursor_style_parse("", &parsed), 0);
    // Case-sensitive, same "reject rather than guess" rule as
    // dbgflag_parse() and the knum parsers.
    KTEST_ASSERT_EQ(vga_cursor_style_parse("Underline", &parsed), 0);
}

KTEST("vga", "setting a style sticks; an out-of-range one is ignored") {
    enum vga_cursor_style original = vga_cursor_style();

    vga_set_cursor_style(VGA_CURSOR_BEAM);
    KTEST_ASSERT_EQ((int)vga_cursor_style(), (int)VGA_CURSOR_BEAM);

    vga_set_cursor_style(VGA_CURSOR_STYLE_COUNT); // out of range
    KTEST_ASSERT_EQ((int)vga_cursor_style(), (int)VGA_CURSOR_BEAM);

    vga_set_cursor_style(original); // tests run in the LIVE kernel -- put it back
    KTEST_ASSERT_EQ((int)vga_cursor_style(), (int)original);
}


// THE CONSOLE'S DECODE (docs/utf8-design.md, step 2): a program, and the
// keyboard, send it UTF-8, and its font is Latin-1. What vga_putc() does
// with each byte, run here without the framebuffer: an a-umlaut split
// across two writes is ONE cell drawn as 0xE4, a-ogonek is one '?', and a
// stray Latin-1 byte is its own letter.
static int console_cells(struct kutf8_stream *st, const char *bytes, int *cells, int cap) {
    int n = 0;
    for (; *bytes; bytes++) {
        uint32_t cps[2];
        int k = kutf8_stream_feed(st, (unsigned char)*bytes, cps);
        for (int i = 0; i < k && n < cap; i++) cells[n++] = vga_console_glyph(cps[i]);
    }
    return n;
}

KTEST("vga", "the console draws UTF-8 a character per cell, '?' past Latin-1") {
    struct kutf8_stream st = { {0}, 0, 0 };
    int cells[8];
    KTEST_ASSERT_EQ(console_cells(&st, "\xC3", cells, 8), 0);   // half of it: nothing yet
    KTEST_ASSERT_EQ(console_cells(&st, "\xA4", cells, 8), 1);   // the other half, a write later
    KTEST_ASSERT_EQ(cells[0], 0xE4);
    KTEST_ASSERT_EQ(console_cells(&st, "\xC4\x85", cells, 8), 1);
    KTEST_ASSERT_EQ(cells[0], '?');
    KTEST_ASSERT_EQ(console_cells(&st, "\xE4s", cells, 8), 2);   // Latin-1 a-umlaut, then s
    KTEST_ASSERT_EQ(cells[0], 0xE4);
    KTEST_ASSERT_EQ(cells[1], 's');
}
