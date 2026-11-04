// base/ufileop.h's tree walks and move-with-replace on real files under
// /home, read back through sys_stat -- plus the name checks the File
// Manager leans on: ufileop_name_ok(), uarchive_name_safe() (the
// zip-slip refusal) and ufind_name_has() (the search box's match).
//
// THE TREE HAS MORE FOLDERS THAN THE WALK'S OLD FIXED QUEUE HELD (64,
// until 2026-11-04): a copy stopped at the 64th folder and a delete left
// the rest behind, each reported only as "FAILED".
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"
#include "base/ufileop.h"
#include "base/uarchive.h"
#include "base/ufind.h"

#define DIR "/home/ufo-test"
enum { DIRS = 100 };

static struct ufileop g_s;   // static: tens of KB

static int exists(const char *p) {
    struct sys_stat st;
    return sys_stat(p, &st) == 0;
}

static void make(const char *p, const char *text) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, text, strlen(text)); close(fd); }
}

static int holds(const char *p, const char *text) {
    char buf[32] = "";
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    return n == (long)strlen(text) && !memcmp(buf, text, (size_t)n);
}

static int count_files(const char *root) {
    int n = 0;
    for (int i = 0; i < DIRS; i++) {
        char p[96];
        snprintf(p, sizeof p, "%s/d%03d/f.txt", root, i);
        n += exists(p);
    }
    return n;
}

int main(void) {
    utest_begin("ufileop_test", "tree walks past 64 folders, move-with-replace, name checks",
                UTEST_VERDICT_FILE);
    ufileop_remove(DIR, 1, &g_s, 0);
    sys_mkdir(DIR);
    sys_mkdir(DIR "/src");
    for (int i = 0; i < DIRS; i++) {
        char p[96];
        snprintf(p, sizeof p, DIR "/src/d%03d", i);
        sys_mkdir(p);
        snprintf(p, sizeof p, DIR "/src/d%03d/f.txt", i);
        make(p, "x");
    }

    int r = ufileop_copy(DIR "/src", DIR "/dst", &g_s, 0);
    int n = count_files(DIR "/dst");
    utest_checkf(r == UFILEOP_OK && n == DIRS, "a tree of %d folders copies whole (r=%d, %d files)",
                 DIRS, r, n);
    r = ufileop_remove(DIR "/dst", 1, &g_s, 0);
    utest_checkf(r == UFILEOP_OK && !exists(DIR "/dst"), "...and removes whole (r=%d)", r);

    // REPLACE ON A MOVE: the old file is set aside, not deleted first,
    // and nothing of it is left once the move has landed.
    make(DIR "/a.txt", "new");
    make(DIR "/b.txt", "old");
    struct ufileop_policy replace = { 0 };   // no on_conflict: OVERWRITE
    r = ufileop_move(DIR "/a.txt", DIR "/b.txt", &g_s, &replace);
    utest_checkf(r == UFILEOP_OK && holds(DIR "/b.txt", "new") && !exists(DIR "/a.txt"),
                 "a move onto a file replaces it (r=%d)", r);
    utest_check(!exists(DIR "/.b.txt.replaced"), "...and leaves no set-aside copy behind");

    // A move of a FOLDER tree onto a new name (rename, same parent).
    r = ufileop_move(DIR "/src", DIR "/moved", &g_s, 0);
    n = count_files(DIR "/moved");
    utest_checkf(r == UFILEOP_OK && n == DIRS && !exists(DIR "/src"),
                 "a tree of %d folders moves whole (r=%d, %d files)", DIRS, r, n);

    static const struct { const char *name; int ok; } names[] = {
        { "a.txt", 1 }, { ".hidden", 1 }, { "a..b", 1 }, { "", 0 }, { ".", 0 },
        { "..", 0 }, { "../x", 0 }, { "sub/x", 0 }, { "x/", 0 },
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        utest_checkf(ufileop_name_ok(names[i].name) == names[i].ok, "name_ok(\"%s\") is %d",
                     names[i].name, names[i].ok);

    static const struct { const char *name; int ok; } members[] = {
        { "a/b.txt", 1 }, { "dir/", 1 }, { "a..b/c", 1 }, { "..x", 1 }, { "../evil", 0 },
        { "a/../../evil", 0 }, { "/etc/passwd", 0 }, { "a/..", 0 }, { "..", 0 },
    };
    for (unsigned i = 0; i < sizeof members / sizeof members[0]; i++)
        utest_checkf(uarchive_name_safe(members[i].name) == members[i].ok,
                     "uarchive_name_safe(\"%s\") is %d", members[i].name, members[i].ok);

    static const struct { const char *name, *query; int has; } finds[] = {
        { "Apple.txt", "ppl", 1 }, { "Apple.txt", "APPLE", 1 }, { "apple", "apx", 0 },
        { "x", "", 1 }, { "", "a", 0 },
        { "\xc3\xa4rzte.txt", "\xc3\x84", 1 },                 // "ärzte" finds "Ä"
        { "\xc3\x84RZTE", "\xc3\xa4rz", 1 },                   // ...and the other way
        { "caf\xc3\xa9", "CAF\xc3\x89", 1 },                   // é / É
        { "\xc5\x81\xc3\xb3" "d\xc5\xba", "\xc5\x82\xc3\x93" "D\xc5\xb9", 1 },   // Łódź / łÓDŹ
        { "\xce\x91\xce\xb8\xce\xae\xce\xbd\xce\xb1", "\xce\xb1\xce\x98", 1 },  // Greek
        { "\xd0\x9c\xd0\xbe\xd1\x81\xd0\xba\xd0\xb2\xd0\xb0", "\xd0\xbc\xd0\x9e", 1 },  // Cyrillic
        { "\xc3\xa4", "a", 0 },                                // a diaeresis is not folded away
    };
    for (unsigned i = 0; i < sizeof finds / sizeof finds[0]; i++)
        utest_checkf(ufind_name_has(finds[i].name, finds[i].query) == finds[i].has,
                     "ufind_name_has #%u is %d", i, finds[i].has);

    ufileop_remove(DIR, 1, &g_s, 0);
    return utest_end();
}
