// lib/ubase64.h against RFC 4648's own test vectors (section 10), an
// HTTP Basic credential worked out with Python's base64 module, and the
// refusal of a buffer one byte short -- which must write nothing.
#include <stdio.h>
#include <string.h>
#include "lib/ubase64.h"
#include "lib/utest.h"

static const struct { const char *in, *out; } CASES[] = {
    { "",       "" },
    { "f",      "Zg==" },
    { "fo",     "Zm8=" },
    { "foo",    "Zm9v" },
    { "foob",   "Zm9vYg==" },
    { "fooba",  "Zm9vYmE=" },
    { "foobar", "Zm9vYmFy" },
    { "ada:lovelace", "YWRhOmxvdmVsYWNl" },
};

int main(void) {
    utest_begin("base64_test", "lib/ubase64.h", UTEST_VERDICT_FILE);
    for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char out[32], what[64];
        int n = ubase64_encode(CASES[i].in, strlen(CASES[i].in), out, sizeof out);
        snprintf(what, sizeof what, "\"%s\" encodes as \"%s\"", CASES[i].in, CASES[i].out);
        utest_check_detail(n == (int)strlen(CASES[i].out) && !strcmp(out, CASES[i].out), what, out);
    }
    // Every byte value, so the alphabet's last characters are reached.
    unsigned char all[3] = { 0xfb, 0xff, 0xbf };
    char out[8];
    int n = ubase64_encode(all, 3, out, sizeof out);
    utest_check_detail(n == 4 && !strcmp(out, "+/+/"), "bytes fb ff bf encode as \"+/+/\"", out);
    memcpy(out, "xxxxxxx", 8);
    n = ubase64_encode("foo", 3, out, 4);   // 4 characters and no room for the NUL
    utest_check_detail(n == -1 && !strcmp(out, "xxxxxxx"), "a buffer one short is refused, untouched", out);
    return utest_end();
}
