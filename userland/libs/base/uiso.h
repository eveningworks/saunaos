#ifndef ULIB_UISO_H
#define ULIB_UISO_H
// ISO 9660 disc images as a program sees them before mounting one: is this
// file a disc image, and what is its volume label (Joliet's when the disc
// has Joliet names, as the kernel reads them). The reading itself is the
// kernel's (kernel/fs/iso9660.c), mounted through base/udrive.h.
#include <stddef.h>

// 1 when `path` is an ISO 9660 image (a primary volume descriptor at
// sector 16), 0 when it is not, -1 when it cannot be read.
int uiso_is_image(const char *path);

// Its volume label into `out` (UTF-8, trailing blanks dropped; "" when the
// disc has none). 0, or -1 when it is no disc image or cannot be read.
int uiso_label(const char *path, char *out, size_t cap);

#endif
