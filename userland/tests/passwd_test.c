// Passwords through privd, from ring 3 (docs/users-design.md, D2a): set,
// check, change and remove one; who may ask about whose; sudo's
// password-as-consent; /etc/shadow's owner and mode; a hash made by
// Linux's libxcrypt verifying here.
//
// What a broken version would still pass, and what stops it: a checker
// that said yes to everything passes every right password, so each is
// paired with a wrong one that must fail; one that said no to everything
// passes the wrong ones, so the right ones must pass; a per-account rule
// that let anyone ask passes root's questions, so 1000 must be refused
// about root; a hash this side alone could read passes our own set-then-
// check, so a libxcrypt hash must verify too; a delay that was not there
// passes everything, so the wrong answer's time is measured.
//
// SPAWNED by tools/tests/gate/usertest_run.py as root. It restores
// /etc/shadow's bytes, owner and mode at the end.
#include <errno.h>
#include <fcntl.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "base/upriv.h"
#include "base/utest.h"

#define SELF   "/tests/passwd_test"
#define SHADOW "/etc/shadow"

// "interop-pw", hashed by the dev host's libxcrypt (crypt_gensalt("$y$")).
#define LINUX_HASH "$y$j9T$sE5wgoZcB6oak9HP2fXal.$25eJFQxljut9p7FUdANLmkr/5YPi.VX9/kUCdegMd79"

static const uint32_t GROUPS[] = { 1000, 10 };   // user, wheel

static int wait_code(int pid) {
    int code = -1;
    if (sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

static int spawn_as_user(const char *mode) {
    char *argv[] = { SELF, (char *)mode, 0 };
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.env = environ;
    o.flags = SPAWN_ARGV | SPAWN_CREDS;
    o.uid = o.gid = 1000;
    o.groups = GROUPS;
    o.ngroups = 2;
    return sys_spawn_opts(SELF, &o);
}

static int denied(int rc, int e) { return rc < 0 && errno == e; }

// As 1000, with the password set to "pw-one" by the parent. A distinct
// code per check, so a failure names it.
static int user_main(void) {
    if (upriv_has_password("user") != 1) return 10;
    if (!denied(upriv_has_password("root"), EPERM)) return 11;       // not its own
    if (!denied(upriv_auth("root", "x"), EPERM)) return 12;
    if (upriv_auth("user", "pw-one") != 0) return 13;
    if (!denied(upriv_auth("user", "pw-two"), EACCES)) return 14;
    if (!denied(upriv_passwd("root", "", "x"), EPERM)) return 15;
    if (!denied(upriv_passwd("user", "wrong", "pw-two"), EACCES)) return 16;
    if (upriv_passwd("user", "pw-one", "pw-two") != 0) return 17;
    if (upriv_auth("user", "pw-two") != 0) return 18;
    if (!denied(upriv_auth("user", "pw-one"), EACCES)) return 19;
    // sudo's shape: the right password is the consent; a wrong one is not.
    char *argv[] = { SELF, "--euid", 0 };
    if (!denied(upriv_spawn_password(SELF, argv, 0, "pw-one"), EACCES)) return 20;
    int pid = upriv_spawn_password(SELF, argv, 0, "pw-two");
    if (pid <= 0) return 21;
    if (wait_code(pid) != 7) return 22;                              // it ran as root
    return 0;
}

// As 1000 WITHOUT the seat, the account having no password: "" sent as
// sudo's password must not stand in for consent (the review's root hole).
static int nopw_main(void) {
    char *argv[] = { SELF, "--euid", 0 };
    int pid = upriv_spawn_password(SELF, argv, 0, "");
    if (pid > 0) { wait_code(pid); return 30; }
    return errno == EPERM ? 0 : 31;
}

static char *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    size_t cap = 4096;
    char *b = malloc(cap);
    *n = 0;
    ssize_t k;
    while (b && (k = read(fd, b + *n, cap - *n)) > 0) {
        *n += (size_t)k;
        if (*n == cap) { char *nb = realloc(b, cap *= 2); if (!nb) { free(b); b = 0; } else b = nb; }
    }
    close(fd);
    return b;
}

// Puts the file back as it was: bytes, root's, 0600.
static void restore(const char *bytes, size_t n) {
    unlink(SHADOW);
    int fd = open(SHADOW, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return;
    fchown(fd, 0, 0);
    fchmod(fd, 0600);
    if (bytes && n) write(fd, bytes, n);
    close(fd);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--user")) return user_main();
    if (argc > 1 && !strcmp(argv[1], "--nopw")) return nopw_main();
    if (argc > 1 && !strcmp(argv[1], "--euid")) return geteuid() == 0 ? 7 : 8;

    utest_begin("passwd_test", "passwords through privd: set, check, change, remove, who may ask",
                UTEST_VERDICT_FILE);
    size_t saved_n = 0;
    char *saved = slurp(SHADOW, &saved_n);
    utest_check(saved != 0, "/etc/shadow is there to save");

    int rc = upriv_passwd("user", 0, "pw-one");
    utest_checkf(rc == 0, "root sets user's password without the current one (%d)", rc ? errno : 0);
    struct stat st;
    utest_check(stat(SHADOW, &st) == 0 && st.st_uid == 0 && (st.st_mode & 0777) == 0600,
                "the rewritten /etc/shadow is root's and 0600");
    struct spwd *sp = getspnam("user");
    utest_check(sp && !strncmp(sp->sp_pwdp, "$y$j9T$", 7), "user's line holds a yescrypt hash at "
                "libxcrypt's default cost ($y$j9T$)");
    sp = getspnam("root");
    utest_check(sp && sp->sp_pwdp[0] == '!', "root's line is kept, locked");

    utest_check(upriv_auth("user", "pw-one") == 0, "the right password is right");
    uint64_t t0 = sys_monotonic_ns();
    rc = upriv_auth("user", "pw-on");
    uint64_t ms = (sys_monotonic_ns() - t0) / 1000000;
    utest_checkf(denied(rc, EACCES) && ms >= UPRIV_FAIL_DELAY_MS - 100,
                 "a wrong one is wrong, answered after the fail delay (%llu ms)",
                 (unsigned long long)ms);
    utest_check(denied(upriv_auth("root", ""), EACCES), "root, locked, matches no password");

    int pid = spawn_as_user("--user");
    int code = pid > 0 ? wait_code(pid) : -3000;
    utest_checkf(code == 0, "1000 checks and changes only its own password, with the current one, "
                            "and its right password elevates a spawn (%d)", code);

    rc = upriv_passwd("user", 0, "");
    utest_check(rc == 0 && upriv_has_password("user") == 0 && upriv_auth("user", "") == 0 &&
                denied(upriv_auth("user", "pw-two"), EACCES),
                "removed, the account has no password and matches only \"\"");
    pid = spawn_as_user("--nopw");
    code = pid > 0 ? wait_code(pid) : -3000;
    utest_checkf(code == 0, "with no password, \"\" sent as the password elevates nothing for an "
                            "account away from the machine (%d)", code);

    // A hash Linux wrote verifies here.
    {
        const char *t = "root:!:20782:0:99999:7:::\nuser:" LINUX_HASH ":20782:0:99999:7:::\n";
        restore(t, strlen(t));
    }
    utest_check(upriv_auth("user", "interop-pw") == 0 && denied(upriv_auth("user", "interop-pX"), EACCES),
                "a libxcrypt yescrypt hash verifies, and only for its password");

    // A file root does not own alone is not read (sshd's StrictModes).
    chown(SHADOW, 1000, 1000);
    errno = 0;
    sp = getspnam("user");
    int e = errno;
    utest_checkf(!sp && e == EPERM, "getspnam() refuses a /etc/shadow that is not root's (%d)", e);
    utest_check(upriv_has_password("user") == 1 && denied(upriv_auth("user", "interop-pw"), EACCES),
                "...and privd locks every account rather than believe it");
    chown(SHADOW, 0, 0);
    chmod(SHADOW, 0644);
    errno = 0;
    utest_check(!getspnam("user") && errno == EPERM, "...or one others may read");

    restore(saved, saved_n);
    free(saved);
    return utest_end();
}
