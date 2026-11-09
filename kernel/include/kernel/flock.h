#ifndef FLOCK_H
#define FLOCK_H

// POSIX RECORD LOCKS -- fcntl()'s F_GETLK/F_SETLK/F_SETLKW
// (kernel/fs/flock.c). Advisory: a lock stops another LOCK, never a
// read or a write.
//
// LINUX'S SHAPE, TRAP INCLUDED. A lock belongs to the PROCESS (Linux's
// fl_owner is the files_struct; here it is the descriptor table's key,
// the address space), covers a byte range of the FILE (mount and
// inode, so a second open or a hard link meets the same locks), is not
// inherited across fork, survives exec, and goes when the process
// exits -- and when the process closes ANY descriptor on that file,
// even one it opened separately and never locked through. That last is
// POSIX's, everyone's least favourite part of it, and kept because a
// program written for it (make's --output-sync) expects exactly that.
// OFD locks (Linux's F_OFD_SETLK, owned by the open file description)
// and flock(2) are not built; neither is deadlock detection, so two
// processes each waiting on the other's lock wait until a signal.

#include <stdint.h>
#include "syscall_abi.h"   // struct sys_flock, SYS_LK_*

struct open_file;

// The one entry: `cmd` is SYS_LK_GET/SET/SETW on description `f`, for the
// process whose table is `pml4` (pid `pid`). 0 or a negative errno;
// SYS_LK_GET rewrites *fl with the first lock in the way (type
// SYS_LK_UN when nothing is). SETW on a contended range parks the
// caller on `regs` and sets *parked -- the handler then returns with no
// result, and the wake's SYS_RETRY makes libsys ask again. Where there
// is nowhere to park (a KTEST), SETW answers -EAGAIN as SET does.
int flock_op(uint64_t pml4, int pid, struct open_file *f, int cmd, struct sys_flock *fl,
             uint64_t *regs, int *parked);

// The descriptor layer's three hooks (kernel/proc/syscall_fd.c): a
// descriptor of `owner` on `f` closing (releases its locks on that
// file -- the POSIX rule), a table going away (every lock it held),
// and an exec moving the table to a new key.
void flock_close(uint64_t owner, struct open_file *f);
void flock_release_owner(uint64_t owner);
void flock_rekey(uint64_t old_owner, uint64_t new_owner);

// How many locks are held, system-wide -- for a KTEST to see none leak.
int flock_count(void);

#endif
