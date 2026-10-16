// lib/usiteperm.h: the Web Browser's per-site choices, through its file.
//
// What a broken version would still pass, and what stops it here: a
// table that only lived in memory answers every get() right after its
// set() -- so each value is read back from a FRESH load of the file; a
// writer that dropped what it could not parse would still round-trip
// its own lines -- so a line of an unknown kind must come back
// byte-for-byte; and a Default that only stopped matching would still
// "forget" -- so the line must be gone from WV_PERMS' list too.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "lib/usiteperm.h"
#include "lib/utest.h"

#define FILE_PATH "/tmp/usiteperm_test"
#define FOREIGN   "example.org\tautoplay\tsometimes"

static struct usiteperm t;

static int has(const char *path, const char *line) {
    static char buf[1024];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, line) != NULL;
}

int main(void) {
    utest_begin("usiteperm_test", "per-site permissions, kept in a file", 0);
    char list[256];

    unlink(FILE_PATH);
    FILE *f = fopen(FILE_PATH, "w");
    if (f) { fprintf(f, "%s\n", FOREIGN); fclose(f); }

    utest_check(usiteperm_load(&t, FILE_PATH) == 0, "a file with a line it cannot read loads");
    utest_check(usiteperm_get(&t, "example.org", "javascript") == USITEPERM_DEFAULT,
                "a site with no choice is Default");
    utest_check(usiteperm_set(&t, "example.org", "javascript", USITEPERM_ALLOW) == 0 &&
                usiteperm_set(&t, "ads.example.net", "javascript", USITEPERM_BLOCK) == 0,
                "two choices are kept");
    usiteperm_free(&t);

    usiteperm_load(&t, FILE_PATH);
    utest_check(usiteperm_get(&t, "example.org", "javascript") == USITEPERM_ALLOW &&
                usiteperm_get(&t, "ads.example.net", "javascript") == USITEPERM_BLOCK,
                "...and read back from the file");
    utest_check(usiteperm_get(&t, "www.example.org", "javascript") == USITEPERM_DEFAULT,
                "a host is matched exactly: www. is another site");
    utest_check(has(FILE_PATH, FOREIGN), "the line of an unknown kind is written back as it was");

    int n = usiteperm_list(&t, "javascript", list, sizeof list);
    utest_checkf(n > 0 && !strcmp(list, "example.org\tallow\nads.example.net\tblock\n"),
                 "the list for one kind is its sites in order (%d: \"%s\")", n, list);
    utest_check(usiteperm_list(&t, "javascript", list, 10) == -1 && list[0] == '\0',
                "a list that does not fit writes nothing");

    utest_check(usiteperm_set(&t, "example.org", "javascript", USITEPERM_DEFAULT) == 0,
                "Default forgets a site's choice");
    usiteperm_free(&t);
    usiteperm_load(&t, FILE_PATH);
    usiteperm_list(&t, "javascript", list, sizeof list);
    utest_checkf(usiteperm_get(&t, "example.org", "javascript") == USITEPERM_DEFAULT &&
                 !strcmp(list, "ads.example.net\tblock\n"),
                 "...in the file, and in the list (\"%s\")", list);
    utest_check(usiteperm_set(&t, "", "javascript", USITEPERM_ALLOW) == -1,
                "a page with no host has no site to keep a choice for");
    usiteperm_free(&t);
    unlink(FILE_PATH);
    return utest_end();
}
