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
//   - The answered list read back by its own reader proves nothing about
//     the file an install writes onto another disk: it is written under a
//     scratch root and its bytes compared, and re-arming must remove the
//     machine's own.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lib/ufbs.h"
#include "lib/ubootcfg.h"
#include "lib/ufile.h"
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

    // The installer's word goes through the same whole-word reader.
    utest_check(ubootcfg_onoff_in("live installer=on", "installer=") == 1, "installer=on");
    utest_check(ubootcfg_onoff_in("installer=off video=1x1", "installer=") == 2, "installer=off");
    utest_check(ubootcfg_onoff_in("noinstaller=on installer=maybe", "installer=") == 0,
                "installer= only as a whole word, and only on or off");

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

    // The pages an install carried: written under another root, as onto
    // the target's /mnt, then this machine's own.
    unsigned two = (1u << UFBS_PAGE_REGION) | (1u << UFBS_PAGE_NAME);
    mkdir("/tmp/fbs_root", 0755);
    utest_check(ufbs_write_answered("/tmp/fbs_root", two) == 0, "the list is written under a root");
    uint8_t *text = 0;
    size_t len = 0;
    int read_ok = ufile_slurp("/tmp/fbs_root" UFBS_ANSWERED, 0, &text, &len) == UFILE_OK;
    utest_check(read_ok && len == 12 && !memcmp(text, "region\nname\n", 12),
                "...one page id a line, in page order");
    free(text);
    utest_check(ufbs_write_answered("/tmp/fbs_root/none", 0) == 0 && stat("/tmp/fbs_root/none" UFBS_ANSWERED, &(struct stat){0}) != 0,
                "no pages: no file at all");
    utest_check(!strcmp(ufbs_page_id(UFBS_PAGE_KEYBOARD), "keyboard") &&
                !strcmp(ufbs_page_title(UFBS_PAGE_TIME), "Time zone"), "page ids and titles");

    // This machine's own list, put back as it was after.
    unsigned before = ufbs_answered();
    utest_check(ufbs_write_answered("", two) == 0 && ufbs_answered() == two, "read back as the same two pages");
    utest_check(ufbs_rearm() == 0 && ufbs_answered() == 0, "re-arming removes the list too");
    if (had) ufbs_mark(strncmp(was, "skipped", 7) ? "finished" : "skipped");
    if (before) ufbs_write_answered("", before);

    return utest_end();
}
