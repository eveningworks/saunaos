// check.c -- asking w16 (`--check`, `--fonts`, `--sounds`).
#include "retrolauncher/retrolauncher.h"

#include <stdlib.h>
#include <string.h>
#include "lib/ucapture.h"

static char *ask(char **argv) { return ucapture(W16_BIN, argv); }

char *retro_check(const char *path) {
    char *argv[] = { "w16", "--check", (char *)path, 0 };
    return ask(argv);
}

char *retro_fonts(void) {
    char *argv[] = { "w16", "--fonts", 0 };
    return ask(argv);
}

char *retro_sounds(void) {
    char *argv[] = { "w16", "--sounds", 0 };
    return ask(argv);
}

