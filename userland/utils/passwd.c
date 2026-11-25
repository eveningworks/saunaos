// passwd -- set, change or remove an account's password, through privd
// (base/upriv.h; docs/users-design.md, D2a). Not setuid: privd checks the
// current password and rewrites /etc/shadow, which nothing else may read.
#include "base/uargs.h"
#include "base/uaskpass.h"
#include "base/upriv.h"
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int f_delete, f_status;

static const struct uargs_opt OPTS[] = {
    { "delete", 'd', 0, "remove the password: the account signs in without one", &f_delete, 0 },
    { "status", 'S', 0, "say whether the account has a password (P) or not (NP)", &f_status, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    "passwd", "[OPTION]... [ACCOUNT]",
    "Set or change ACCOUNT's password -- your own when none is named.\n"
    "Asks for the current password first, then the new one twice.",
    OPTS, 0,
    "Only root may name another account, and root is not asked for its\n"
    "current password. Setting a password turns automatic sign-in off.",
    0, 0, 0,
};

#define PW_MAX 256

// The current password, or "" for an account that has none (or for
// root, which privd does not ask). -1 at end of input.
static int ask_current(const char *acct, char *cur) {
    cur[0] = '\0';
    if (geteuid() == 0) return 0;
    int has = upriv_has_password(acct);
    if (has < 0) {
        fprintf(stderr, "passwd: %s\n", errno == ENOENT ? "privd is not running" : strerror(errno));
        return -1;
    }
    if (!has) return 0;
    return uaskpass("Current password: ", cur, PW_MAX) < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc > 1) return uargs_error(&PROG, "one ACCOUNT at most");
    struct passwd *me = getpwuid(getuid());
    if (!me) { fprintf(stderr, "passwd: this account is not in /etc/passwd\n"); return 1; }
    char acct[64];
    snprintf(acct, sizeof acct, "%s", a.argc ? a.argv[0] : me->pw_name);
    if (!getpwnam(acct)) { fprintf(stderr, "passwd: %s: no such account\n", acct); return 1; }
    if (geteuid() != 0 && strcmp(acct, me->pw_name) != 0) {
        fprintf(stderr, "passwd: only root may change another account's password\n");
        return 1;
    }
    if (f_status) {
        int has = upriv_has_password(acct);
        if (has < 0) { fprintf(stderr, "passwd: %s\n", strerror(errno)); return 1; }
        printf("%s %s\n", acct, has ? "P" : "NP");
        return 0;
    }

    char cur[PW_MAX], pw[PW_MAX], again[PW_MAX];
    if (ask_current(acct, cur) < 0) return 1;
    pw[0] = '\0';
    if (!f_delete) {
        if (uaskpass("New password: ", pw, sizeof pw) < 0) return 1;
        if (!pw[0]) {
            fprintf(stderr, "passwd: an empty password is no password -- use passwd -d to remove it\n");
            return 1;
        }
        if (uaskpass("Retype new password: ", again, sizeof again) < 0) return 1;
        if (strcmp(pw, again) != 0) {
            fprintf(stderr, "passwd: the passwords do not match\n");
            return 1;
        }
    }
    int rc = upriv_passwd(acct, geteuid() == 0 ? 0 : cur, pw);
    int e = errno;
    memset(cur, 0, sizeof cur);
    memset(pw, 0, sizeof pw);
    memset(again, 0, sizeof again);
    if (rc < 0) {
        const char *why = e == EACCES ? "the current password is wrong"
                        : e == EPERM  ? "not allowed"
                        : e == ENOENT ? "privd is not running"
                        : strerror(e);
        fprintf(stderr, "passwd: %s\n", why);
        return 1;
    }
    printf(f_delete ? "passwd: %s's password removed\n" : "passwd: %s's password changed\n", acct);
    return 0;
}
