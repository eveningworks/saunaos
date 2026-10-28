// /bin/utf8migrate end to end, judged by BYTES: a fixture tree under the
// persistent scratch directory with Latin-1 names and contents -- made
// here with raw bytes, which no console could type -- converted with
// --root, then run again (the marker) and again with --force (nothing
// already UTF-8 is touched), each compared with what the tree held.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "rt/sys.h"
#include "tmppath.h"
#include "lib/utmppath.h"
#include "lib/ulistdir.h"
#include "lib/utest.h"

static char R[128];             // the fixture root

static char *P(const char *rel) {
    static char buf[8][512];
    static int k;
    char *b = buf[k++ & 7];
    snprintf(b, 512, "%s%s", R, rel);
    return b;
}

static void put(const char *rel, const char *data, int len) {
    int fd = open(P(rel), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { if (write(fd, data, (size_t)len) != len) utest_notef("short write %s", rel); close(fd); }
    else utest_notef("cannot create %s", rel);
}

// The file's bytes into `out` (NUL-terminated); its length, or -1.
static int get(const char *rel, char *out, int cap) {
    int fd = open(P(rel), O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, out, (size_t)cap - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = 0;
    return n;
}

static int same(const char *rel, const char *want, int len) {
    char b[256];
    return get(rel, b, sizeof b) == len && !memcmp(b, want, (size_t)len);
}

static int exists(const char *rel) { struct sys_stat st; return sys_stat(P(rel), &st) == 0; }

// The whole tree as one string -- every path and every byte -- so a run
// that changed ANYTHING compares unequal.
static void manifest_dir(const char *dir, char **buf, size_t *len, size_t *cap) {
    struct sys_dirent *e;
    int n = ulistdir(dir, &e);
    for (int i = 0; i < n; i++) {
        if (!strcmp(dir, R) && !strcmp(e[i].name, "var")) continue;   // the log grows
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e[i].name);
        char data[256];
        int dl = 0;
        if (!e[i].is_dir) {
            int fd = open(path, O_RDONLY);
            if (fd >= 0) { dl = (int)read(fd, data, sizeof data); close(fd); if (dl < 0) dl = 0; }
        }
        size_t need = strlen(path) + (size_t)dl + 8;
        if (*len + need > *cap) { *cap = (*cap + need) * 2; *buf = realloc(*buf, *cap); }
        *len += (size_t)sprintf(*buf + *len, "%s%c", path, e[i].is_dir ? '/' : '=');
        memcpy(*buf + *len, data, (size_t)dl);
        *len += (size_t)dl;
        (*buf)[(*len)++] = '\n';
        if (e[i].is_dir) manifest_dir(path, buf, len, cap);
    }
    if (n > 0) free(e);
}

static char *manifest(size_t *len) {
    char *buf = 0;
    size_t cap = 0;
    *len = 0;
    manifest_dir(R, &buf, len, &cap);
    return buf;
}

static int run(const char *extra) {
    char args[256];
    snprintf(args, sizeof args, "--root %s %s", R, extra);
    int pid = sys_spawn("/bin/utf8migrate", args, -1);
    if (pid < 0) return -100;
    int code = -1;
    sys_waitpid(pid, &code);
    return code;
}

static void rm_tree(const char *dir) {
    struct sys_dirent *e;
    int n = ulistdir(dir, &e);
    for (int i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e[i].name);
        if (e[i].is_dir) rm_tree(path);
        else sys_unlink(path);
    }
    if (n > 0) free(e);
    rmdir(dir);
}

int main(void) {
    utest_begin("utf8migrate_test", "Latin-1 names and text files become UTF-8, once", UTEST_VERDICT_FILE);
    // THE BOOT'S OWN RUN FIRST: the debug console runs this before init
    // has finished starting services, and the service walking / while
    // the fixture is made converts half of it (it did).
    struct sys_stat bst;
    int waited = 0;
    while (sys_stat("/var/lib/utf8migrate", &bst) != 0 && waited < 120000) {
        sys_sleep_ms(250);
        waited += 250;
    }
    utest_check(waited < 120000, "the boot's conversion has run (its marker exists)");
    char boot[160];
    int fd = open("/var/lib/utf8migrate", O_RDONLY);
    int bn = fd >= 0 ? (int)read(fd, boot, sizeof boot - 1) : -1;
    if (fd >= 0) close(fd);
    if (bn > 0) { boot[bn] = 0; boot[strcspn(boot, "\n")] = 0; utest_notef("boot's run: %s", boot); }
    snprintf(R, sizeof R, "%s", utest_path(TMP_PERSISTENT, "utf8migrate_test"));
    rm_tree(R);
    mkdir(R, 0755);
    static const char *const DIRS[] = { "/etc", "/home", "/home/K\xe4s", "/home/Caf\xc3\xa9",
                                        "/home/Win16", "/home/Dr\xe4ve" };
    for (size_t i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++) mkdir(P(DIRS[i]), 0755);

    put("/etc/host.conf", "K\xe4se\n", 5);                 // converted
    put("/etc/plain", "na\xefve\n", 6);                    // /etc: any name
    put("/etc/ok.conf", "caf\xc3\xa9\n", 6);               // already UTF-8
    put("/etc/blob", "\xe4\x00\xff", 3);                   // binary
    put("/etc/win16.conf", "drive_d=/home/Dr\xe4ve\n", 19); // a Latin-1 drive path
    put("/home/notes.txt", "\xe5\xe4\xf6\n", 4);           // converted
    put("/home/NOTES.MD", "\xe9", 1);                      // the extension in capitals
    put("/home/photo.bin", "\xe4\xe4", 2);                 // not a text extension
    put("/home/link1.txt", "\xe4", 1);
    sys_link(P("/home/link1.txt"), P("/home/link2.txt"));  // hard-linked: skipped
    put("/home/K\xe4s/\xf6l.txt", "\xe4", 1);              // renamed twice, converted
    put("/home/Caf\xc3\xa9/x.txt", "ok", 2);
    put("/home/Z\xc3\xa4.txt", "utf8", 4);                 // the clash's owner
    put("/home/Z\xe4.txt", "latin", 5);                    // -> "Zä (1).txt"
    put("/home/Win16/WIN.INI", "\xe4", 1);                 // drive C: by default
    put("/home/Win16/R\xe4.TXT", "\xe4", 1);
    put("/home/Dr\xe4ve/x.txt", "\xe4", 1);                // drive D:, named in a Latin-1 conf

    size_t l0, l1;
    char *m0 = manifest(&l0);
    utest_check(run("--dry-run") == 0, "a dry run exits 0");
    char *m1 = manifest(&l1);
    utest_check(l0 == l1 && !memcmp(m0, m1, l0), "a dry run changes nothing");
    utest_check(!exists("/var/lib/utf8migrate") && !exists("/var/log/utf8migrate.log"),
                "a dry run writes no marker and no log");
    free(m1);

    int code = run("");
    utest_checkf(code == 0, "the conversion exits 0 (got %d)", code);
    utest_check(same("/etc/host.conf", "K\xc3\xa4se\n", 6), "an /etc file's contents converted");
    utest_check(same("/etc/plain", "na\xc3\xafve\n", 7), "an /etc file with no extension converted");
    utest_check(same("/etc/ok.conf", "caf\xc3\xa9\n", 6), "an /etc file already UTF-8 untouched");
    utest_check(same("/etc/blob", "\xe4\x00\xff", 3), "a binary file (a NUL) untouched");
    utest_check(same("/etc/win16.conf", "drive_d=/home/Dr\xc3\xa4ve\n", 20), "win16.conf converted");
    utest_check(same("/home/notes.txt", "\xc3\xa5\xc3\xa4\xc3\xb6\n", 7), "a /home .txt converted");
    utest_check(same("/home/NOTES.MD", "\xc3\xa9", 2), "a /home .MD converted");
    utest_check(same("/home/photo.bin", "\xe4\xe4", 2), "a /home file with no text extension untouched");
    utest_check(same("/home/link1.txt", "\xe4", 1) && same("/home/link2.txt", "\xe4", 1),
                "a hard-linked file untouched, both names");
    utest_check(!exists("/home/K\xe4s") && exists("/home/K\xc3\xa4s"), "a Latin-1 folder renamed");
    utest_check(same("/home/K\xc3\xa4s/\xc3\xb6l.txt", "\xc3\xa4", 2),
                "a Latin-1 file inside it renamed and converted");
    utest_check(same("/home/Caf\xc3\xa9/x.txt", "ok", 2), "an already-UTF-8 folder untouched");
    utest_check(same("/home/Z\xc3\xa4.txt", "utf8", 4), "the clash's owner kept its name and bytes");
    utest_check(same("/home/Z\xc3\xa4 (1).txt", "latin", 5) && !exists("/home/Z\xe4.txt"),
                "the clashing Latin-1 name became \"Z\xc3\xa4 (1).txt\"");
    utest_check(same("/home/Win16/WIN.INI", "\xe4", 1) && same("/home/Win16/R\xe4.TXT", "\xe4", 1),
                "w16's drive C: left alone, names and contents");
    utest_check(exists("/home/Dr\xc3\xa4ve") && same("/home/Dr\xc3\xa4ve/x.txt", "\xe4", 1),
                "a mapped drive's folder renamed with its conf, its contents left alone");
    utest_check(exists("/var/lib/utf8migrate"), "the marker was written");
    static char log[4096];
    int ll = get("/var/log/utf8migrate.log", log, sizeof log);
    utest_check(ll > 0 && strstr(log, "rename  /home/K\\xE4s -> /home/K\xc3\xa4s"),
                "the log names the old bytes and the new name");
    utest_check(ll > 0 && strstr(log, "skip    /home/link1.txt: it has 2 links"),
                "the log names the hard-link skip");
    utest_check(ll > 0 && strstr(log, "done: 4 renamed, 6 converted"), "the log's summary counts");

    size_t l2, l3, l4;
    char *m2 = manifest(&l2);
    utest_check(run("") == 0, "a second run exits 0");
    char *m3 = manifest(&l3);
    utest_check(l2 == l3 && !memcmp(m2, m3, l2), "a second run (the marker) changes nothing");
    utest_check(run("--force") == 0, "a forced run exits 0");
    char *m4 = manifest(&l4);
    utest_check(l2 == l4 && !memcmp(m2, m4, l2) ? 1 : 0, "a forced run over the converted tree changes nothing");
    ll = get("/var/log/utf8migrate.log", log, sizeof log);
    utest_check(ll > 0 && strstr(log, "done: 0 renamed, 0 converted"), "the forced run's summary says so");
    free(m0); free(m2); free(m3); free(m4);
    rm_tree(R);
    return utest_end();
}
