// user -- list, add, remove and change the people's accounts (users D3a).
//
// The command line of Settings > System > Users, over the same privd
// actions (base/uusers.h): an administrator's, asked as `sudo` asks --
// the password on this terminal once the account has one. One command
// with verbs, as `component` and `part` are, rather than shadow-utils'
// useradd/userdel/usermod trio.
#include "base/uargs.h"
#include "base/uaskpass.h"
#include "base/upriv.h"
#include "base/uusers.h"
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_admin, g_password, g_delete;
static const char *g_full;

static const struct uargs_opt OPTS[] = {
    { "full-name", 'c', "NAME", "with add: the person's full name, shown at sign-in", 0, &g_full },
    { "admin", 'a', 0, "with add: an administrator (in wheel), not a standard account", &g_admin, 0 },
    { "password", 'p', 0, "with add: ask for the new account's password (twice)", &g_password, 0 },
    { "delete-files", 'r', 0, "with remove: delete the account's home too, not keep it", &g_delete, 0 },
    { 0 },
};

static const struct uargs_cmd CMDS[] = {
    { "list",   0,               "every person's account: name, full name, type, home (also a bare user)" },
    { "add",    "NAME",          "add an account; its home is made from /etc/skel" },
    { "remove", "NAME",          "remove an account; its home is kept unless -r" },
    { "set",    "NAME WHAT [VALUE]", "change one thing: admin, standard, name FULL-NAME, or password" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "user",
    .usage = "[OPTION]... [COMMAND [NAME]...]",
    .summary = "List, add, remove and change the people's accounts -- what Settings >\n"
               "System > Users does, from a terminal.",
    .opts = OPTS,
    .cmds = CMDS,
    .notes = "Every change is an administrator's: privd asks for your password here, as\n"
             "sudo does, or on the desktop's prompt when your account has none. A NAME\n"
             "is 1-32 lower-case letters, digits, '_' or '-', starting with a letter.\n"
             "Exit status: 0 done, 1 refused or failed, 2 bad usage.",
    .shorts_after_command = 1,
};

static int fail(const char *what, int e) {
    const char *why = e == EEXIST  ? "that name is taken"
                    : e == EBUSY   ? "the account is signed in, or has programs running"
                    : e == EMLINK  ? "it is the last administrator"
                    : e == ENOENT  ? "no such account"
                    : e == EINVAL  ? "not a name an account can have"
                    : e == E2BIG   ? "the full name is too long to show in the prompt"
                    : upriv_strerror(e);
    fprintf(stderr, "user: %s: %s\n", what, why);
    return 1;
}

static int cmd_list(void) {
    struct uuser *u;
    int n = uusers_list(&u);
    if (n < 0) return fail("list", errno);
    printf("%-16s %-24s %-14s %s\n", "NAME", "FULL NAME", "TYPE", "HOME");
    for (int i = 0; i < n; i++)
        printf("%-16s %-24s %-14s %s\n", u[i].name, u[i].real, u[i].admin ? "administrator" : "standard",
               u[i].home);
    uusers_free(u, n);
    return 0;
}

// A new password, twice, into `out` (cap bytes): 0, or -1 when they
// differ or the input ended.
static int ask_new(char *out, size_t cap) {
    char *again = malloc(cap);
    if (!again) return -1;
    int rc = -1;
    if (uaskpass("New password: ", out, cap) >= 0 && uaskpass("Retype new password: ", again, cap) >= 0) {
        if (!strcmp(out, again)) rc = 0;
        else fprintf(stderr, "user: the passwords do not match\n");
    }
    memset(again, 0, cap);
    free(again);
    return rc;
}

static int cmd_add(const char *name) {
    if (!uusers_name_ok(name)) return fail(name, EINVAL);
    if (getpwnam(name)) return fail(name, EEXIST);
    static char pw[256];
    if (g_password && ask_new(pw, sizeof pw) < 0) return 1;
    int rc = uusers_add(name, g_full ? g_full : "", g_admin != 0, pw, 1);
    int e = errno;
    memset(pw, 0, sizeof pw);
    if (rc < 0) return fail(name, e);
    printf("user: added %s (%s), home /home/%s\n", name, g_admin ? "administrator" : "standard", name);
    return 0;
}

static int cmd_remove(const char *name) {
    if (!getpwnam(name)) return fail(name, ENOENT);
    if (uusers_remove(name, g_delete != 0, 1) < 0) return fail(name, errno);
    printf("user: removed %s%s\n", name, g_delete ? " and its files" : "; its files are kept");
    return 0;
}

static int cmd_set(int argc, char **argv) {
    const char *name = argv[1], *what = argv[2];
    if (!getpwnam(name)) return fail(name, ENOENT);
    int rc;
    if (!strcmp(what, "admin") || !strcmp(what, "standard")) {
        if (argc != 3) return uargs_error(&PROG, "'set NAME %s' takes nothing more", what);
        rc = uusers_set(name, "admin", !strcmp(what, "admin") ? "1" : "0", 1);
    } else if (!strcmp(what, "name")) {
        if (argc != 4) return uargs_error(&PROG, "'set NAME name' takes the FULL-NAME");
        if (!uusers_full_ok(argv[3])) return fail(name, EINVAL);
        rc = uusers_set(name, "real", argv[3], 1);
    } else if (!strcmp(what, "password")) {
        if (argc != 3) return uargs_error(&PROG, "'set NAME password' asks for it; it takes nothing more");
        static char pw[256];
        if (ask_new(pw, sizeof pw) < 0) return 1;
        rc = uusers_set(name, "password", pw, 1);
        int e = errno;
        memset(pw, 0, sizeof pw);
        errno = e;
    } else {
        return uargs_error(&PROG, "unknown 'set' item '%s' (admin, standard, name, password)", what);
    }
    if (rc < 0) return fail(name, errno);
    printf("user: %s changed\n", name);
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    const char *cmd = a.argc ? a.argv[0] : "list";
    if (!strcmp(cmd, "list")) {
        if (a.argc > 1) return uargs_error(&PROG, "'list' takes no arguments");
        return cmd_list();
    }
    if (!strcmp(cmd, "set")) {
        if (a.argc < 3) return uargs_error(&PROG, "'set' takes NAME WHAT [VALUE]");
        return cmd_set(a.argc, a.argv);
    }
    if (a.argc != 2) return uargs_error(&PROG, "'%s' takes one NAME", cmd);
    if (!strcmp(cmd, "add")) return cmd_add(a.argv[1]);
    if (!strcmp(cmd, "remove")) return cmd_remove(a.argv[1]);
    return uargs_error(&PROG, "unknown command '%s'", cmd);
}
