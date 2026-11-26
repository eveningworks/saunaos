// File permissions from ring 3 (docs/users-design.md, stage C): the
// owner, group and other classes, search on the folders above a path,
// the sticky bit, who may chmod and chown, root's exec rule, the umask,
// access(), and an owner on every new file.
//
// What a broken version would still pass, and what stops it here: every
// check made as ROOT passes with no checks at all, so the refusals are
// asked by a child started as uid 1000 -- and each refusal has a twin
// that must SUCCEED (the public file reads, the user's own folder takes a
// file), so a kernel refusing everything fails too. A check that only
// looked at the file would pass the search case, so the secret is inside
// a 0700 folder with a readable mode of its own. A sticky rule that only
// compared the folder's owner passes for root's file, so the child must
// also remove its own.
//
// Its children run with CRED_PERM_ENFORCE, so the checks hold for them
// whatever the machine's `fileperm=` says; a positive control: start
// them without it on a `fileperm=permissive` boot and checks 1-9 go red. SPAWNED by tools/tests/gate/usertest_run.py as
// root.
#include "cred_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "base/utest.h"

#define SELF "/tests/fileperm_test"
#define D    "/var/tmp/fileperm"

static const uint32_t GROUPS[] = { 1000, 10 };

static int wait_code(int pid) {
    int code = -1;
    if (sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

static int put(const char *path, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int ok = write(fd, "x\n", 2) == 2;
    close(fd);
    return ok && chmod(path, mode) == 0 ? 0 : -1;
}

static int refused(int rc, int err) { return rc < 0 && errno == err; }

// As uid 1000: the first check that goes wrong, by number, or 0.
static int child_main(void) {
    errno = 0;
    if (!refused(open(D "/secret", O_RDONLY), EACCES)) return 1;           // other has no r
    int fd = open(D "/public", O_RDONLY);
    if (fd < 0) return 2;                                                  // ...and this one has
    close(fd);
    if (!refused(open(D "/public", O_WRONLY), EACCES)) return 3;           // nor w
    if (!refused(open(D "/new", O_WRONLY | O_CREAT, 0644), EACCES)) return 4;   // root's folder
    struct stat st;
    if (!refused(stat(D "/closed/inner", &st), EACCES)) return 5;          // search above it
    if (!refused(unlink(D "/public"), EACCES)) return 6;
    if (!refused(truncate(D "/public", 0), EACCES)) return 7;
    if (!refused(rename(D "/public", D "/mine/stolen"), EACCES)) return 8;
    if (!refused(chmod(D "/public", 0666), EPERM)) return 9;               // not the owner

    // Its own folder: a file it makes is its own, the umask taken off.
    mode_t old = umask(077);
    fd = open(D "/mine/made", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    umask(old);
    if (fd < 0) return 10;
    close(fd);
    if (stat(D "/mine/made", &st) != 0 || st.st_uid != 1000 || st.st_gid != 1000) return 11;
    if ((st.st_mode & 0777) != 0700) return 12;                            // 0755 less 077
    if (chmod(D "/mine/made", 0640) != 0) return 13;                       // the owner may
    if (!refused(chown(D "/mine/made", 0, (gid_t)-1), EPERM)) return 14;   // but not give it away
    if (chown(D "/mine/made", (uid_t)-1, 10) != 0) return 15;              // ...into its own group, yes
    if (!refused(chown(D "/mine/made", (uid_t)-1, 0), EPERM)) return 16;   // ...one it is not in, no

    // The sticky folder: its own file goes, root's stays.
    if (!refused(unlink(D "/sticky/roots"), EPERM)) return 17;
    if (unlink(D "/sticky/users") != 0) return 18;

    // access() agrees with what open() did.
    if (!refused(access(D "/secret", R_OK), EACCES)) return 19;
    if (access(D "/public", R_OK) != 0) return 20;
    if (!refused(access(D "/public", W_OK), EACCES)) return 21;
    return 0;
}

// As root, enforced: it may read anything, but run only a file with an
// x bit somewhere. 0, or the check that went wrong.
static int rootchild_main(void) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    errno = 0;
    if (!refused(sys_spawn_opts(D "/noexec", &o), EACCES)) return 1;
    int fd = open(D "/secret", O_RDONLY);
    if (fd < 0) return 2;
    close(fd);
    return 0;
}

// Started as `uid` with CRED_PERM_ENFORCE; its exit code.
static int run_enforced(const char *arg, uint32_t uid, const uint32_t *groups, uint32_t ngroups) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = arg;
    o.flags = SPAWN_CREDS;
    o.uid = uid;
    o.gid = uid;
    o.groups = groups;
    o.ngroups = ngroups;
    o.cred_flags = CRED_PERM_ENFORCE;
    int pid = sys_spawn_opts(SELF, &o);
    return pid > 0 ? wait_code(pid) : -3000;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) return child_main();
    if (argc > 1 && !strcmp(argv[1], "--rootchild")) return rootchild_main();

    utest_begin("fileperm_test", "owner/group/other, search, sticky, chmod/chown rights, umask, access",
                UTEST_VERDICT_FILE);

    // The fixture, made as root. /var/tmp is real storage (tfs3), so the
    // owners and modes go through the inode, not ramfs's memory.
    mkdir(D, 0755);
    utest_check(chown(D, 0, 0) == 0 && chmod(D, 0755) == 0, "the fixture folder is root's, 0755");
    utest_check(put(D "/secret", 0600) == 0 && put(D "/public", 0644) == 0, "root's 0600 and 0644 files");
    mkdir(D "/closed", 0755);
    utest_check(chmod(D "/closed", 0700) == 0 && put(D "/closed/inner", 0644) == 0,
                "a 0700 folder holding a readable file");
    mkdir(D "/mine", 0755);
    utest_check(chown(D "/mine", 1000, 1000) == 0, "a folder given to 1000");
    mkdir(D "/sticky", 0755);
    utest_check(chmod(D "/sticky", 01777) == 0 && put(D "/sticky/roots", 0644) == 0 &&
                put(D "/sticky/users", 0644) == 0 && chown(D "/sticky/users", 1000, 1000) == 0,
                "a 1777 folder with a file of root's and one of 1000's");
    unlink(D "/mine/made");

    struct stat st;
    utest_check(stat(D "/public", &st) == 0 && st.st_uid == 0 && (st.st_mode & 07777) == 0644,
                "stat reports root's owner and the mode");
    utest_check(stat(D "/mine", &st) == 0 && st.st_uid == 1000 && st.st_gid == 1000,
                "...and the chowned folder's");

    int code = run_enforced("--child", 1000, GROUPS, sizeof GROUPS / sizeof GROUPS[0]);
    utest_checkf(code == 0, "as 1000: refused where the mode says, allowed where it does not "
                            "(first wrong check %d)", code);
    utest_check(stat(D "/sticky/roots", &st) == 0, "root's file in the sticky folder is still there");

    // Root's own rule: it may read anything, but execute only a file with
    // an x bit somewhere.
    utest_check(put(D "/noexec", 0644) == 0, "a 0644 file");
    code = run_enforced("--rootchild", 0, 0, 0);
    utest_checkf(code == 0, "root may not run a file with no x bit, and reads a 0600 one (%d)", code);

    // What a folder hands down (kernel/fs/vfs.c's fs_new_attrs()): a
    // setgid folder its group may write gives a file root makes there
    // that group and group write; a setuid folder of an account's gives
    // that account.
    mkdir(D "/sg", 0755);
    utest_check(chown(D "/sg", 0, 1000) == 0 && chmod(D "/sg", 02775) == 0 &&
                put(D "/sg/f", 0644) == 0, "a 2775 folder of group 1000, and root's file in it");
    utest_checkf(stat(D "/sg/f", &st) == 0 && st.st_gid == 1000 && st.st_uid == 0,
                 "...the file is in the folder's group (%u)", (unsigned)st.st_gid);
    int fd = open(D "/sg/g", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
    utest_checkf(stat(D "/sg/g", &st) == 0 && (st.st_mode & 0060) == 0060,
                 "...and group-writable when made (%o)", (unsigned)(st.st_mode & 0777));
    mkdir(D "/su", 0755);
    utest_check(chown(D "/su", 1000, 1000) == 0 && chmod(D "/su", 04755) == 0, "a 4755 folder of 1000's");
    mkdir(D "/su/sub", 0755);
    fd = open(D "/su/sub/f", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
    utest_checkf(stat(D "/su/sub/f", &st) == 0 && st.st_uid == 1000,
                 "...what root makes below it is 1000's, a folder down too (%u)", (unsigned)st.st_uid);
    unlink(D "/sg/f"); unlink(D "/sg/g"); rmdir(D "/sg");
    unlink(D "/su/sub/f"); rmdir(D "/su/sub"); rmdir(D "/su");

    // A mount point's mode is its mount's: the tmpfs service's 1777.
    utest_checkf(stat("/tmp", &st) == 0 && (st.st_mode & 07777) == 01777,
                 "/tmp is 1777 (%o)", (unsigned)(st.st_mode & 07777));

    unlink(D "/secret"); unlink(D "/public"); unlink(D "/noexec");
    unlink(D "/closed/inner"); rmdir(D "/closed");
    unlink(D "/mine/made"); rmdir(D "/mine");
    unlink(D "/sticky/roots"); unlink(D "/sticky/users"); rmdir(D "/sticky");
    rmdir(D);
    return utest_end();
}
