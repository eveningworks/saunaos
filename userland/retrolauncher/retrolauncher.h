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
char *retro_sounds(void);   // `w16 --sounds`: each event, what it plays, whether found

// The Options' card of what `w16 --fonts` found (fonts.c): a row per face
// -- its name, its file and sizes, a sample drawn in it -- for Options >
// Display beside the Fonts choice. retro_fontcard_load() asks w16 again.
struct retro_fontcard;
struct uui_widget_ops;
extern struct retro_fontcard g_retro_fontcard;
extern const struct uui_widget_ops retro_fontcard_ops;
void retro_fontcard_load(struct retro_fontcard *c);

// Options > Sound's table (sounds.c): a row per event `w16 --sounds`
// found, what it plays under `mode` (W16_SOUNDS_*), and a Play cell.
// retro_sounds_load() asks w16 again; retro_sounds_mode() follows the
// choice before it is saved.
struct uui_table;
extern struct uui_table g_retro_sounds;
void retro_sounds_init(void);
void retro_sounds_load(int mode);
void retro_sounds_mode(int mode);
void retro_sounds_play(int row);

// The window listing what `path` imports: `retrolauncher --calls PATH`.
int retro_calls_main(const char *path);

#endif
