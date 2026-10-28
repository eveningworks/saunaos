// See klineedit_cases.h. The cases are deliberately the bash-fidelity
// details rather than the easy ones -- the places a reimplementation
// (or a second compilation that somehow differed) would go subtly
// wrong: the two different word definitions, a kill ring that yanks
// back what it took, an undo stack, and Ctrl-D meaning two things
// depending on where the cursor is.
#include "klineedit_cases.h"
#include "klineedit.h"
#include "keyboard.h"
#include "string.h"

#define CTRL(c) ((c) - 'a' + 1)
#define ESC 0x1B

// Each array is the literal key stream a front end would feed in, in
// the encoding keyboard.h defines: ASCII for printables and control
// codes, KEY_* for specials, and ESC-then-key for a Meta binding.
static const int k_insert_mid[]  = { 'h','e','l','o', KEY_ARROW_LEFT, 'l' };
static const int k_home_end[]    = { 'a','b','c', KEY_HOME, 'x', KEY_END, 'y' };
static const int k_ctrl_a_e[]    = { 'a','b','c', CTRL('a'), 'x', CTRL('e'), 'y' };
static const int k_kill_yank[]   = { 'o','n','e',' ','t','w','o', CTRL('u'), CTRL('y') };
static const int k_ctrl_w[]      = { '/','b','i','n','/','l','s', CTRL('w') };
static const int k_meta_bksp[]   = { '/','b','i','n','/','l','s', ESC, 0x7F };
static const int k_meta_b[]      = { 'o','n','e',' ','t','w','o', ESC, 'b', 'X' };
static const int k_meta_d[]      = { 'o','n','e',' ','t','w','o', ESC, 'b', ESC, 'd' };
static const int k_undo[]        = { 'a','b','c', CTRL('u'), CTRL('x'), CTRL('u') };
static const int k_delete[]      = { 'a','b','c', KEY_HOME, KEY_DELETE };
static const int k_ctrl_k[]      = { 'a','b','c','d', KEY_HOME, KEY_ARROW_RIGHT, CTRL('k') };
static const int k_ctrl_d_mid[]  = { 'a','b','c', KEY_HOME, CTRL('d') };
static const int k_transpose[]   = { 'a','b', CTRL('t') };
// Alt-T, transpose WORDS -- a different code path from Ctrl-T, and one
// that had no case at all until it was rewritten as an in-place rotate.
static const int k_transpose_w[] = { 'o','n','e',' ','t','w','o', ESC, 't' };

// UTF-8 (docs/utf8-design.md, step 2): a typed key is a CODEPOINT and
// the line holds its UTF-8, so every edit below moves a whole CHARACTER.
// a-umlaut U+00E4 is C3 A4, a-ogonek U+0105 C4 85, z-dot U+017C C5 BC.
// The wanted strings are spelled in bytes so this file stays ASCII.
static const int k_u8_insert[]   = { 'K','e','s', 0xE4 };
static const int k_u8_bksp[]     = { 'a', 0x105, 0x7F };
static const int k_u8_left[]     = { 'a', 0x105, KEY_ARROW_LEFT, 'x' };
static const int k_u8_delete[]   = { 0x105, 'b', KEY_HOME, KEY_DELETE };
static const int k_u8_transpose[] = { 'a', 0x17C, CTRL('t') };
static const int k_u8_word[]     = { 'o','n','e',' ', 0xE4, 0x105, 'b', ESC, 'b', 'X' };
static const int k_u8_meta_bksp[] = { 'c','d',' ', 'k', 0xE4, 0x17C, ESC, 0x7F };
static const int k_u8_upcase[]   = { 0x105, 0xE4, 'b', KEY_HOME, ESC, 'u' };

#define CASE(n, arr, want_s, want_c) \
    { n, arr, (int)(sizeof(arr) / sizeof((arr)[0])), want_s, want_c }

const struct kline_case kline_cases[] = {
    CASE("insert at the cursor",            k_insert_mid, "hello",    4),
    CASE("Home/End",                        k_home_end,   "xabcy",    5),
    CASE("Ctrl-A / Ctrl-E",                 k_ctrl_a_e,   "xabcy",    5),
    CASE("Ctrl-U kills backwards, Ctrl-Y yanks it back", k_kill_yank, "one two", 7),
    // The two word definitions, which is the pair most worth pinning:
    // Ctrl-W is whitespace-delimited so it takes the whole path, while
    // Alt-Backspace is alphanumeric so it takes only "ls".
    CASE("Ctrl-W takes the whole path",     k_ctrl_w,     "",         0),
    CASE("Alt-Backspace takes one word",    k_meta_bksp,  "/bin/",    5),
    CASE("Alt-B lands at the word start",   k_meta_b,     "one Xtwo", 5),
    CASE("Alt-D kills the word forward",    k_meta_d,     "one ",     4),
    CASE("Ctrl-X Ctrl-U undoes the kill",   k_undo,       "abc",      3),
    CASE("Delete removes forwards",         k_delete,     "bc",       0),
    CASE("Ctrl-K kills to the end",         k_ctrl_k,     "a",        1),
    // Ctrl-D on a NON-empty line is delete-forward; on an empty one it
    // is EOF, which is an action code rather than a buffer change and
    // so is asserted in klineedit_test.c instead.
    CASE("Ctrl-D mid-line deletes forward", k_ctrl_d_mid, "bc",       0),
    CASE("Ctrl-T transposes",               k_transpose,  "ba",       2),
    CASE("Alt-T transposes words",          k_transpose_w, "two one", 7),
    CASE("a codepoint is inserted as UTF-8",  k_u8_insert, "Kes\xC3\xA4", 5),
    CASE("Backspace takes a whole character", k_u8_bksp,   "a",        1),
    CASE("Left steps over a whole character", k_u8_left,   "ax\xC4\x85", 2),
    CASE("Delete takes a whole character",    k_u8_delete, "b",        0),
    CASE("Ctrl-T moves a whole character",    k_u8_transpose, "\xC5\xBC" "a", 3),
    CASE("Alt-B: letters past ASCII are one word", k_u8_word, "one X\xC3\xA4\xC4\x85" "b", 5),
    CASE("Alt-Backspace kills a UTF-8 word",  k_u8_meta_bksp, "cd ",    3),
    CASE("Alt-U upcases past ASCII",          k_u8_upcase, "\xC4\x84\xC3\x84" "B", 5),
};

const int kline_case_count = (int)(sizeof kline_cases / sizeof kline_cases[0]);

int kline_case_run(const struct kline_case *c, struct kline_edit *e,
                   char *got, int cap, int *got_cursor,
                   const struct kline_mem *mem) {
    // Freed then re-initialised, so a case that grew the line does not
    // hand its memory to the next one. The editor may be uninitialised
    // on the first call, which is why the caller passes `mem` rather
    // than this reading e->mem out of whatever was on the stack.
    if (e->cap) kline_free(e);
    kline_init_mem(e, mem);
    for (int i = 0; i < c->nkeys; i++) kline_key(e, c->keys[i]);

    k_strlcpy(got, e->buf, (size_t)cap);
    *got_cursor = e->cursor;
    return k_strcmp(e->buf, c->want) == 0 && e->cursor == c->want_cursor;
}
