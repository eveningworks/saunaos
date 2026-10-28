// Tests for api/kutf8.h's whole-string helpers: validity, the Latin-1
// conversion utf8migrate makes once, and the UTF-16 converters exFAT,
// FAT32 and SMB share.
#include "ktest.h"
#include "kutf8.h"
#include "string.h"

KTEST("kutf8", "valid means every byte belongs to an RFC 3629 sequence") {
    KTEST_ASSERT(kutf8_valid("plain", -1));
    KTEST_ASSERT(kutf8_valid("K\xc3\xa4s", -1));
    KTEST_ASSERT(kutf8_valid("\xf0\x9f\x8d\xb5", -1));
    KTEST_ASSERT(!kutf8_valid("K\xe4s", -1));           // Latin-1
    KTEST_ASSERT(!kutf8_valid("\xc0\x80", -1));         // overlong NUL
    KTEST_ASSERT(!kutf8_valid("\xed\xa0\x80", -1));     // a surrogate
    KTEST_ASSERT(!kutf8_valid("\xc3", -1));             // cut short
    KTEST_ASSERT(kutf8_valid("ab\xe4", 2));             // counted: the stray is past n
}

KTEST("kutf8", "from_latin1 keeps valid sequences and converts the strays") {
    char out[32];
    KTEST_ASSERT_EQ(kutf8_from_latin1("K\xe4s", -1, out, sizeof out), 4);
    KTEST_ASSERT(!k_strcmp(out, "K\xc3\xa4s"));
    // Mixed: what the transition rule DREW is what comes out.
    KTEST_ASSERT(kutf8_from_latin1("\xc3\xa9\xe4", -1, out, sizeof out) == 4);
    KTEST_ASSERT(!k_strcmp(out, "\xc3\xa9\xc3\xa4"));
    // Already valid: unchanged.
    KTEST_ASSERT(kutf8_from_latin1("K\xc3\xa4s", -1, out, sizeof out) == 4);
    KTEST_ASSERT(!k_strcmp(out, "K\xc3\xa4s"));
    // No room is -1, never a cut name.
    char small[4];
    KTEST_ASSERT_EQ(kutf8_from_latin1("K\xe4s", -1, small, sizeof small), -1);
}

KTEST("kutf8", "UTF-16LE both ways: pairs whole, an unpaired surrogate is U+FFFD") {
    uint8_t u[16];
    // U+00E4 then U+1F375 (a pair), little-endian.
    KTEST_ASSERT_EQ(kutf8_to_utf16le("\xc3\xa4\xf0\x9f\x8d\xb5", -1, u, 8), 3);
    static const uint8_t want[] = { 0xe4, 0, 0x3c, 0xd8, 0x75, 0xdf };
    KTEST_ASSERT(!k_memcmp(u, want, sizeof want));
    char out[16];
    KTEST_ASSERT_EQ(kutf8_from_utf16le(u, 3, out, sizeof out), 6);
    KTEST_ASSERT(!k_strcmp(out, "\xc3\xa4\xf0\x9f\x8d\xb5"));
    // Strict going out: Latin-1 is refused, and so is a name that does not fit.
    KTEST_ASSERT_EQ(kutf8_to_utf16le("K\xe4s", -1, u, 8), -1);
    KTEST_ASSERT_EQ(kutf8_to_utf16le("\xf0\x9f\x8d\xb5", -1, u, 1), -1);
    // A lone high surrogate, then 'a'.
    static const uint8_t lone[] = { 0x3c, 0xd8, 'a', 0 };
    KTEST_ASSERT_EQ(kutf8_from_utf16le(lone, 2, out, sizeof out), 4);
    KTEST_ASSERT(!k_strcmp(out, "\xef\xbf\xbd" "a"));
    // Too small: stops at a whole character, NUL kept, -1.
    KTEST_ASSERT_EQ(kutf8_from_utf16le(want, 3, out, 4), -1);
    KTEST_ASSERT(!k_strcmp(out, "\xc3\xa4"));
}
