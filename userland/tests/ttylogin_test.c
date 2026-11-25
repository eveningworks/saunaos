// /bin/login on a terminal (users D2c): run on a pty as telnetd runs it,
// with no password it starts the person's shell at once (`-f`); with one
// it asks `login:` and `Password:`, refuses a wrong password and starts
// the shell -- as the ACCOUNT, not root -- for the right one.
//
// What a broken version would still pass, and what stops it: a login that
// let anyone in passes the right password, so a wrong one must read
// "Login incorrect"; one that started the shell as root passes "a shell
// came", so the shell's own `id` must say uid=1000; a `-f` that skipped
// the password even when one is set passes the first half, so `-f` is
// asked again once there is one and must PROMPT.
//
// SPAWNED by tools/tests/gate/usertest_run.py as root. It removes the
// password it sets.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "base/upriv.h"
#include "base/usetting.h"
#include "base/utest.h"

#define PW "tty-pw-1"

static int g_master = -1, g_pid = -1;

// login -f user (or plain login) on a fresh pty, as telnetd starts it.
static int start(const char *args) {
    int slave = -1;
    if (sys_openpty(&g_master, &slave) < 0) return -1;
    fcntl(g_master, F_SETFL, O_NONBLOCK);
    int in0 = dup(0), out1 = dup(1), err2 = dup(2);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    g_pid = sys_spawn_flags("/bin/login", args, -1, environ, PGID_NEW, SPAWN_SETSID);
    dup2(in0, 0);
    dup2(out1, 1);
    dup2(err2, 2);
    close(in0);
    close(out1);
    close(err2);
    close(slave);
    return g_pid;
}

static void stop(void) {
    if (g_pid > 0) {
        sys_kill(g_pid, 9);
        int code;
        sys_waitpid(g_pid, &code);
    }
    if (g_master >= 0) close(g_master);
    g_master = g_pid = -1;
}

// Everything the terminal says until `want` appears or `ms` pass; 1 if it did.
static char g_out[4096];
static int expect(const char *want, int ms) {
    size_t n = strlen(g_out);
    for (int waited = 0; waited < ms; waited += 50) {
        if (strstr(g_out, want)) return 1;
        int k = (int)read(g_master, g_out + n, sizeof g_out - 1 - n);
        if (k > 0) { n += (size_t)k; g_out[n] = '\0'; continue; }
        sys_sleep_ms(50);
    }
    return strstr(g_out, want) != 0;
}

static void say(const char *line) {
    g_out[0] = '\0';
    write(g_master, line, strlen(line));
}

int main(void) {
    utest_begin("ttylogin_test", "/bin/login on a terminal: no password, a wrong one, the right one",
                UTEST_VERDICT_FILE);
    upriv_passwd("user", 0, "");   // from no password, whatever an earlier run left
    // SETTING A PASSWORD TURNS AUTOMATIC SIGN-IN OFF (privd): put back what
    // it was, or every later boot of this image stops at the login screen.
    char autologin[64] = "";
    usetting_get("login.autologin", autologin, sizeof autologin);

    g_out[0] = '\0';
    int pid = start("-f user");
    utest_check(pid > 0, "login starts on a pty");
    say("id\n");
    utest_checkf(expect("uid=1000", 15000), "with no password, -f user is a shell at once, as user (%s)",
                 g_out[0] ? "it answered" : "silence");
    stop();

    utest_check(upriv_passwd("user", 0, PW) == 0, "root sets user's password");
    g_out[0] = '\0';
    start("-f user");
    utest_check(expect("login:", 15000), "with one, even -f user asks login:");
    say("user\n");
    utest_check(expect("Password:", 10000), "...then Password:");
    say("wrong\n");
    utest_check(expect("Login incorrect", 15000), "a wrong password: Login incorrect");
    utest_check(expect("login:", 10000), "...and it asks again");
    say("user\n");
    expect("Password:", 10000);
    say(PW "\n");
    sys_sleep_ms(1500);
    say("id\n");
    utest_checkf(expect("uid=1000", 15000), "the right one starts the shell as user, not root");
    // ROOT FROM THERE, as tools/dev/remote.py makes its session: sudo's
    // password, then a root shell that takes this terminal's keys.
    say("sudo /bin/tosh\n");
    utest_check(expect("password for user", 10000), "sudo /bin/tosh asks for user's password");
    say(PW "\n");
    sys_sleep_ms(1500);
    say("id\n");
    utest_checkf(expect("uid=0", 15000), "...and the root shell answers on this terminal (%.200s)", g_out);
    stop();

    utest_check(upriv_passwd("user", 0, "") == 0, "the password is removed again");
    utest_check(usetting_set("login.autologin", autologin) == SETTING_SAVED, "automatic sign-in is put back");
    return utest_end();
}
