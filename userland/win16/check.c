// check.c -- asking w16 (`--check`, `--fonts`), and the short name a
// program goes by (win16.h).
#include "win16/win16.h"

#include <stdlib.h>
#include <string.h>
#include "lib/ucapture.h"

static char *ask(char **argv) { return ucapture(W16_BIN, argv); }

char *win16_check(const char *path) {
    char *argv[] = { "w16", "--check", (char *)path, 0 };
    return ask(argv);
}

char *win16_fonts(void) {
    char *argv[] = { "w16", "--fonts", 0 };
    return ask(argv);
}

char *win16_short_title(const char *d) {
    for (int again = 1; again; ) {
        again = 0;
        static const char *const PREFIX[] = { "Microsoft ", "Windows " };
        for (int i = 0; i < 2; i++) {
            size_t n = strlen(PREFIX[i]);
            if (!strncmp(d, PREFIX[i], n) && d[n]) { d += n; again = 1; }
        }
    }
    char *t = strdup(d);
    if (!t) return 0;
    for (char *c = t; *c; c++)
        if (c > t && c[-1] == ' ' &&
            (!strncmp(c, "(c)", 3) || !strncmp(c, "(C)", 3) || !strncmp(c, "Copyright", 9) ||
             (c[0] == 'v' && c[1] >= '0' && c[1] <= '9'))) {
            c[-1] = 0;
            break;
        }
    return t;
}
