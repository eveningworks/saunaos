#ifndef API_ANSI_H
#define API_ANSI_H

#include <stdint.h>
#include "vga.h" // enum vga_color -- what an SGR parameter resolves to

// ANSI escape sequences as a pure state machine: the SGR (colour)
// subset, plus cursor movement and erasing.
//
// WHY THIS EXISTS. Colour used to be a SYSCALL -- `ls` called
// sys_set_color(), which reaches around the byte stream and changes the
// console's state directly. That is a side channel, and it leaks in the
// obvious way: `ls > out.txt` recoloured the console while its bytes went
// to the file, and `ls | cat` coloured whatever the console happened to
// be printing instead. An escape sequence travels IN the stream, so it
// lands wherever the output lands -- which is why every real terminal
// does it this way and why `--color=never` is a thing a program can
// meaningfully offer.
//
// WHAT IS AND IS NOT SUPPORTED. SGR (`ESC [ ... m`, including REVERSE
// VIDEO, which is what a status bar is made of), cursor movement
// (CUP/CUU/CUD/CUF/CUB/CHA/VPA), erasing (ED/EL), save/restore, DECTCEM
// cursor visibility (`?25h`/`?25l`), and the SCROLLING REGION: DECSTBM
// (`ESC [ t ; b r`) with the line operations that work inside it -- IL
// and DL (`ESC [ n L`/`M`), SU and SD (`ESC [ n S`/`T`) and RI (`ESC M`).
// That is the set a program drawing a screen actually uses -- a board
// game repainting a grid, a progress display rewriting one line, a pager
// or an editor scrolling part of the screen under a fixed status bar.
//
// Cursor movement was swallowed until 2026-08-21, on the reasoning that
// a program moving the cursor would fight the console's scrollback. The
// scrollback part of that is still true and is handled rather than
// avoided: it records the BYTE STREAM, so a cursor-addressed program's
// history is not a meaningful transcript. Real terminals have the same
// problem and solve it with an alternate screen buffer, which the GUI
// Terminal has (ANSI_OP_ALT_ON) and the console does not. What is NOT
// here: tab stops, origin mode (DECOM), left/right margins (DECSLRM) and
// character-set selection.
//
// NO HARDWARE, ON PURPOSE. This file resolves bytes to colours and knows
// nothing about a screen, so vga.c drives it for the physical console
// AND for any installed sink (a GUI Terminal's scrollback), and a KTEST
// exercises it with no display at all. That is the same split geom.h
// uses for drawing.
//
// The 8 ANSI colours are NOT in the same order as enum vga_color -- ANSI
// counts 0..7 as black/red/green/yellow/blue/magenta/cyan/white while VGA
// interleaves its bits -- so the mapping is a table, not arithmetic.

enum ansi_result {
    ANSI_PASS,   // not part of a sequence -- print this byte normally
    ANSI_EATEN,  // consumed as part of a sequence; nothing to do yet
    ANSI_SGR,    // a complete SGR sequence: apply `fg`/`bg` from the state
    ANSI_CTRL,   // a complete cursor/erase sequence: act on `op`/`a`/`b`
    // A complete OSC: the terminal has been told its TITLE. `osc` holds
    // it and `a` says which of the two strings it is (see ANSI_OSC_*).
    //
    // **A CONSUMER MAY IGNORE THIS, and the physical console does.** The
    // sequence is swallowed either way, which is the whole contract: a
    // program that sets a title is correct on a terminal that has
    // nowhere to put one.
    ANSI_OSC,
};

// Which string an ANSI_OSC carries, from the number before the first
// `;`. xterm defined 0 as "both", and every terminal since has kept it
// -- so a shell that only wants a window title sends 0 and gets it.
#define ANSI_OSC_BOTH  0 // icon name AND window title
#define ANSI_OSC_ICON  1 // icon name only
#define ANSI_OSC_TITLE 2 // window title only

// The longest title kept. A longer one is TRUNCATED rather than
// refused, which is the one place this parser guesses instead of
// rejecting -- and deliberately: a title is decoration, so losing its
// tail is better than losing the whole thing, and no consumer can be
// harmed by a short string. Every real terminal truncates too.
#define ANSI_OSC_MAX 64

// What an ANSI_CTRL asks for. The parser resolves the letter AND applies
// the defaults, so a caller never repeats rules like "a missing or zero
// count means 1" -- which is exactly the sort of thing that ends up
// implemented differently in two places.
enum ansi_op {
    ANSI_OP_NONE = 0,
    ANSI_OP_MOVE_TO,   // CUP/HVP: row `a`, column `b`, both 1-BASED
    ANSI_OP_UP,        // CUU: by `a`
    ANSI_OP_DOWN,      // CUD
    ANSI_OP_RIGHT,     // CUF
    ANSI_OP_LEFT,      // CUB
    ANSI_OP_COLUMN,    // CHA: to column `a`, 1-based
    ANSI_OP_ROW,       // VPA: to row `a`, 1-based
    // Erase. `a` is the mode: 0 = cursor to end, 1 = start to cursor,
    // 2 = all. ED's mode 3 (scrollback too) is reported as 2 -- see
    // vga.c on why the history is not the screen's to erase.
    ANSI_OP_ERASE_DISPLAY, // ED
    ANSI_OP_ERASE_LINE,    // EL
    ANSI_OP_SAVE,      // SCP -- save the position
    ANSI_OP_RESTORE,   // RCP
    ANSI_OP_SHOW,      // DECTCEM `?25h`
    ANSI_OP_HIDE,      // `?25l`
    // THE ALTERNATE SCREEN, DEC private mode 1049. A full-screen
    // program switches to a second screen, does its work, and switches
    // back -- and what was on the terminal before it started is exactly
    // what is there afterwards. That is why `less` and `vim` leave no
    // wreckage behind on any real terminal, and why a pager that merely
    // cleared the screen would still lose whatever the shell had
    // printed before it ran.
    //
    // 1049 rather than 47 or 1047: it is the one that saves the CURSOR
    // as well as the screen, and clears the alternate before switching
    // to it, so a program does not have to do those itself. xterm added
    // it for exactly that reason and every terminal since implements
    // it.
    //
    // **A CONSUMER MAY IGNORE THESE**, and the physical console does.
    // The sequence is swallowed rather than printed either way, so a
    // program that uses them is correct on a terminal that has no
    // second screen -- it simply does not get its restore.
    ANSI_OP_ALT_ON,    // `?1049h` -- save cursor, switch, clear
    ANSI_OP_ALT_OFF,   // `?1049l` -- switch back, restore cursor

    // THE SCROLLING REGION AND THE LINE OPERATIONS. What each one moves
    // is ansi_region_op()'s business, not the caller's: a terminal only
    // stores the region and executes the shift it is handed.
    ANSI_OP_MARGINS,      // DECSTBM: rows `a`..`b`, 1-based; `b` 0 = the
                          // bottom row, which the parser cannot know
    ANSI_OP_INSERT_LINES, // IL: `a` blank lines at the cursor
    ANSI_OP_DELETE_LINES, // DL: `a` lines at the cursor
    ANSI_OP_SCROLL_UP,    // SU: the region's text up by `a`
    ANSI_OP_SCROLL_DOWN,  // SD: down by `a`
    ANSI_OP_REVERSE_INDEX,// RI (`ESC M`): up a row, scrolling down at the top
    // NEVER FROM THE PARSER: a line feed, or an autowrap's. Here so the
    // one function that knows the region answers for it too.
    ANSI_OP_LINEFEED,
};

#define ANSI_MAX_PARAMS 8

struct ansi_parser {
    uint8_t state;      // internal; zero-initialised is "ground"
    uint8_t nparam;
    uint8_t bold;       // SGR 1 -- brightens the foreground
    uint8_t reverse;    // SGR 7 -- fg and bg swap, and the parser does
                        // the swapping, so a caller never sees this
    uint8_t private;    // a `?`-style private sequence, swallowed whole
    uint16_t param[ANSI_MAX_PARAMS];
    enum vga_color fg;  // the colours a completed ANSI_SGR resolves to
    enum vga_color bg;
    // What a completed ANSI_CTRL resolves to. `a`/`b` already have
    // their defaults applied.
    enum ansi_op op;
    uint16_t a, b;
    // The text of a completed ANSI_OSC, NUL-terminated. Meaningless in
    // any other result, and rewritten by the next OSC.
    char osc[ANSI_OSC_MAX];
    uint8_t osc_len;
};

// Arms a parser with the colours the console currently shows, so a
// sequence that only sets a foreground leaves the background alone.
void ansi_init(struct ansi_parser *p, enum vga_color fg, enum vga_color bg);

// One byte in. See enum ansi_result -- the ONLY byte a caller prints is
// one that comes back ANSI_PASS.
enum ansi_result ansi_feed(struct ansi_parser *p, char c);

// --- the scrolling region -------------------------------------------
//
// DECSTBM confines scrolling to rows top..bot: a line feed on `bot`
// scrolls only those rows, and the rows outside stay put -- what lets a
// program keep a status line still while text scrolls above it. These
// are the RULES, shared by every terminal that draws (vga.c's console
// and uvterm's grid), so the two cannot disagree about where a line
// lands; each one only moves its own rows when told to.

// Rows top..bot, 0-BASED and INCLUSIVE.
struct ansi_region { uint16_t top, bot; };

// The whole screen -- the state at start, and after a resize (xterm's).
void ansi_region_reset(struct ansi_region *r, int rows);

// An ANSI_OP_MARGINS on a screen of `rows`. Returns 1 if it took -- the
// caller then HOMES THE CURSOR, as a VT100 does -- and 0 for a region of
// fewer than two rows, which is refused and changes nothing.
int ansi_region_set(struct ansi_region *r, int rows, uint16_t a, uint16_t b);

// Rows top..bot move by `n`: UP for n > 0 (the top |n| rows are lost),
// DOWN for n < 0 (the bottom ones are). The |n| rows uncovered at the
// other end become BLANK. 0 < |n| <= bot - top + 1.
struct ansi_shift { int top, bot, n; };

// What a line operation -- LINEFEED, REVERSE_INDEX, INSERT_LINES,
// DELETE_LINES, SCROLL_UP, SCROLL_DOWN -- does with the cursor at
// (*row, *col) and `count` as the parser resolved it. Moves the cursor,
// and returns 1 with `*s` filled when rows must move as well; 0 when
// the op is only a cursor move or does nothing (IL/DL outside the
// region, a line feed on the screen's last row below the region).
int ansi_region_op(const struct ansi_region *r, int rows, enum ansi_op op,
                   int count, int *row, int *col, struct ansi_shift *s);

// The VGA colour an ANSI colour index (0..7) names. Exposed because the
// mapping is the part worth asserting, and a caller writing escapes
// (userland) needs the inverse of what the parser does.
enum vga_color ansi_color(int index, int bright);

#endif // API_ANSI_H
