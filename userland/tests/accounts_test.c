// A person's account added, changed and removed through privd (users
// D3a, base/uusers.h): the passwd, group and shadow lines, the home made
// from /etc/skel and owned by the account, wheel membership, the refusals
// (a taken name, a bad one, an account signed in, the last administrator)
// and a kept home given back.
//
// What a broken version would still pass, and what stops it: an add that
// wrote passwd alone passes getpwnam(), so the group, the shadow line and
// the home's owner are each checked; a home made root's passes "it
// exists", so its owner and a skeleton file's owner must be the new uid;
// a removal that left a line behind passes "can no longer sign in", so
// the three files must come back BYTE FOR BYTE as they were; a refusal
// that refused everything passes the refusals, so each is paired with a
// success.
//
// SPAWNED by tools/tests/gate/usertest_run.py as root. It puts the three
// files back and deletes the homes it made.
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "base/ufileop.h"
#include "base/upriv.h"
#include "base/uusers.h"
#include "base/utest.h"

#define SELF "/tests/accounts_test"
#define NAME "anna"
#define HOME "/home/anna"

static const char *const FILES[] = { "/etc/passwd", "/etc/group", "/etc/shadow" };
static const int MODES[] = { 0644, 0644, 0600 };
static char *g_saved[3];
static size_t g_saved_n[3];

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

static int same_as_saved(int i) {
    size_t n;
    char *now = slurp(FILES[i], &n);
    int same = now && n == g_saved_n[i] && !memcmp(now, g_saved[i], n);
    free(now);
    return same;
}

static void restore(int i) {
    if (!g_saved[i] || same_as_saved(i)) return;
    unlink(FILES[i]);
    int fd = open(FILES[i], O_WRONLY | O_CREAT | O_EXCL, MODES[i]);
    if (fd < 0) return;
    fchown(fd, 0, 0);
    fchmod(fd, MODES[i]);
    write(fd, g_saved[i], g_saved_n[i]);
    close(fd);
}

static int refused(int rc, int e) { return rc < 0 && errno == e; }

static int in_wheel(const char *name) { return uusers_is_admin(name); }

static struct ufileop g_op;

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--sleep")) { sys_sleep_ms(20000); return 0; }

    utest_begin("accounts_test", "accounts added, changed and removed through privd", UTEST_VERDICT_FILE);
    for (int i = 0; i < 3; i++) {
        g_saved[i] = slurp(FILES[i], &g_saved_n[i]);
        utest_checkf(g_saved[i] != 0, "%s is there to save", FILES[i]);
    }
    if (getpwnam(NAME)) uusers_remove(NAME, 1, 0);   // a run cut short before
    ufileop_remove(HOME, 1, &g_op, 0);

    // --- added ---
    int rc = uusers_add(NAME, "Anna Virtanen", 0, "pw-anna", 0);
    utest_checkf(rc == 0, "root adds a standard account (%d)", rc ? errno : 0);
    struct passwd *pw = getpwnam(NAME);
    unsigned uid = pw ? pw->pw_uid : 0, gid = pw ? pw->pw_gid : 0;
    utest_checkf(pw && uid >= 1001 && gid == uid && !strcmp(pw->pw_dir, HOME) && !strcmp(pw->pw_shell, "/bin/tosh") &&
                 !strcmp(pw->pw_gecos, "Anna Virtanen"),
                 "its passwd line: uid above every person's (%u), its own gid, /home/anna, tosh, the full name", uid);
    struct group *g = getgrnam(NAME);
    utest_check(g && g->gr_gid == gid, "a group of its own name and id");
    utest_check(!in_wheel(NAME), "standard: not in wheel");
    utest_check(upriv_has_password(NAME) == 1 && upriv_auth(NAME, "pw-anna") == 0,
                "its password is set, and it is the one given");
    struct stat st;
    memset(&st, 0, sizeof st);
    int home_ok = stat(HOME, &st) == 0;   // before the check: its arguments are read in any order
    utest_checkf(home_ok && S_ISDIR(st.st_mode) && st.st_uid == uid && st.st_gid == gid &&
                 (st.st_mode & 07777) == 04700,
                 "its home is the account's, 04700 (mode %o, owner %u)", (unsigned)(st.st_mode & 07777),
                 (unsigned)st.st_uid);
    int skel_ok = stat(HOME "/Desktop/files.desktop", &st) == 0;
    utest_checkf(skel_ok && st.st_uid == uid,
                 "a file copied from /etc/skel is the account's too (owner %u)", (unsigned)st.st_uid);
    utest_check(stat(HOME "/.config", &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == uid,
                "...and its XDG folders");
    for (int i = 0; i < 2; i++)
        utest_checkf(stat(FILES[i], &st) == 0 && st.st_uid == 0 && (st.st_mode & 0777) == 0644,
                     "%s rewritten root's and 0644", FILES[i]);
    utest_check(stat(FILES[2], &st) == 0 && st.st_uid == 0 && (st.st_mode & 0777) == 0600,
                "/etc/shadow rewritten root's and 0600");

    // --- refused ---
    utest_check(refused(uusers_add(NAME, "", 0, "", 0), EEXIST), "the same name again: taken");
    utest_check(refused(uusers_add("user", "", 0, "", 0), EEXIST), "an existing person's name: taken");
    const char *bad[] = { "Bad:name", "", "0", "", 0 };
    utest_check(refused(upriv_action(UPRIV_ACT_USERADD, bad, 0), EINVAL),
                "privd itself refuses a name the rule refuses, whatever the client checked");
    const char *colon[] = { "zed", "A:B", "0", "", 0 };
    utest_check(refused(upriv_action(UPRIV_ACT_USERADD, colon, 0), EINVAL) && !getpwnam("zed"),
                "...and a full name holding a ':'");

    // --- changed ---
    utest_check(uusers_set(NAME, "admin", "1", 0) == 0 && in_wheel(NAME), "made an administrator: in wheel");
    utest_check(uusers_set(NAME, "admin", "0", 0) == 0 && !in_wheel(NAME), "made standard: out of wheel");
    g = getgrnam("wheel");
    int user_kept = 0;
    for (int i = 0; g && g->gr_mem && g->gr_mem[i]; i++) user_kept |= !strcmp(g->gr_mem[i], "user");
    utest_check(user_kept, "...and wheel's other member stays");
    utest_check(uusers_set(NAME, "real", "Anna V.", 0) == 0 && (pw = getpwnam(NAME)) && !strcmp(pw->pw_gecos, "Anna V.") &&
                pw->pw_uid == uid,
                "renamed: the full name changes, nothing else");
    utest_check(uusers_set(NAME, "password", "", 0) == 0 && upriv_has_password(NAME) == 0,
                "an administrator removes its password without the old one");
    int admins = 0;
    struct uuser *u;
    int n = uusers_list(&u);
    for (int i = 0; i < n; i++) admins += u[i].admin;
    uusers_free(u, n);
    if (admins == 1 && in_wheel("user"))
        utest_check(refused(uusers_set("user", "admin", "0", 0), EMLINK) && in_wheel("user"),
                    "the last administrator cannot be made standard");

    // --- signed in: refused; then removed, keeping the home ---
    char *sargv[] = { SELF, "--sleep", 0 };
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = sargv;
    o.env = environ;
    o.flags = SPAWN_ARGV | SPAWN_CREDS;
    o.uid = o.gid = uid;
    int child = sys_spawn_opts(SELF, &o);
    utest_check(child > 0 && refused(uusers_remove(NAME, 0, 0), EBUSY) && getpwnam(NAME),
                "an account with a program running is not removed");
    if (child > 0) { sys_kill(child, 9); int c; sys_waitpid(child, &c); }

    rc = uusers_remove(NAME, 0, 0);
    utest_checkf(rc == 0 && !getpwnam(NAME) && !getgrnam(NAME), "removed: no passwd line, no group (%d)",
                 rc ? errno : 0);
    for (int i = 0; i < 3; i++)
        utest_checkf(same_as_saved(i), "%s is byte for byte as it was before the add", FILES[i]);
    utest_check(stat(HOME, &st) == 0 && st.st_uid == uid, "the home is kept, still its owner's");

    // --- added again over the kept home: its owner back ---
    rc = uusers_add(NAME, "", 0, "", 0);
    pw = getpwnam(NAME);
    utest_checkf(rc == 0 && pw && pw->pw_uid == uid, "added again, it takes the kept home's owner back (%u)",
                 pw ? (unsigned)pw->pw_uid : 0);
    rc = uusers_remove(NAME, 1, 0);
    utest_checkf(rc == 0 && stat(HOME, &st) != 0, "removed with its files: the home is gone (%d)", rc ? errno : 0);

    for (int i = 0; i < 3; i++) { restore(i); free(g_saved[i]); }
    return utest_end();
}
