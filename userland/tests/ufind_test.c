// lib/ufind.h: the File Manager's search filters and its contents match.
// The contents cases put the phrase ACROSS a read's block boundary (8 KB),
// once on a short line and once in a line longer than the carried tail, so
// a search that only looked inside each block would miss both.
//
// Prints one line per check and exits with the number of failures.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include <caltime.h>
#include "lib/ufind.h"
#include "lib/utest.h"

static struct sys_dirent ent(const char *name, int is_dir, unsigned long size, long long when) {
    struct sys_dirent e;
    memset(&e, 0, sizeof e);
    snprintf(e.name, sizeof e.name, "%s", name);
    e.is_dir = is_dir;
    e.size = size;
    cal_epoch_to_rtc((uint64_t)when, &e.modified);
    return e;
}

static void put(const char *path, const char *data, int n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) { (void)write(fd, data, (size_t)n); close(fd); }
}

static char big[20000];

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    utest_begin("ufind_test", "lib/ufind: search filters and the contents match", 0);
    long long now = (long long)time(0);
    struct ufind_filter f = UFIND_FILTER_ANY;
    struct sys_dirent pic = ent("dusk.JPG", 0, 20000, now);
    struct sys_dirent txt = ent("notes.txt", 0, 0, now - 400LL * 86400);
    struct sys_dirent dir = ent("Pictures", 1, 0, now);

    utest_check(!ufind_filter_active(&f), "the empty filter narrows nothing");
    utest_check(ufind_matches(&f, &pic, now) && ufind_matches(&f, &dir, now), "...and keeps everything");

    f.kind = UFILETYPE_PICTURE;
    utest_check(ufind_filter_active(&f), "a type is a filter");
    utest_check(ufind_matches(&f, &pic, now), "Pictures keeps a .JPG, any case");
    utest_check(!ufind_matches(&f, &txt, now) && !ufind_matches(&f, &dir, now),
                "...and drops a text file and a folder");
    f.kind = UFILETYPE_FOLDER;
    utest_check(ufind_matches(&f, &dir, now) && !ufind_matches(&f, &pic, now), "Folders keeps only folders");
    f.kind = -1;

    f.size = UFIND_EMPTY;
    utest_check(ufind_matches(&f, &txt, now) && !ufind_matches(&f, &pic, now), "Empty keeps the 0-byte file");
    utest_check(!ufind_matches(&f, &dir, now), "...and no folder: a folder has no size");
    f.size = UFIND_SMALL;
    utest_check(ufind_matches(&f, &pic, now), "20000 bytes is Small (under 1 MB)");
    utest_check(ufind_size_band(16 * 1024 - 1) == UFIND_TINY && ufind_size_band(16 * 1024) == UFIND_SMALL,
                "the Tiny band ends at 16 KB");
    utest_check(ufind_size_band(128ull << 20) == UFIND_LARGE, "128 MB is Large");
    f.size = UFIND_ANY_SIZE;

    struct sys_dirent ahead = ent("ahead", 0, 1, now + 3600 * 30);
    struct sys_dirent month = ent("month", 0, 1, now - 40LL * 86400);
    f.age = UFIND_TODAY;
    utest_check(ufind_matches(&f, &pic, now), "Today keeps a file written now");
    utest_check(ufind_matches(&f, &ahead, now), "...and one dated ahead of the clock");
    utest_check(!ufind_matches(&f, &txt, now), "...and drops one from last year");
    f.age = UFIND_THIS_WEEK;
    utest_check(!ufind_matches(&f, &month, now), "This week drops 40 days ago");
    f.age = UFIND_THIS_MONTH;
    utest_check(!ufind_matches(&f, &month, now), "This month drops 40 days ago");
    f.age = UFIND_OLDER;
    utest_check(ufind_matches(&f, &txt, now) && !ufind_matches(&f, &pic, now),
                "Before this year keeps 400 days ago, and not today");
    utest_check(!strcmp(ufind_age_name(UFIND_THIS_WEEK), "This week") &&
                !strcmp(ufind_size_name(99), "Any size"), "the names, and a bad index answers 'any'");

    // --- contents ---------------------------------------------------------
    char line[80];
    int ln = 0;
    // Short lines, the phrase straddling byte 8192.
    int n = 0;
    while (n < 8190) n += snprintf(big + n, sizeof big - (size_t)n, "row %d of filler\n", n);
    n = 8180;   // "Wallpaper " is 10 bytes: DUSK spans 8190..8193
    big[n - 1] = '\n';
    n += snprintf(big + n, sizeof big - (size_t)n, "Wallpaper DUSK here\nlast line\n");
    int want_line = 1;
    for (int i = 0; i < 8180; i++) want_line += big[i] == '\n';
    put("/tmp/ufind_a.txt", big, n);
    int hit = ufind_contains("/tmp/ufind_a.txt", "dusk", &ln, line, sizeof line, 0);
    utest_check(hit, "a phrase across the block boundary is found, any case");
    utest_checkf(ln == want_line, "...on line %d (want %d)", ln, want_line);
    utest_checkf(!strcmp(line, "Wallpaper DUSK here"), "...with its line: '%s'", line);
    utest_check(!ufind_contains("/tmp/ufind_a.txt", "dawn", &ln, line, sizeof line, 0),
                "a phrase the file lacks is not");

    // One long line: the tail carried is the phrase's length, not the line.
    memset(big, 'x', 9000);
    memcpy(big + 8190, "needle", 6);
    put("/tmp/ufind_b.txt", big, 9000);
    hit = ufind_contains("/tmp/ufind_b.txt", "needle", &ln, line, sizeof line, 0);
    utest_check(hit && ln == 1, "a long line's phrase across the boundary is found, on line 1");
    utest_checkf(!strncmp(line, "...", 3) && strstr(line, "needle"), "...cut before it: '%s'", line);

    memcpy(big, "\0needle", 7);
    put("/tmp/ufind_c.bin", big, 100);
    utest_check(!ufind_contains("/tmp/ufind_c.bin", "needle", &ln, line, sizeof line, 0),
                "a file with a NUL in it is binary and not searched");
    volatile int stop = 1;
    utest_check(!ufind_contains("/tmp/ufind_a.txt", "dusk", &ln, line, sizeof line, &stop),
                "a raised cancel flag stops before reading");
    utest_check(!ufind_contains("/tmp/nonexistent_ufind", "x", &ln, line, sizeof line, 0),
                "a missing file is no match");
    unlink("/tmp/ufind_a.txt");
    unlink("/tmp/ufind_b.txt");
    unlink("/tmp/ufind_c.bin");
    return utest_end();
}
