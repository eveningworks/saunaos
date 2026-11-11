// POSIX's answers for the path calls a ported program leans on: the
// empty path, a directory renamed over an empty one, and link().
// std::filesystem's tests found each one (the cc corpus, 2026-11-11).
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "tmppath.h"
#include "base/utest.h"

static char base[256];

static const char *at(const char *name) {
    static char buf[4][320];
    static int n;
    char *b = buf[n++ & 3];
    snprintf(b, sizeof buf[0], "%s/%s", base, name);
    return b;
}

static int touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    int ok = write(fd, "x", 1) == 1;
    close(fd);
    return ok;
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(void) {
    utest_begin("fsposix_test", "the empty path, directory rename, link()", 0);
    tmppath(base, sizeof base, TMP_PERSISTENT, "fsposix_test");
    mkdir(base, 0755);

    // --- the empty path names no file ----------------------------------
    errno = 0;
    utest_check(open("", O_RDONLY) < 0 && errno == ENOENT, "open(\"\") is ENOENT");
    errno = 0;
    utest_check(remove("") < 0 && errno == ENOENT, "remove(\"\") is ENOENT, not the current directory");
    errno = 0;
    utest_check(mkdir("", 0755) < 0 && errno == ENOENT, "mkdir(\"\") is ENOENT");
    struct stat st;
    errno = 0;
    utest_check(stat("", &st) < 0 && errno == ENOENT, "stat(\"\") is ENOENT");
    // remove_all("") walked this listing -- of the current directory --
    // and deleted what it found.
    errno = 0;
    DIR *d = opendir("");
    utest_check(!d && errno == ENOENT, "opendir(\"\") lists nothing: ENOENT");
    if (d) closedir(d);

    // --- rename() and directories --------------------------------------
    mkdir(at("full"), 0755);
    touch(at("full/inside"));
    mkdir(at("empty"), 0755);
    touch(at("file"));
    errno = 0;
    utest_check(rename(at("full"), at("file")) < 0 && errno == ENOTDIR,
          "a directory over a file is ENOTDIR");
    errno = 0;
    utest_check(rename(at("file"), at("empty")) < 0 && errno == EISDIR,
          "a file over a directory is EISDIR");
    mkdir(at("full2"), 0755);
    touch(at("full2/other"));
    errno = 0;
    utest_check(rename(at("full"), at("full2")) < 0 && errno == ENOTEMPTY,
          "a directory over a non-empty one is ENOTEMPTY");
    utest_check(rename(at("full"), at("empty")) == 0,
          "a directory over an EMPTY directory replaces it");
    utest_check(!is_dir(at("full")) && is_dir(at("empty")) &&
          access(at("empty/inside"), F_OK) == 0,
          "...and its contents moved with it");
    utest_check(rename(at("file"), at("file")) == 0, "a rename onto itself is a no-op");

    // --- link() --------------------------------------------------------
    int r = link(at("file"), at("file2"));
    if (r < 0 && errno == EPERM) {
        utest_notef("%s's filesystem has no link counts; link() skipped", base);
    } else {
        utest_check(r == 0, "link() makes a second name");
        utest_check(stat(at("file"), &st) == 0 && st.st_nlink == 2,
              "both names count: st_nlink is 2");
        unlink(at("file"));
        utest_check(stat(at("file2"), &st) == 0 && st.st_nlink == 1 && st.st_size == 1,
              "the second name keeps the file when the first goes");
        unlink(at("file2"));
    }

    unlink(at("empty/inside"));
    rmdir(at("empty"));
    unlink(at("full2/other"));
    rmdir(at("full2"));
    unlink(at("file"));
    rmdir(base);
    return utest_end();
}
