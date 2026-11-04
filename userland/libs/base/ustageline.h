#ifndef ULIB_USTAGELINE_H
#define ULIB_USTAGELINE_H

// A long job's STAGE LINE on a terminal, redrawn in place over the last
// one -- "fsck: stage 2 of 3, compare the allocation maps: 18 of 28
// groups (64%)", e2fsck -C's shape -- from the kernel's progress record
// for a check or a format (abi/mount_abi.h's struct fs_check_progress).

#include "mount_abi.h"

// Draws `p` to stderr as `prog`'s line, over the previous one; `*width`
// is that one's length, kept between calls (start it at 0). A record not
// running draws nothing.
void ustageline_draw(const char *prog, const struct fs_check_progress *p, int *width);

// Blanks the line and returns the cursor to its start; `*width` becomes 0.
void ustageline_clear(int *width);

#endif
