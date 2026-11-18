// The kernel build of kgrapheme.c at the cases that decide stepping; the
// full comparison with Perl's \X is tools/hostchecks/grapheme_hostcheck.py.
#include "ktest.h"
#include "kgrapheme.h"

// U+1F468 U+200D U+1F469 (a ZWJ pair), U+1F1EB U+1F1EE (a flag), "e" U+0301.
static const char FAMILY[] = "a\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9z";
static const char FLAGS[] = "\xf0\x9f\x87\xab\xf0\x9f\x87\xae\xf0\x9f\x87\xab";
static const char ACUTE[] = "e\xcc\x81x";

KTEST("kgrapheme", "a ZWJ sequence, a flag and a mark each step as one") {
    KTEST_ASSERT_EQ(kgrapheme_next(FAMILY, -1, 0), 1);
    KTEST_ASSERT_EQ(kgrapheme_next(FAMILY, -1, 1), 12);       // both people and the ZWJ
    KTEST_ASSERT_EQ(kgrapheme_prev(FAMILY, 12), 1);
    KTEST_ASSERT_EQ(kgrapheme_next(FLAGS, -1, 0), 8);         // indicators pair...
    KTEST_ASSERT_EQ(kgrapheme_next(FLAGS, -1, 8), 12);        // ...and the odd one stands alone
    KTEST_ASSERT_EQ(kgrapheme_prev(FLAGS, 12), 8);
    KTEST_ASSERT_EQ(kgrapheme_next(ACUTE, -1, 0), 3);
    KTEST_ASSERT_EQ(kgrapheme_prev(ACUTE, 3), 0);
}

KTEST("kgrapheme", "a line break is its own character, \\r\\n two") {
    KTEST_ASSERT_EQ(kgrapheme_next("\r\n", -1, 0), 1);
    KTEST_ASSERT_EQ(kgrapheme_next("\n\xcc\x81", -1, 0), 1);  // a mark after a break starts a cluster
}

KTEST("kgrapheme", "backspace takes an emoji whole and a mark alone") {
    KTEST_ASSERT_EQ(kgrapheme_backspace(FAMILY, 12), 1);
    KTEST_ASSERT_EQ(kgrapheme_backspace(ACUTE, 3), 1);
    KTEST_ASSERT_EQ(kgrapheme_backspace(FLAGS, 8), 0);
}
