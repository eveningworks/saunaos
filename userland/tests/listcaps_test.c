// The Recycle Bin past its old fixed tables (fixed-tables round 4, track
// B), on real files under /home/user and the home bin: a file in a folder
// past 255 bytes is binned, listed with its whole path and restored; a
// 250-byte name is binned under a cut name (Path= keeps it whole) and
// restored to it; and more items than any old cap (512) are all listed,
// counted by utrash_usage() and found by utrash_any(). Each of these
// failed before: a deep path was ENAMETOOLONG, a name past 63 bytes too,
// and a listing stopped at 256 or 512 items. Then a config key as long as
// a desktop icon's name round-trips through uconf, and a desktop entry
// reads a long Exec= whole (appentry_test has its own).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "base/utest.h"
#include "base/utrash.h"
#include "base/uconf.h"

#define DIR   "/home/user/listcaps-test"
#define PART  "deep_0123456789abcdefghijklmnopqrstuvw"   // 38 + "_N"
#define MANY  520

static int make(const char *path) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return 0;
    sys_write(fd, "x", 1);
    sys_close(fd);
    return 1;
}

static int exists(const char *path) {
    struct sys_stat st;
    return sys_stat(path, &st) == 0;
}

// The item in the bin deleted from `orig`, or -1.
static int find(const struct utrash_item *items, int n, const char *orig) {
    for (int i = 0; i < n; i++) if (!strcmp(items[i].orig, orig)) return i;
    return -1;
}

static int under_dir(const char *p) { return !strncmp(p, DIR "/", sizeof DIR); }

// Every item this test put in the bin, deleted for good.
static void clear_ours(void) {
    struct utrash_item *items;
    int n = utrash_list(&items);
    for (int i = 0; i < n; i++) {
        if (!under_dir(items[i].orig)) continue;
        char *p = utrash_item_path(&items[i]);
        if (p) sys_unlink(p);
        free(p);
        utrash_forget(&items[i]);
    }
    if (n > 0) utrash_list_free(items, n);
}

static void deep_path(void) {
    char *deep = strdup(DIR);
    for (int i = 0; i < 8 && deep; i++) {
        char *next = 0;
        if (asprintf(&next, "%s/%s_%d", deep, PART, i) < 0) next = 0;
        free(deep);
        deep = next;
        if (deep) sys_mkdir(deep);
    }
    char *file = 0;
    if (!deep || asprintf(&file, "%s/deep.txt", deep) < 0 || !make(file)) {
        utest_check(0, "a file in a folder past 255 bytes");
        free(deep);
        return;
    }
    struct utrash_item it;
    int e = utrash_put(file, &it);
    utest_checkf(!e, "a %d-byte path goes into the bin (e=%d)", (int)strlen(file), e);
    if (!e) {
        utest_check(!strcmp(it.orig, file) && !exists(file), "...whole, and the file is gone");
        utrash_item_free(&it);
    }
    struct utrash_item *items;
    int n = utrash_list(&items), at = n > 0 ? find(items, n, file) : -1;
    utest_check(at >= 0, "the listing has it, by its whole path");
    if (at >= 0) {
        e = utrash_restore(&items[at]);
        utest_checkf(!e && exists(file), "it is restored where it was (e=%d)", e);
    }
    if (n > 0) utrash_list_free(items, n);
    free(file);
    free(deep);
}

static void long_name(void) {
    char name[256], *file = 0;
    memset(name, 'n', 246);
    strcpy(name + 246, ".txt");
    if (asprintf(&file, DIR "/%s", name) < 0 || !make(file)) {
        utest_check(0, "a file with a 250-byte name");
        return;
    }
    struct utrash_item it;
    int e = utrash_put(file, &it);
    utest_checkf(!e, "a 250-byte name goes into the bin (e=%d)", e);
    if (!e) {
        size_t l = strlen(it.name);
        utest_checkf(l + 10 <= 255 && !strcmp(it.name + l - 4, ".txt"),
                     "its name in the bin leaves room for .trashinfo, the extension kept (%d)", (int)l);
        utest_check(!strcmp(it.orig, file), "Path= keeps the whole name");
        e = utrash_restore(&it);
        utest_checkf(!e && exists(file), "it is restored to the whole name (e=%d)", e);
        utrash_item_free(&it);
    }
    free(file);
}

static void many(void) {
    int put = 0;
    for (int i = 0; i < MANY; i++) {
        char p[64];
        snprintf(p, sizeof p, DIR "/m%03d.txt", i);
        if (make(p) && utrash_put(p, 0) == 0) put++;
    }
    utest_checkf(put == MANY, "%d of %d files go into the bin", put, MANY);
    struct utrash_item *items;
    int n = utrash_list(&items), ours = 0;
    for (int i = 0; i < n; i++) ours += under_dir(items[i].orig);
    utest_checkf(ours == MANY, "the listing has all %d of them (%d)", MANY, ours);
    if (n > 0) utrash_list_free(items, n);

    struct utrash_usage *u;
    int nb = utrash_usage(&u), held = 0;
    for (int b = 0; b < nb; b++)
        if (!strcmp(u[b].bin, utrash_home_bin())) held = u[b].items;
    utest_checkf(held >= MANY, "the home bin's usage counts them (%d)", held);
    if (nb > 0) utrash_usage_free(u, nb);
    utest_check(utrash_any(), "the bin is not empty");
}

// A config key as long as a desktop icon's name (the desktop keys its
// saved cells by file name, /etc/desktop.conf): set, read back, rewritten.
static void long_key(void) {
    char key[256];
    memset(key, 'k', 245);
    key[245] = '\0';
    const char *f = DIR "/long.conf";
    int set = uconf_set(f, "before", "1") && uconf_set(f, key, "3,4") && uconf_set(f, "after", "2");
    char v[16] = "", b[4] = "", a[4] = "";
    int got = uconf_get(f, key, v, sizeof v);
    utest_checkf(set && got && !strcmp(v, "3,4"), "a 245-byte config key round-trips (set %d, got %d, '%s')",
                 set, got, v);
    int moved = uconf_set(f, key, "5,6") && uconf_get(f, key, v, sizeof v) && !strcmp(v, "5,6");
    utest_check(moved && uconf_get(f, "before", b, sizeof b) && uconf_get(f, "after", a, sizeof a) &&
                !strcmp(b, "1") && !strcmp(a, "2"), "...is rewritten in place, its neighbours kept");
}

int main(void) {
    utest_begin("listcaps_test", "the Recycle Bin past its old fixed tables", UTEST_VERDICT_FILE);
    char *rm[] = { "/bin/rm", "-r", DIR, 0 };
    int code = 0, rp = sys_spawn_argv("/bin/rm", rm);
    if (rp > 0) sys_waitpid(rp, &code);
    clear_ours();
    if (sys_mkdir(DIR) < 0) { utest_check(0, "mkdir " DIR); return utest_end(); }

    deep_path();
    long_name();
    many();
    long_key();

    clear_ours();
    rp = sys_spawn_argv("/bin/rm", rm);
    if (rp > 0) sys_waitpid(rp, &code);
    return utest_end();
}
