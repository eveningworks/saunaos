// tosh's exit status, through the library a shell runs lines with: a
// failed command prints no `[exit N]` of its own, `$?` expands to the
// last line's status (outside quotes and in double ones, never in
// single), and every line sets it -- a builtin's too. Each `$?` is read
// back as BYTES, from a file the line's `echo` wrote, so the check sees
// what a program was handed.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "base/tosh.h"
#include "base/utmppath.h"
#include "base/utest.h"

static char g_out[4096];   // what the shell itself printed, this line
static int g_len;

static void capture(void *ctx, const char *text, int len) {
    (void)ctx;
    if (g_len + len >= (int)sizeof g_out) len = (int)sizeof g_out - 1 - g_len;
    memcpy(g_out + g_len, text, (size_t)len);
    g_len += len;
    g_out[g_len] = 0;
}

static struct tosh g_sh;
static char g_file[256];

// Runs `line`; 1 when the shell printed nothing that names a status.
static int run(const char *line) {
    g_len = 0;
    g_out[0] = 0;
    tosh_run_line(&g_sh, line);
    return !strstr(g_out, "[exit") && !strstr(g_out, "exit ");
}

// `echo WORDS > file`, then the file: what `$?` expanded to.
static const char *echoed(const char *words) {
    static char buf[128];
    char line[384];
    snprintf(line, sizeof line, "echo %s > %s", words, g_file);
    run(line);
    buf[0] = 0;
    int fd = open(g_file, O_RDONLY);
    if (fd >= 0) {
        int n = (int)read(fd, buf, sizeof buf - 1);
        buf[n > 0 ? n : 0] = 0;
        close(fd);
    }
    buf[strcspn(buf, "\n")] = 0;
    return buf;
}

int main(void) {
    utest_begin("toshstatus_test", "tosh prints no status; $? holds it", UTEST_VERDICT_FILE);
    tosh_init(&g_sh, capture, 0);
    snprintf(g_file, sizeof g_file, "%s", utest_path(TMP_VOLATILE, "toshstatus.out"));
    const char *got;

    // A builtin that fails: its own message, no status line, and 1.
    int quiet = run("cd /no/such/folder");
    utest_checkf(quiet && strstr(g_out, "cd:"), "a failed cd says why and prints no status (%s)", g_out);
    got = echoed("$?");
    utest_checkf(!strcmp(got, "1"), "$? after a failed cd is 1 (got '%s')", got);

    // An external program that fails: nothing from the shell at all.
    quiet = run("/bin/grep nothing-here /no/such/file");
    utest_checkf(quiet, "a failed program gets no status line from the shell (%s)", g_out);
    got = echoed("$?");
    utest_checkf(!strcmp(got, "2"), "$? after grep on a missing file is 2 (got '%s')", got);

    // Quoting: single quotes keep it, double quotes expand it.
    run("/bin/grep nothing-here /no/such/file");
    got = echoed("'$?' \"s=$?\" x$?y");
    utest_checkf(!strcmp(got, "$? s=2 x2y"), "'$?' stays, \"s=$?\" and x$?y expand (got '%s')", got);

    // A builtin that succeeds sets it too, and so does a missing command.
    run("cd /no/such/folder");
    run("pwd");
    got = echoed("$?");
    utest_checkf(!strcmp(got, "0"), "$? after pwd is 0, not the earlier failure's (got '%s')", got);
    quiet = run("no-such-command-here");
    utest_checkf(quiet && strstr(g_out, "not found"), "a missing command says not found, no status (%s)", g_out);
    got = echoed("$?");
    utest_checkf(!strcmp(got, "127"), "$? after a missing command is 127 (got '%s')", got);

    // A pipeline's status is its LAST stage's, even one that never ran.
    run("echo hi | no-such-command-here");
    got = echoed("$?");
    utest_checkf(!strcmp(got, "127"), "$? after `echo hi | missing` is 127 (got '%s')", got);
    run("/bin/grep nothing-here /no/such/file | pwd");
    got = echoed("$?");
    utest_checkf(!strcmp(got, "0"), "$? after `failing | pwd` is pwd's 0 (got '%s')", got);

    // A backslash keeps it literal inside double quotes, as in POSIX.
    run("/bin/grep nothing-here /no/such/file");
    got = echoed("\"\\$? $?\"");
    utest_checkf(!strcmp(got, "$? 2"), "\"\\$? $?\" is '$? 2' (got '%s')", got);

    unlink(g_file);
    return utest_end();
}
