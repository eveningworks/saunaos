// win16.h -- what the Win16 Programs launcher's two windows share: the
// program list (gui/apps/win16.c) and a program's calls (calls.c).
#ifndef WIN16_H
#define WIN16_H

#define W16_BIN "/bin/w16"

// Everything `w16 --check PATH` printed, NUL-terminated, however long:
// free() it. NULL when w16 could not be run. The program is hostile
// input and only w16, in its sandbox, ever reads it; this reads w16.
char *win16_check(const char *path);

// What a tile or a window title calls a program: its description with a
// maker's prefix ("Microsoft Windows ") off the front and a copyright or
// a version (" (c)1990", " v3.0a") off the end. Allocated: free() it.
char *win16_short_title(const char *description);

// The window listing what `path` imports: `win16 --calls PATH`.
int win16_calls_main(const char *path);

#endif
