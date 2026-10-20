// retrolauncher.h -- what the Retro Launcher launcher's two windows share: the
// program list (gui/apps/retrolauncher.c) and a program's calls (calls.c).
#ifndef RETROLAUNCHER_H
#define RETROLAUNCHER_H

#define W16_BIN "/bin/w16"

// Everything `w16 --check PATH` printed, NUL-terminated, however long:
// free() it. NULL when w16 could not be run. The program is hostile
// input and only w16, in its sandbox, ever reads it; this reads w16.
char *retro_check(const char *path);

// Everything `w16 --fonts` printed: Windows 3.11's own fonts on drive C:,
// with a sample of each face. Same contract; fonts are hostile input too.
char *retro_fonts(void);

// The Options' card of what `w16 --fonts` found (fonts.c): a row per face
// -- its name, its file and sizes, a sample drawn in it -- for Options >
// Display beside the Fonts choice. retro_fontcard_load() asks w16 again.
struct retro_fontcard;
struct uui_widget_ops;
extern struct retro_fontcard g_retro_fontcard;
extern const struct uui_widget_ops retro_fontcard_ops;
void retro_fontcard_load(struct retro_fontcard *c);

// What a tile or a window title calls a program: its description with a
// maker's prefix ("Microsoft Windows ") off the front and a copyright or
// a version (" (c)1990", " v3.0a") off the end. Allocated: free() it.
char *retro_short_title(const char *description);

// The window listing what `path` imports: `retrolauncher --calls PATH`.
int retro_calls_main(const char *path);

#endif
