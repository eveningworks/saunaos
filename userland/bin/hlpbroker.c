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
// user's, as with Windows Help.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"

#define VIEWER "/bin/wm/apps/hlpview"
#define LINE_MAX_BYTES 4096

static int g_child;

static void close_child(void) {
    if (g_child > 0) sys_kill(g_child, 15);
    int code;
    while (g_child > 0 && sys_waitpid_nohang(g_child, &code) == SYS_RETRY) {
        usleep(10000);
        break;
    }
    g_child = 0;
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
        if (!strcmp(line, "QUIT")) { close_child(); continue; }
        char *argv[16];
        if (!build(line, argv, 16)) continue;
        close_child();
        g_child = sys_spawn_argv(VIEWER, argv);
    }
    return 0;
}
