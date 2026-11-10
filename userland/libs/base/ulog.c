// Toolkit diagnostics -- see base/ulog.h.
#include "base/ulog.h"
#include "rt/sys.h"     // sys_eprint
#include <stdio.h>      // vsnprintf
#include <stdlib.h>
#include <stdarg.h>

void ulog(const char *s) {
    sys_eprint(s);
}

// SIZED TO THE LINE: a short one is formatted on the stack, a longer one
// (a path past a hundred bytes) into a buffer of its own length -- a log
// line cut short is a test reading a different path than was opened.
void ulogf(const char *fmt, ...) {
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof line) { sys_eprint(line); return; }
    char *big = malloc((size_t)n + 1);
    if (!big) { sys_eprint(line); return; }   // no memory: the cut line beats none
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sys_eprint(big);
    free(big);
}
