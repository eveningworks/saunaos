#ifndef EXFAT_H
#define EXFAT_H

#include "fs_ops.h"

// exFAT, as a VFS backend: what a stick bigger than 32 GiB ships with, and
// what Windows and macOS both read and write. kernel/fs/exfat.c has the
// format's traps; exfat_internal.h the split across its three files.
extern const struct fs_ops exfat_ops;

#endif
