#ifndef ANSIREGION_SEQ_H
#define ANSIREGION_SEQ_H

// One scrolling-region scenario, shared by the two tests that run it:
// uvterm_test (the GUI Terminal's grid, read as cells) and /tests/
// ansiregion (painted on whichever terminal runs it, read as pixels by
// tools/tests/sweep/ansi_cursor_test.py, whose ANSIREGION_WANT is this
// file's AR_WANT).
//
// THE ROWS ARE NUMBERED, because rows that look alike move invisibly:
// rows 1..AR_ROWS start out carrying their own number (a mark at column
// AR_COL0 + n, or a letter), and the steps move them about inside a
// region of rows 3..10. CHOSEN SO NO STEP CAN BE SKIPPED UNSEEN: drop
// any one of them (or treat the region as the whole screen) and the
// final rows differ from AR_WANT.
#define AR_ROWS 14
#define AR_COL0 20

static const char AR_STEPS[] =
    "\033[3;10r"         // DECSTBM: rows 3..10; the cursor homes
    "\033[2S"            // SU 2: the region's rows up by two
    "\033[5;1H\033[L"    // IL: a blank line at row 5
    "\033[4;1H\033[M"    // DL: row 4 goes
    "\033[T"             // SD 1: the region down by one
    "\033[3;1H\033M"     // RI on the top margin: down by one more
    "\033[10;1H\n\n\n"   // three line feeds on the bottom margin: up by three
    "\033[r";            // the whole screen again

// What row 1..AR_ROWS holds afterwards: its original number, or 0 for
// blank. Rows 1, 2 and 11..14 are OUTSIDE the region and never move.
static const int AR_WANT[AR_ROWS] = { 1, 2, 0, 7, 8, 9, 10, 0, 0, 0, 11, 12, 13, 14 };

#endif
