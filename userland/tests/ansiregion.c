// Paints ansiregion_seq.h's scenario -- numbered rows moved about by a
// scrolling region -- and then the line-feed pair, on whatever terminal
// runs it, and holds it still to be photographed:
// tools/tests/sweep/ansi_cursor_test.py reads the cells of the physical
// console and of a GUI Terminal window.
//
// THE LINE-FEED PAIR is the output side of the TTY layer: `#\n#` written
// with OPOST on lands its second mark at column 1 of the next row (ONLCR
// made the `\n` a `\r\n`), and written with OPOST off at column 2 -- a
// bare line feed, down a row and the column kept. A terminal that still
// returned the carriage on LF puts both at column 1; a discipline that
// ignored oflag, the same.
//
// Spawned, not `run`: it sleeps (ansidraw.c says why that matters).
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "ansiregion_seq.h"

#define HOLD_MS 6000

static void out(const char *s) { sys_write(1, s, strlen(s)); }

int main(void) {
    out("\033[2J\033[?25l");
    char b[32];
    for (int n = 1; n <= AR_ROWS; n++) {
        snprintf(b, sizeof b, "\033[%d;%dH#", n, AR_COL0 + n);
        out(b);
    }
    out(AR_STEPS);

    // OPOST on (the default): the second mark at row 17, column 1.
    out("\033[16;1H#\n#");

    // OPOST off: row 20, column 2.
    struct tty_termios tio, was;
    int have = sys_tcgetattr(1, &was) == 0;
    if (have) {
        tio = was;
        tio.oflag &= (uint16_t)~TTY_OPOST;
        sys_tcsetattr(1, &tio);
    }
    out("\033[19;1H#\n#");
    if (have) sys_tcsetattr(1, &was);
    // A CURSOR MOVE LAST, before the hold: the physical console presents
    // a character on the next tick or at a cursor op, and with nobody
    // reading the console the tail of a write is never presented at all
    // (docs/bugs.md) -- which would hide the mark this test is about.
    out("\033[22;1H");

    sys_eprint(have ? "ansiregion: painted, holding\n"
                    : "ansiregion: painted (no termios on fd 1), holding\n");
    sys_sleep_ms(HOLD_MS);
    out("\033[?25h\033[22;1H");
    return 0;
}
