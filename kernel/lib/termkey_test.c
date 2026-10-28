// termkey in ring 0. The same cases run in ring 3 as
// /tests/termkey_test, because the encoder is shared source compiled
// twice and these tests would pass whether or not ring 3 could link a
// byte of it -- the arrangement klineedit_cases.h already has.
#include "ktest.h"
#include "termkey.h"
#include "termkey_cases.h"
#include "string.h"

KTEST("termkey", "every key encodes to the sequence a real terminal sends") {
    for (int i = 0; i < TERMKEY_CASE_COUNT; i++) {
        char buf[TERMKEY_MAX + 1];
        int n = termkey_encode(TERMKEY_CASES[i].key, buf, TERMKEY_MAX);
        buf[n] = 0;
        KTEST_ASSERT_EQ(k_strcmp(buf, TERMKEY_CASES[i].seq), 0);
    }
}

KTEST("termkey", "and decodes back to the key it came from") {
    for (int i = 0; i < TERMKEY_CASE_COUNT; i++) {
        struct termkey_state st;
        k_memset(&st, 0, sizeof st);
        const char *s = TERMKEY_CASES[i].seq;
        int got = TERMKEY_MORE;
        for (int j = 0; s[j]; j++) got = termkey_feed(&st, (unsigned char)s[j]);
        KTEST_ASSERT_EQ(got, TERMKEY_CASES[i].key);
    }
}

KTEST("termkey", "an ordinary character passes through untouched") {
    struct termkey_state st;
    k_memset(&st, 0, sizeof st);
    // Including the control codes: Ctrl-A really IS 0x01 on a terminal,
    // which is why they need no table row.
    KTEST_ASSERT_EQ(termkey_feed(&st, 'a'), 'a');
    KTEST_ASSERT_EQ(termkey_feed(&st, 0x01), 0x01);
    KTEST_ASSERT_EQ(termkey_feed(&st, '\n'), '\n');
    char buf[TERMKEY_MAX];
    KTEST_ASSERT_EQ(termkey_encode('a', buf, sizeof buf), 1);
    KTEST_ASSERT_EQ(buf[0], 'a');
}

// **THE ESC AMBIGUITY, which is the whole difficulty of reading a
// terminal.** A lone ESC is the Esc key or a Meta prefix; ESC [ and
// ESC O begin a sequence. Resolving on the NEXT byte rather than a
// timer is why Alt-<key> still works here.
KTEST("termkey", "a lone ESC is given back, and Alt-<key> survives") {
    struct termkey_state st;
    k_memset(&st, 0, sizeof st);
    KTEST_ASSERT_EQ(termkey_feed(&st, 0x1B), TERMKEY_MORE);
    // 'b' does not begin a sequence, so the ESC comes back and the
    // caller re-feeds the 'b' -- which is Alt-B to a line editor.
    KTEST_ASSERT_EQ(termkey_feed(&st, 'b'), 0x1B);
    KTEST_ASSERT_EQ(termkey_feed(&st, 'b'), 'b');
}

KTEST("termkey", "an unknown sequence is refused, not guessed at") {
    struct termkey_state st;
    k_memset(&st, 0, sizeof st);
    KTEST_ASSERT_EQ(termkey_feed(&st, 0x1B), TERMKEY_MORE);
    KTEST_ASSERT_EQ(termkey_feed(&st, '['), TERMKEY_MORE);
    // No CSI Z here, and inventing one would be worse than saying no.
    KTEST_ASSERT_EQ(termkey_feed(&st, 'Z'), TERMKEY_NONE);
    // And the state is clean for the next key.
    KTEST_ASSERT_EQ(termkey_feed(&st, 'x'), 'x');
}

KTEST("termkey", "a key with no terminal sequence encodes to nothing") {
    char buf[TERMKEY_MAX];
    // Super toggles the Start menu; it is a desktop key and a terminal
    // has never had a sequence for it.
    KTEST_ASSERT_EQ(termkey_encode(KEY_SUPER, buf, sizeof buf), 0);
}

KTEST("termkey", "a sequence longer than the buffer is refused, not cut") {
    char buf[2];
    // `CSI 1;2 D` is six bytes and must not be half-written: half a
    // sequence is a DIFFERENT key to whatever reads it.
    KTEST_ASSERT_EQ(termkey_encode(KEY_SHIFT_ARROW_LEFT, buf, 2), 0);
}

// UTF-8 (docs/utf8-design.md, step 2): what crosses a terminal for a
// character past ASCII is its UTF-8, and it comes back as ONE key.
KTEST("termkey", "a codepoint encodes to its UTF-8 and decodes back as one key") {
    static const int cps[] = { 0xE4, 0x105, 0x219, 0x20AC, 0x1F600 };
    for (unsigned i = 0; i < sizeof cps / sizeof cps[0]; i++) {
        char buf[TERMKEY_MAX];
        int n = termkey_encode(cps[i], buf, sizeof buf);
        KTEST_ASSERT(n >= 2 && n <= 4);
        struct termkey_state st;
        k_memset(&st, 0, sizeof st);
        int got = TERMKEY_NONE;
        for (int j = 0; j < n; j++) {
            got = termkey_feed(&st, (unsigned char)buf[j]);
            if (j < n - 1) KTEST_ASSERT_EQ(got, TERMKEY_MORE);
        }
        KTEST_ASSERT_EQ(got, cps[i]);
    }
    char buf[TERMKEY_MAX];
    KTEST_ASSERT_EQ(termkey_encode(0xE4, buf, sizeof buf), 2);
    KTEST_ASSERT_EQ((unsigned char)buf[0], 0xC3);
    KTEST_ASSERT_EQ((unsigned char)buf[1], 0xA4);
}

// THE TRANSITION RULE: a Latin-1 byte that starts no valid sequence is
// its own letter, and the byte that broke a sequence is fed again.
KTEST("termkey", "a broken UTF-8 sequence gives its lead back and re-feeds the byte") {
    struct termkey_state st;
    k_memset(&st, 0, sizeof st);
    KTEST_ASSERT_EQ(termkey_feed(&st, 0xE4), TERMKEY_MORE);   // could lead 3 bytes
    KTEST_ASSERT_EQ(termkey_feed(&st, 's'), 0xE4);            // it did not
    KTEST_ASSERT_EQ(termkey_refeed(&st), 1);
    KTEST_ASSERT_EQ(termkey_refeed(&st), 0);                  // read once
    KTEST_ASSERT_EQ(termkey_feed(&st, 's'), 's');
    // A byte that can lead nothing is a character at once.
    KTEST_ASSERT_EQ(termkey_feed(&st, 0xA4), 0xA4);
    KTEST_ASSERT_EQ(termkey_refeed(&st), 0);
}
