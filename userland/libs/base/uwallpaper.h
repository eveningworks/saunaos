#ifndef ULIB_UWALLPAPER_H
#define ULIB_UWALLPAPER_H

// A WALLPAPER'S SIZE FOR A SCREEN: a picture may come drawn at several
// sizes, `<dir>/<stem>/<W>x<H>.<ext>` beside its `<dir>/<stem>.<ext>` --
// KDE Plasma's wallpaper package (images/ beside a screenshot), flat.
// The single file is what a gallery shows and the answer when there is
// no folder; the folder is what the desktop decodes.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uwallpaper_size {
    int w, h;
};

// THE CHOICE, apart from the disk: the closest aspect ratio first (within
// about 1% counts as the same, so 1366x768 is 16:9), then among those the
// smallest that covers the screen, else the largest -- a crop or a
// downscale, never an upscale while a bigger one exists. The index into
// `sizes`, or -1 when n is 0. Ties go to the earlier entry.
int uwallpaper_pick(const struct uwallpaper_size *sizes, int n, int screen_w, int screen_h);

// The path of `stem`'s size for the screen, from `<dir>/<stem>/`: names
// that are not `<W>x<H>.<jpg|png|qoi|gif>` are ignored. 1 with the path
// in `out`; 0 when there is no such folder, nothing in it parses, or the
// path does not fit `cap` -- then the caller loads `<dir>/<stem>.<ext>`.
int uwallpaper_path(const char *dir, const char *stem, int screen_w, int screen_h,
                    char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
