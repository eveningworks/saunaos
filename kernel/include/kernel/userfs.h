#ifndef USERFS_H
#define USERFS_H

// The process-served filesystem (kernel/fs/userfs.c). The backend is
// mounted only by SYS_USERFS -- nothing probes for it, nothing names it
// by type -- so the one thing the rest of the kernel needs is to
// recognise one of its mounts and describe it.
#include <stdint.h>

struct fs_ops;
extern const struct fs_ops userfs_ops;

struct userfs_describe {
    char type[16];             // "smb"
    char source[128];          // "//nas/media"
    char note[96];
    uint32_t state;            // USERFS_ST_*
};

// `st` is a userfs mount's state (struct mount's). Always 1.
int userfs_describe(const void *st, struct userfs_describe *out);

#endif
