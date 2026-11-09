// base/ucomponent: the optional components' definitions and Remove, in ring 3.
//
// Against definitions written here, in a directory of their own
// (ucomponent_load_dir()), for what the shipped ones never show: a
// definition with no Name= -- its name is its id, which must survive the
// array growing and the sort moving it -- and a removal that fails part
// of the way, which must leave the machine's state saying what it did.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "base/ucomponent.h"
#include "base/utest.h"

#define DIR_DEFS "/tmp/uct.defs"
#define DIR_FILES "/tmp/uct.files"

static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// Named ones sort FIRST ("A1".."A6", ids x1..x6) and the nameless after
// (m1..m6, named by their ids): whichever order the directory lists them
// in, by file name or by creation, the sort moves the nameless ones.
static void nameless(void) {
    char path[64], text[128];
    mkdir(DIR_DEFS, 0755);
    for (int i = 1; i <= 6; i++) {
        snprintf(path, sizeof path, DIR_DEFS "/m%d.component", i);
        snprintf(text, sizeof text, "License=MIT\nPaths=/nonexistent/m%d\n", i);
        write_file(path, text);
        snprintf(path, sizeof path, DIR_DEFS "/x%d.component", i);
        snprintf(text, sizeof text, "Name=A%d\nLicense=MIT\nPaths=/nonexistent/x%d\n", i, i);
        write_file(path, text);
    }
    struct ucomponent_set s = { 0 };
    int rc = ucomponent_load_dir(&s, DIR_DEFS);
    utest_checkf(rc == 0 && s.count == 12, "twelve definitions load (%d)", s.count);
    int bad = 0;
    char first_bad[64] = "";
    for (int i = 0; i < s.count; i++) {
        const struct ucomponent *c = &s.c[i];
        char want[24];
        if (c->id[0] == 'x') snprintf(want, sizeof want, "A%s", c->id + 1);
        else snprintf(want, sizeof want, "%s", c->id);
        if (strcmp(c->name, want) != 0) {
            if (!bad++) snprintf(first_bad, sizeof first_bad, "%s is named '%s'", c->id, c->name);
        }
    }
    utest_checkf(!bad, "a definition with no Name= is named by its own id, after the sort (%d wrong%s%s)", bad,
                 bad ? ": " : "", first_bad);
    utest_check(s.count == 12 && !strcmp(s.c[0].name, "A1") && !strcmp(s.c[11].name, "m6"),
                "...and the set is in Name order, the named ones first");
    ucomponent_free(&s);
    for (int i = 1; i <= 6; i++) {
        snprintf(path, sizeof path, DIR_DEFS "/m%d.component", i);
        remove(path);
        snprintf(path, sizeof path, DIR_DEFS "/x%d.component", i);
        remove(path);
    }
}

// A file on a read-only mount cannot be unlinked: /boot is mounted
// read-only by policy, so a component holding a file there and one in
// /tmp is removed only in part. Proved read-only first, or skipped.
static const char *boot_file(void) {
    static const char *const CAND[] = { "/boot/kernel.bin", "/boot/boot/kernel.bin" };
    int fd = open("/boot/uct.probe", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) { close(fd); unlink("/boot/uct.probe"); return 0; }   // writable: never risk it
    for (size_t i = 0; i < sizeof CAND / sizeof CAND[0]; i++)
        if (exists(CAND[i])) return CAND[i];
    return 0;
}

static void partial_remove(void) {
    const char *keep = boot_file();
    if (!keep) { utest_notef("no read-only /boot file: the partial removal is not checked"); return; }
    mkdir(DIR_DEFS, 0755);
    mkdir(DIR_FILES, 0755);
    write_file(DIR_FILES "/a", "a\n");
    write_file(DIR_FILES "/b", "b\n");
    char text[160];
    snprintf(text, sizeof text, "Name=Partial\nLicense=MIT\nPaths=" DIR_FILES "/ %s\n", keep);
    write_file(DIR_DEFS "/uct-partial.component", text);
    struct ucomponent_set s = { 0 };
    ucomponent_load_dir(&s, DIR_DEFS);
    int i = ucomponent_find(&s, "uct-partial");
    utest_check(i >= 0, "the partial definition loads");
    if (i < 0) { ucomponent_free(&s); return; }
    char why[160] = "";
    int rc = ucomponent_remove(&s, i, 0, 0, why, sizeof why);
    utest_checkf(rc != 0, "a removal that cannot delete %s fails (%s)", keep, why);
    utest_check(exists(keep), "...and the read-only file is still there");
    utest_check(!exists(DIR_FILES "/a") && !exists(DIR_FILES "/b"), "...the rest is gone");
    utest_check(ucomponent_state_removed("uct-partial"),
                "...and the state says it is removed: files deleted are never left unrecorded");
    utest_checkf(strstr(why, keep) != 0, "...naming what is left ('%s')", why);
    ucomponent_state_set("uct-partial", 0);
    ucomponent_free(&s);
    remove(DIR_DEFS "/uct-partial.component");
    rmdir(DIR_FILES);
}

int main(void) {
    utest_begin("ucomponent_test", "the optional components: definitions and Remove, in ring 3", 0);
    nameless();
    partial_remove();
    rmdir(DIR_DEFS);
    return utest_end();
}
