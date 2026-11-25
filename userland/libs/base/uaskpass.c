// A password at a terminal -- see uaskpass.h.
#include "base/uaskpass.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved;
static struct sigaction g_old_int, g_old_term;

// The terminal back as it was, then the signal as the program had it.
static void restore_and_raise(int sig) {
    tcsetattr(0, TCSANOW, &g_saved);
    sigaction(SIGINT, &g_old_int, 0);
    sigaction(SIGTERM, &g_old_term, 0);
    (void)!write(2, "\n", 1);   // not stdio: this runs inside a signal
    raise(sig);
}

int uaskpass(const char *prompt, char *out, size_t cap) {
    if (!out || cap == 0) { errno = EINVAL; return -1; }
    int tty = isatty(0) && tcgetattr(0, &g_saved) == 0;
    if (tty) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = restore_and_raise;
        sigaction(SIGINT, &sa, &g_old_int);
        sigaction(SIGTERM, &sa, &g_old_term);
        struct termios quiet = g_saved;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSANOW, &quiet);
        if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    }
    size_t n = 0;
    int over = 0, got = 0;
    char c;
    while (read(0, &c, 1) == 1) {
        got = 1;
        if (c == '\n' || c == '\r') break;
        if (n + 1 < cap) out[n++] = c;
        else over = 1;
    }
    out[n] = '\0';
    if (tty) {
        tcsetattr(0, TCSANOW, &g_saved);
        sigaction(SIGINT, &g_old_int, 0);
        sigaction(SIGTERM, &g_old_term, 0);
        fputs("\n", stderr);
    }
    if (over) {
        memset(out, 0, cap);
        errno = E2BIG;
        return -1;
    }
    if (!got) return -1;
    return (int)n;
}
