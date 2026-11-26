// A folder path past 255 bytes, through the shell and ls (fixed-tables
// round 4, track A). /bin/tosh runs on a pty INSIDE the folder, as a
// person meets it: its prompt and title name the whole path, `pwd`
// prints it, a bare `ls` lists that folder (not `/`), and a program is
// run by a path that long. Then `ls -R` walks down to it, and
// getcwd(NULL, 0) answers it whole. Builds against the old tree too (no
// new names), which is how it was seen red.
//
// The old 64-byte budgets failed each of these SILENTLY: the prompt fell
// back to `/`, `pwd` printed `?`, a bare `ls` listed `/`.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "rt/sys.h"
#include "tty_abi.h"
#include "signal_abi.h"   // SIGKILL
#include "base/utest.h"

// Eight 40-byte components under the scratch folder: well past 255.
#define LEVELS 8
#define PART "deep_0123456789abcdefghijklmnopqrstuvw"   // 38 + "_N"

static char *g_deep;           // the deepest folder, heap

static char *join(const char *dir, const char *name) {
    char *p = 0;
    return asprintf(&p, "%s/%s", dir, name) < 0 ? 0 : p;
}

// Everything tosh wrote, never cleared, GROWN: the line editor repaints
// the prompt and line on every key, so typing after a 341-byte prompt
// writes a few hundred KB.
static char *acc;
static int acc_len, acc_cap;

// Waits for `needle` at or after `from`; the position just past it, or -1.
static int waited(int master, const char *needle, int from) {
    int nl = (int)strlen(needle);
    for (int tries = 0; tries < 400; tries++) {
        for (;;) {
            if (acc_cap - acc_len < 4097) {
                char *g = realloc(acc, (size_t)acc_cap * 2 + 8192);
                if (!g) break;
                acc = g;
                acc_cap = acc_cap * 2 + 8192;
            }
            int n = (int)sys_read(master, acc + acc_len, 4096);
            if (n <= 0) break;
            acc_len += n;
            acc[acc_len] = 0;
        }
        for (int i = from; i + nl <= acc_len; i++)
            if (!memcmp(acc + i, needle, (size_t)nl)) return i + nl;
        sys_sleep_ms(20);
    }
    return -1;
}

static int put(int fd, const char *s) {
    size_t n = strlen(s);
    return sys_write(fd, s, n) == (long)n ? 0 : -1;
}

// Copies /bin/echo into the deep folder, so a program has a long path.
static int copy_echo(const char *to) {
    int in = sys_open("/bin/echo", 0);
    if (in < 0) return 0;
    int out = sys_open(to, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (out < 0) { sys_close(in); return 0; }
    static char buf[4096];
    long n;
    int ok = 1;
    while ((n = sys_read(in, buf, sizeof buf)) > 0)
        if (sys_write(out, buf, (size_t)n) != n) { ok = 0; break; }
    sys_close(in);
    sys_close(out);
    return ok && n == 0;
}

static void shell_checks(void) {
    int master = -1, slave = -1;
    if (sys_openpty(&master, &slave) < 0 || sys_set_nonblock(master, 1) < 0) {
        utest_check(0, "a pty for the shell");
        return;
    }
    // Wide enough that the prompt does not wrap: a wrap is the line
    // editor's business, not this test's.
    struct tty_winsize ws = { 50, 1000 };
    sys_tcsetwinsz(master, &ws);

    if (chdir(g_deep) != 0) {
        utest_check(0, "chdir into the deep folder");
        sys_close(master);
        sys_close(slave);
        return;
    }
    int saved_in = sys_dup(0), saved_out = sys_dup(1), saved_err = sys_dup(2);
    sys_dup2(slave, 0);
    sys_dup2(slave, 1);
    sys_dup2(slave, 2);
    int pid = sys_spawn_group("/bin/tosh", 0, -1, 0, PGID_NEW);
    sys_dup2(saved_in, 0);  sys_close(saved_in);
    sys_dup2(saved_out, 1); sys_close(saved_out);
    sys_dup2(saved_err, 2); sys_close(saved_err);
    chdir("/");
    if (pid < 0) { utest_check(0, "/bin/tosh starts on the pty"); return; }
    sys_tcsetpgrp(master, pid);

    char *prompt = 0, *want = 0;
    asprintf(&prompt, "%s$ ", g_deep);
    int mark = waited(master, prompt, 0);
    utest_checkf(mark >= 0, "the prompt names the whole %d-byte folder", (int)strlen(g_deep));
    asprintf(&want, "\033]0;%s\007", g_deep);
    utest_check(waited(master, want, 0) >= 0, "the terminal title names it whole");
    free(want);
    if (mark < 0) mark = 0;

    put(master, "pwd\n");
    asprintf(&want, "\n%s\r\n", g_deep);
    int at = waited(master, want, mark);
    utest_check(at >= 0, "pwd prints the whole path");
    free(want);
    if (at > 0) mark = at;
    // THE PROMPT BACK before the next line: until tosh's editor has the
    // terminal again it is in canonical mode, which drops a line over
    // TTY_CANON_MAX whole -- and the next line is longer than that.
    if ((at = waited(master, prompt, mark)) > 0) mark = at;

    put(master, "ls\n");
    at = waited(master, "marker_deeppath", mark);
    utest_check(at >= 0, "a bare ls lists the deep folder, not /");
    if (at > 0) mark = at;
    if ((at = waited(master, prompt, mark)) > 0) mark = at;

    // A program by its full path: the PATH lookup once cut it at 64.
    char *line = 0;
    // `a$?b` prints a0b: the typed line's echo cannot match it.
    asprintf(&line, "%s/echo_copy a$?b\n", g_deep);
    put(master, line);
    free(line);
    at = waited(master, "a0b", mark);
    utest_check(at >= 0, "a program runs by a path past 255 bytes");

    // Asked to exit, then made to: a shell that hangs must not hang the
    // test (whether it exits is not what this asserts).
    free(prompt);
    put(master, "exit\n");
    sys_sleep_ms(300);
    sys_kill(pid, SIGKILL);
    int code = 0;
    sys_waitpid(pid, &code);
    sys_close(master);
    sys_close(slave);
}

int main(void) {
    utest_begin("deeppath_test", "a path past 255 bytes in the shell and ls", UTEST_VERDICT_FILE);
    const char *base = utest_path(TMP_VOLATILE, "deeppath");
    char *rm[] = { "/bin/rm", "-r", (char *)base, 0 };
    int code = 0, rp = sys_spawn_argv("/bin/rm", rm);
    if (rp > 0) sys_waitpid(rp, &code);

    g_deep = strdup(base);
    sys_mkdir(g_deep);
    for (int i = 0; i < LEVELS && g_deep; i++) {
        char part[48];
        snprintf(part, sizeof part, "%s_%d", PART, i);
        char *next = join(g_deep, part);
        free(g_deep);
        g_deep = next;
        if (g_deep && sys_mkdir(g_deep) < 0) { utest_check(0, "mkdir a deep level"); return utest_end(); }
    }
    if (!g_deep) { utest_check(0, "memory for the path"); return utest_end(); }
    utest_checkf(strlen(g_deep) > 300, "the folder path is %d bytes", (int)strlen(g_deep));

    char *marker = join(g_deep, "marker_deeppath");
    int fd = sys_open(marker, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd >= 0) sys_close(fd);
    utest_check(fd >= 0, "a file in the deep folder");
    char *prog = join(g_deep, "echo_copy");
    utest_check(copy_echo(prog), "a program copied into it");

    // getcwd(NULL, 0) and the lookup, in this process.
    chdir(g_deep);
    char *cwd = getcwd(0, 0);
    utest_checkf(cwd && !strcmp(cwd, g_deep), "getcwd(NULL, 0) answers the whole path (%d bytes)",
                 cwd ? (int)strlen(cwd) : -1);
    free(cwd);
    chdir("/");
    shell_checks();

    // ls -R from the top: the deepest folder's header, whole.
    char *out = 0;
    asprintf(&out, "%s.lsR", base);
    int ofd = sys_open(out, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (ofd >= 0) {
        int keep = sys_dup(1);
        sys_dup2(ofd, 1);
        char *ls[] = { "/bin/ls", "-R", (char *)base, 0 };
        int lp = sys_spawn_argv("/bin/ls", ls);
        sys_dup2(keep, 1);
        sys_close(keep);
        sys_close(ofd);
        if (lp > 0) sys_waitpid(lp, &code);
    }
    static char got[8192];
    int rfd = sys_open(out, 0), n = rfd >= 0 ? (int)sys_read(rfd, got, sizeof got - 1) : 0;
    if (rfd >= 0) sys_close(rfd);
    got[n > 0 ? n : 0] = 0;
    char *header = 0;
    asprintf(&header, "\n%s:\n", g_deep);
    utest_check(strstr(got, header) && strstr(got, "marker_deeppath"),
                "ls -R reaches the deepest folder by its whole path");
    free(header);
    free(out);
    free(marker);
    free(prog);
    return utest_end();
}
