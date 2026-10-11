// First-boot setup's decisions (lib/ufbs.h) and the computer name's rule
// (lib/uhostname.h) -- the parts a GUI test cannot reach cheaply.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, which is what shaped this:
//
//   - A strstr() for "fbs=on" would pass `fbs=on` and also take `xfbs=on`
//     and `fbs=onward`; both are here, refused.
//   - A parser that took the FIRST word would pass every single-word line;
//     a line saying fbs=on then fbs=off must end off, as a later word wins.
//   - A name check that only looked at the first character would pass
//     "toy-os"; a trailing hyphen, a space, 64 characters and "" must fail.
//   - Mark and re-arm checked only through ufbs_done() prove the library
//     agrees with itself; the marker's FILE is checked directly too.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "lib/ufbs.h"
#include "lib/uhostname.h"
#include "lib/utest.h"

static int marker_exists(void) {
    struct stat st;
    return stat(UFBS_MARKER, &st) == 0;
}

int main(void) {
    utest_begin("fbs_test", "First-boot setup's boot words and marker, the computer name rule", 0);

    utest_check(ufbs_word_in("") == UFBS_WORD_NONE, "no word, no override");
    utest_check(ufbs_word_in("bootpart=x fbs=on debugcon") == UFBS_WORD_ON, "fbs=on mid-line");
    utest_check(ufbs_word_in("fbs=off") == UFBS_WORD_OFF, "fbs=off as the whole line");
    utest_check(ufbs_word_in("xfbs=on nofbs=off") == UFBS_WORD_NONE, "only a WHOLE word counts");
    utest_check(ufbs_word_in("fbs=onward fbs=of") == UFBS_WORD_NONE, "a value is on or off exactly");
    utest_check(ufbs_word_in("fbs=on fbs=off") == UFBS_WORD_OFF, "the later word wins");

    utest_check(uhostname_valid("toy-os") && uhostname_valid("A1") && uhostname_valid("x"),
                "ordinary names are valid");
    utest_check(!uhostname_valid("") && !uhostname_valid("-a") && !uhostname_valid("a-") &&
                !uhostname_valid("my pc") && !uhostname_valid("a.b") && !uhostname_valid("ä"),
                "empty, edge hyphens, a space, a dot and non-ASCII are refused");
    char longest[UHOSTNAME_MAX + 2];
    memset(longest, 'a', sizeof longest);
    longest[UHOSTNAME_MAX] = 0;
    utest_check(uhostname_valid(longest), "63 characters is the limit...");
    longest[UHOSTNAME_MAX] = 'a';
    longest[UHOSTNAME_MAX + 1] = 0;
    utest_check(!uhostname_valid(longest), "...and 64 is over it");

    // The machine's own marker, kept and put back: this runs on a real image.
    char was[64];
    int had = ufbs_done(was, sizeof was);
    utest_check(ufbs_mark("finished") == 0 && marker_exists(), "ufbs_mark writes the file");
    char how[64];
    utest_check(ufbs_done(how, sizeof how) && !strncmp(how, "finished ", 9),
                "its line says how it ended, then when");
    utest_check(ufbs_rearm() == 0 && !marker_exists() && !ufbs_done(0, 0), "ufbs_rearm removes it");
    utest_check(ufbs_rearm() == 0, "re-arming an unmarked machine is not an error");
    if (had) ufbs_mark(strncmp(was, "skipped", 7) ? "finished" : "skipped");

    return utest_end();
}
