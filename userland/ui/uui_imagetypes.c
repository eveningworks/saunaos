// The picture rows of a file chooser -- see ui/uui_imagetypes.h.
#include "ui/uui_imagetypes.h"
#include "lib/uimg.h"
#include "kpath.h"
#include <string.h>

// `ctx` is the codec name the row lists, or NULL for every picture.
static int keep(void *ctx, const char *dir, const struct sys_dirent *e) {
    char path[UUI_FILEDIALOG_PATH_MAX];
    if (!k_path_join(dir, e->name, path, sizeof path)) return 0;
    const char *f = uimg_format_guess(path);
    return f && (!ctx || !strcmp(f, (const char *)ctx));
}

const struct uui_filedialog_filter uui_imagetypes_open[] = {
    { "All pictures (*.png; *.jpg; *.jpeg; *.gif; *.bmp; *.qoi)", keep, 0, 0 },
    { "PNG (*.png)",           keep, "png",  ".png" },
    { "JPEG (*.jpg; *.jpeg)",  keep, "jpeg", ".jpg" },
    { "GIF (*.gif)",           keep, "gif",  ".gif" },
    { "BMP (*.bmp)",           keep, "bmp",  ".bmp" },
    { "QOI (*.qoi)",           keep, "qoi",  ".qoi" },
    { "All files (*.*)",       0,    0,      0 },
};
const int uui_imagetypes_open_count = (int)(sizeof uui_imagetypes_open / sizeof uui_imagetypes_open[0]);

const struct uui_filedialog_filter uui_imagetypes_save[] = {
    { "PNG (*.png)",           keep, "png",  ".png" },
    { "JPEG (*.jpg; *.jpeg)",  keep, "jpeg", ".jpg" },
    { "QOI (*.qoi)",           keep, "qoi",  ".qoi" },
};
const int uui_imagetypes_save_count = (int)(sizeof uui_imagetypes_save / sizeof uui_imagetypes_save[0]);

int uui_imagetypes_save_index(const char *format) {
    for (int i = 0; format && i < uui_imagetypes_save_count; i++)
        if (!strcmp((const char *)uui_imagetypes_save[i].ctx, format)) return i;
    return 0;
}
