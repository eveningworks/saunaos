// login -- sign in at a text terminal: an account and its password, then
// that account's shell (users D2c, docs/users-design.md). Linux's login(1)
// in the place getty and telnetd start it, run as ROOT: it checks nothing
// itself -- privd does (base/upriv.h) -- and starts the shell with the
// account's identity and login variables (base/ulogin.h).
//
// `-f ACCOUNT` signs that account in without asking WHEN IT HAS NO
// PASSWORD (agetty's --autologin, made opt-in by the password): a machine
// nobody set one on keeps today's shells.
#include "base/uargs.h"
#include "base/uaskpass.h"
#include "base/ulogin.h"
#include "base/upriv.h"
#include "base/usetting.h"
#include "rt/sys.h"
#include "cred_abi.h"
#include "setting_abi.h"
#include "syscall_abi.h"   // SPAWN_FD_LOG
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int f_seat;
static const char *f_auto;

static const struct uargs_opt OPTS[] = {
    { "seat", 0, 0, "the shell holds the seat: this terminal is AT the machine", &f_seat, 0 },
    { 0, 'f', "ACCOUNT", "sign ACCOUNT in without asking, when it has no password", 0, &f_auto },
    { 0 },
};

static const struct uargs_prog PROG = {
    "login", "[--seat] [-f ACCOUNT]",
    "Ask for an account and its password on this terminal, then start that\n"
    "account's shell.",
    OPTS, 0,
    "Run as root -- by init on a text terminal with no session, and by\n"
    "telnetd for each connection. privd checks the password.",
    0, 0, 0, 0,
};

// THE ACCOUNT'S POINTER, KEYBOARD AND FONTS for a text session at the
// machine (users D3c): `config session` run AS the account, which reads
// its ~/.config -- this process is root, and root reads no person's file.
static int settings_held(void) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_PERSON_COUNT;
    return sys_setting(&m) == 0 && m.count > 0;
}

static void apply_settings(const char *acct) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    struct ulogin_creds c;
    if (ulogin_creds(acct, CRED_SEAT, &o, &c) < 0) return;
    char *argv[] = { "/bin/config", "session", 0 };
    o.argv = argv;
    o.flags |= SPAWN_ARGV;
    o.stdout_fd = SPAWN_FD_LOG;
    int pid = sys_spawn_opts(argv[0], &o);
    ulogin_creds_release(&c);
    int code;
    if (pid > 0) sys_waitpid(pid, &code);
}

// ...and the machine's back when the shell is gone. ONE KEYBOARD: the
// first sign-in at the machine sets it, as Linux's console has one keymap.
static void revert_settings(void) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_REVERT;
    sys_setting(&m);
}

// The account's shell on this terminal, as the account; its exit code.
// A session's values already live -- the desktop's, locked, or another
// text sign-in's -- are left as they are, and not reverted after; this
// sign-in reverts only what it applied. A desktop with no values of its
// own looks like none, so a VT sign-in behind its lock applies its own
// until it signs out (one keyboard, as above).
static int run_shell(const char *acct) {
    int mine = 0;
    if (f_seat && !settings_held()) {
        apply_settings(acct);
        mine = settings_held();
    }
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    struct ulogin_creds c;
    if (ulogin_creds(acct, f_seat ? CRED_SEAT : 0, &o, &c) < 0) {
        fprintf(stderr, "login: %s cannot sign in here\n", acct);
        if (mine) revert_settings();
        return 1;
    }
    // Its own group, made the terminal's FOREGROUND one, so Ctrl-C reaches
    // its jobs; started in its home, as login(1) chdir()s to pw_dir.
    o.pgid = PGID_NEW;
    if (chdir(c.home) < 0) chdir("/");
    char *argv[] = { c.shell, 0 };
    o.argv = argv;
    o.flags |= SPAWN_ARGV;
    int pid = sys_spawn_opts(c.shell, &o);
    chdir("/");
    ulogin_creds_release(&c);
    if (pid < 0) {
        fprintf(stderr, "login: cannot start %s's shell\n", acct);
        if (mine) revert_settings();
        return 1;
    }
    sys_tcsetpgrp(0, pid);
    int code = 1;
    sys_waitpid(pid, &code);
    if (mine) revert_settings();
    return code;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (geteuid() != 0) { fprintf(stderr, "login: run as root\n"); return 1; }
    sys_notify_ready();   // a service's readiness: the prompt is up

    if (f_auto && upriv_has_password(f_auto) == 0) return run_shell(f_auto);

    char host[64] = "";
    if (!usetting_get("system.hostname", host, sizeof host) || !host[0]) snprintf(host, sizeof host, "saunaos");
    char *acct = 0;
    size_t acap = 0;
    for (;;) {
        char pw[256];   // PAM_MAX_RESP_SIZE's order; uaskpass refuses a longer one
        printf("\n%s login: ", host);
        fflush(stdout);
        if (getline(&acct, &acap, stdin) < 0) return 1;
        acct[strcspn(acct, "\r\n")] = '\0';
        if (!acct[0]) continue;
        if (uaskpass("Password: ", pw, sizeof pw) < 0) return 1;
        int rc = upriv_auth(acct, pw);
        memset(pw, 0, sizeof pw);
        // Wrong, unknown and refused all read the same, as login(1)'s do:
        // the terminal does not say which accounts exist.
        if (rc == 0) return run_shell(acct);
        printf(errno == EAGAIN ? "Wait a moment, then try again.\n" : "Login incorrect\n");
    }
}
