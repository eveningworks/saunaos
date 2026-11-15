// hlpbroker -- opens Help Viewer for a Windows program that may not spawn.
//
// w16 runs a Windows program in a sandbox with no right to start another
// process, and a program's F1 has to open a window. So w16 starts this
// BEFORE it closes its own sandbox and keeps the write end of the pipe
// that is this process's standard input; WinHelp() then writes a line.
// What this will do for whatever is on the pipe is exactly one thing --
// start /bin/wm/apps/hlpview, with options it checks -- so a w16 that has
// been taken over can open a help file, and nothing else. (The viewer
// pledges itself, so a help file's bytes are read in a sandbox.)
//
// A line is tab-separated fields. `OPEN`, then any of `-c` NUMBER,
// `-k` KEYWORD, `-m` MACRO, then the help file's path; or `QUIT`. A new
// OPEN closes the window this started before, as a second WinHelp() call
// to the same Help window replaces its topic. It exits when the pipe
// closes (the program ended) and leaves the window: closing it is the
// user's, as with Windows Help. A line may instead be `WINHELP`, the path of
// WINHELP.EXE, an optional `-i`ID and the file's path: it starts w16 on that
// program, for a machine that has Windows' own Help and is set to use it.
//
// It also SAVES THE PROGRAM'S OWN SCREEN EFFECT, which the sandbox keeps
// w16 from writing: the FIRST line is `PROGRAM` and the program's path,
// written by w16 before its sandbox closes and never taken again, and
// `EFFECT` with off, a preset's word (ucrt_effect_word) or custom then sets
// that program's eight effect keys and nothing else (w16_prefs_save_effect). A w16 taken
// over can change its own program's look -- not its drive C:, not another's.
//
// IT ANSWERS ON ITS STANDARD OUTPUT WHEN A HELP WINDOW CLOSES, so the program's FindWindow can wait
// on it as on Windows (WinBench waits for its licence's window to come and go). Each OPEN and
// WINHELP line is numbered from 1 in the order received, and `CLOSED\tN` says window N is gone --
// at once for a line this refused or a start that failed; a newer N is the one still open.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include "rt/sys.h"
#include "common/w16prefs.h"

#define VIEWER "/bin/wm/apps/hlpview"
#define LINE_MAX_BYTES 4096

static int g_child;
static unsigned g_seq;                  // OPEN and WINHELP lines received

static void report_closed(unsigned seq) {
    char out[24];
    int n = snprintf(out, sizeof out, "CLOSED\t%u\n", seq);
    if (write(1, out, (size_t)n) < 0) { /* the program ended: nobody to tell */ }
}

// One per window started: reaps it, then says it is gone. The only reaper of its child.
struct waiter { int pid; unsigned seq; };
static void *wait_child(void *arg) {
    struct waiter wt = *(struct waiter *)arg;
    free(arg);
    int code;
    sys_waitpid(wt.pid, &code);
    report_closed(wt.seq);
    return 0;
}

static void watch_child(int pid, unsigned seq) {
    struct waiter *wt = malloc(sizeof *wt);
    pthread_t t;
    if (wt) { wt->pid = pid; wt->seq = seq; }
    if (!wt || pthread_create(&t, 0, wait_child, wt) != 0) { free(wt); return; }
    pthread_detach(t);
}

// Closes the window this started last; its waiter reaps it and reports.
static void close_child(void) {
    if (g_child > 0) sys_kill(g_child, 15);
    g_child = 0;
}

#define W16BIN "/bin/w16"

// `WINHELP`, the program's path, an optional `-i`ID and the help file's: Windows' own WINHELP.EXE under
// w16 -- argv for /bin/w16, or 0. The program must be named winhelp.exe, the ID is letters, the file is
// an absolute path: w16 is itself sandboxed, and this opens that one program on that one kind of file.
static int build_winhelp(char *line, char **argv, int max) {
    int n = 0;
    argv[n++] = (char *)W16BIN;
    char *f = strtok(line, "\t");
    if (!f || strcmp(f, "WINHELP")) return 0;
    char *exe = strtok(NULL, "\t");
    if (!exe || exe[0] != '/') return 0;
    const char *base = strrchr(exe, '/') + 1;
    if (strcasecmp(base, "winhelp.exe")) return 0;
    argv[n++] = exe;
    argv[n++] = (char *)"--";
    while ((f = strtok(NULL, "\t")) != NULL) {
        if (n + 2 >= max) return 0;
        if (f[0] == '-' && f[1] == 'i') {
            size_t k = 2;
            while (f[k] >= 'a' && f[k] <= 'z' && k < 40) k++;
            if (k == 2 || f[k]) return 0;
            argv[n++] = f;
        } else if (f[0] == '/') {
            argv[n++] = f;
        } else {
            return 0;
        }
    }
    argv[n] = NULL;
    return n >= 4 && argv[n - 1][0] == '/';
}

// Fields to argv for the viewer, or 0 if the line is not one this will run.
static int build(char *line, char **argv, int max) {
    int n = 0;
    argv[n++] = (char *)VIEWER;
    char *f = strtok(line, "\t");
    if (!f || strcmp(f, "OPEN")) return 0;
    while ((f = strtok(NULL, "\t")) != NULL) {
        if (n + 3 >= max) return 0;
        if (!strcmp(f, "-c") || !strcmp(f, "-k") || !strcmp(f, "-m")) {
            char *v = strtok(NULL, "\t");
            if (!v) return 0;
            argv[n++] = f;
            argv[n++] = v;
        } else if (f[0] == '/') {
            argv[n++] = f;                          // the help file: an absolute path
        } else {
            return 0;
        }
    }
    argv[n] = NULL;
    return n >= 2 && argv[n - 1][0] == '/';
}

int main(void) {
    // NOT SANDBOXED, on purpose: a child inherits its parent's promises and path list
    // (abi/sandbox_abi.h), so a broker that pledged would leave the viewer without a window. What
    // keeps it harmless is how little it does -- one program, checked options, a bounded line.
    static char line[LINE_MAX_BYTES];
    static char program[W16_PATH_MAX];       // set by the first line only
    int first = 1;
    size_t len = 0;
    for (;;) {
        char c;
        long r = read(0, &c, 1);
        if (r <= 0) break;                           // the program ended
        if (c != '\n') {
            if (len + 1 < sizeof line) line[len++] = c;
            continue;
        }
        line[len] = 0;
        len = 0;
        int was_first = first;
        first = 0;
        if (was_first && !strncmp(line, "PROGRAM\t/", 9) && strlen(line + 8) < sizeof program) {
            snprintf(program, sizeof program, "%s", line + 8);
            continue;
        }
        if (!strncmp(line, "EFFECT\t", 7)) {
            if (program[0] && w16_prefs_save_effect(program, line + 7) < 0)
                fprintf(stderr, "hlpbroker: could not save %s's screen effect\n", program);
            continue;
        }
        if (!strcmp(line, "QUIT")) { close_child(); continue; }
        char *argv[16];
        int winhelp = !strncmp(line, "WINHELP\t", 8);
        if (!winhelp && strncmp(line, "OPEN\t", 5)) continue;
        unsigned seq = ++g_seq;
        if (!(winhelp ? build_winhelp(line, argv, 16) : build(line, argv, 16))) { report_closed(seq); continue; }
        close_child();
        g_child = sys_spawn_argv(winhelp ? W16BIN : VIEWER, argv);
        if (g_child > 0) watch_child(g_child, seq);
        else report_closed(seq);
    }
    return 0;
}
