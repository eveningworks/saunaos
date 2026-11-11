#ifndef KERNEL_DEVNODES_H
#define KERNEL_DEVNODES_H

// /dev/null, /dev/zero, /dev/random, /dev/urandom (kernel/core/devnodes.c).
// There are no device nodes on disk: sys_open() asks this first.

struct fd_ops;

// The kind of description an absolute, resolved `path` opens, or NULL
// when it names no device.
const struct fd_ops *devnode_ops(const char *path);

#endif
