// SYS_GETDENTS and what lists through it: a name longer than 63 bytes
// reaches ring 3 WHOLE, through readdir(), ulistdir() and uwalk, and a
// directory too big for one page resumes from each record's d_off.
//
// The names are made here with open(O_CREAT), a different path from the
// listing under test, and compared byte for byte -- "a long name came
// back" is what a listing that cut every name to 63 bytes would still
// claim for a name of 63.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/ulistdir.h"
#include "base/uwalk.h"
#include "base/utest.h"

#define DIR_LONG "/var/tmp/gd.long"
#define DIR_MANY "/var/tmp/gd.many"
#define MANY 300   // records of ~230 bytes: about nine 8 KiB pages

static char g_200[201], g_255[256];
static char g_path[512];

// A name of `len` bytes, distinct by `tag`, every byte a letter so a cut
// anywhere shows.
static void make_name(char *out, int len, int tag) {
    for (int i = 0; i < len; i++) out[i] = (char)('a' + (i + tag) % 26);
    out[0] = (char)('A' + tag % 26);
    out[len] = '\0';
}

static int touch(const char *dir, const char *name) {
    snprintf(g_path, sizeof g_path, "%s/%s", dir, name);
    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    write(fd, "x", 1);
    close(fd);
    return 1;
}

static void cleanup(void) {
    unlink(DIR_LONG "/short");
    snprintf(g_path, sizeof g_path, "%s/%s", DIR_LONG, g_200); unlink(g_path);
    snprintf(g_path, sizeof g_path, "%s/%s", DIR_LONG, g_255); unlink(g_path);
    unlink(DIR_LONG);
    char name[201];
    for (int i = 0; i < MANY; i++) {
        make_name(name, 200, i);
        snprintf(name + 190, 11, "%05d", i);
        snprintf(g_path, sizeof g_path, "%s/%s", DIR_MANY, name);
        unlink(g_path);
    }
    unlink(DIR_MANY);
}

static int g_walk_200, g_walk_255;
static int walk_fn(void *ctx, const char *path, const struct ulist_entry *e) {
    (void)ctx; (void)path;
    if (!strcmp(e->name, g_200)) g_walk_200 = 1;
    if (!strcmp(e->name, g_255)) g_walk_255 = 1;
    return 1;
}

int main(void) {
    utest_begin("getdents_test", "long names and paged listings through SYS_GETDENTS", UTEST_VERDICT_FILE);
    make_name(g_200, 200, 1);
    make_name(g_255, 255, 2);
    cleanup();

    mkdir(DIR_LONG, 0755);
    int made = touch(DIR_LONG, "short") && touch(DIR_LONG, g_200) && touch(DIR_LONG, g_255);
    utest_check(made, "a 200-byte and a 255-byte name can be created");

    // --- readdir() -----------------------------------------------------
    int rd_200 = 0, rd_255 = 0, rd_n = 0;
    DIR *d = opendir(DIR_LONG);
    for (struct dirent *e; d && (e = readdir(d)); rd_n++) {
        if (!strcmp(e->d_name, g_200)) rd_200 = 1;
        if (!strcmp(e->d_name, g_255)) rd_255 = 1;
    }
    if (d) closedir(d);
    utest_check(rd_n == 3, "readdir() lists all three");
    utest_check(rd_200 && rd_255, "and returns the 200- and 255-byte names whole");

    // --- ulistdir() and uwalk ------------------------------------------
    struct ulist_entry *v;
    int n = ulistdir(DIR_LONG, &v), ul_200 = 0, ul_255 = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(v[i].name, g_200)) ul_200 = 1;
        if (!strcmp(v[i].name, g_255)) ul_255 = 1;
    }
    if (n >= 0) free(v);
    utest_check(n == 3 && ul_200 && ul_255, "ulistdir() returns them whole too");

    static struct uwalk w;   // several KB
    if (uwalk_begin(&w, DIR_LONG))
        while (uwalk_step(&w, walk_fn, 0)) {}
    uwalk_end(&w);
    utest_check(g_walk_200 && g_walk_255, "and a uwalk visits them by their whole names");

    // --- the syscall's edges -------------------------------------------
    char tiny[24];
    int r = sys_getdents(DIR_LONG, tiny, sizeof tiny, 0);
    utest_check(r == -1 && errno == EINVAL, "a buffer too small for one record is EINVAL");
    static char page[4096];
    r = sys_getdents(DIR_LONG, page, sizeof page, 3);
    utest_check(r == 0, "and a start past the last entry is 0, the end");
    r = sys_getdents("/var/tmp/gd.none", page, sizeof page, 0);
    utest_check(r == -1 && errno == ENOENT, "a missing directory is ENOENT");
    snprintf(g_path, sizeof g_path, "%s/short", DIR_LONG);
    r = sys_getdents(g_path, page, sizeof page, 0);
    utest_check(r == -1 && errno == ENOTDIR, "a file is ENOTDIR");

    // --- a directory of many pages -------------------------------------
    mkdir(DIR_MANY, 0755);
    char name[201];
    int all = 1;
    for (int i = 0; i < MANY && all; i++) {
        make_name(name, 200, i);
        snprintf(name + 190, 11, "%05d", i);   // 195 bytes, unique by index
        all = touch(DIR_MANY, name);
    }
    utest_check(all, "300 files with 195-byte names are created");

    static unsigned char seen[MANY];
    memset(seen, 0, sizeof seen);
    n = ulistdir(DIR_MANY, &v);
    int good = n == MANY;
    for (int i = 0; i < n; i++) {
        int idx = atoi(v[i].name + 190);
        if (strlen(v[i].name) != 195 || idx < 0 || idx >= MANY || seen[idx]++) good = 0;
    }
    if (n >= 0) free(v);
    char detail[64];
    snprintf(detail, sizeof detail, "%d listed", n);
    utest_check_detail(good, "ulistdir() reads every one, once, across pages", detail);

    int rn = 0;
    d = opendir(DIR_MANY);
    for (struct dirent *e; d && (e = readdir(d)); ) if (strlen(e->d_name) == 195) rn++;
    if (d) closedir(d);
    snprintf(detail, sizeof detail, "%d read", rn);
    utest_check_detail(rn == MANY, "and so does readdir()", detail);

    cleanup();
    return utest_end();
}
