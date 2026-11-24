// The yes/no question described in uask.h.
#include "uask.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// 1 yes, 0 no, -1 neither. Leading and trailing blanks are ignored.
static int answer(const char *s, int dflt) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strcspn(s, " \t\r\n");
    if (!n) return dflt;
    char w[4];
    if (n >= sizeof w) return -1;
    for (size_t i = 0; i < n; i++) w[i] = (char)tolower((unsigned char)s[i]);
    w[n] = '\0';
    if (!strcmp(w, "y") || !strcmp(w, "yes")) return 1;
    if (!strcmp(w, "n") || !strcmp(w, "no")) return 0;
    return -1;
}

int uask_yes_no(const char *question, int dflt) {
    if (!isatty(0) || !isatty(1)) return UASK_NO_TERMINAL;
    for (;;) {
        printf("%s %s ", question, dflt ? "[Y/n]" : "[y/N]");
        fflush(stdout);
        char reply[32];
        if (!fgets(reply, sizeof reply, stdin)) { printf("\n"); return 0; }
        if (!strchr(reply, '\n')) {   // longer than the buffer: no answer, and the rest goes
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            continue;
        }
        int a = answer(reply, dflt);
        if (a >= 0) return a;
    }
}
