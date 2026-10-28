#ifndef ULIB_SYS_SELECT_H
#define ULIB_SYS_SELECT_H

// fd_set and its macros -- and NO select(). The kernel cannot wait on
// several descriptors at once (unistd.h says so), and a select() that
// failed every call would turn a link error into a program that runs
// and misbehaves. The type is here because ported code hands fd_sets
// through interfaces that, on this system, never reach select():
// NetSurf's fetcher table has an `fdset` slot our fetchers leave empty.
#include <sys/time.h>

//
// FD_SETSIZE IS AN ABI BOUND, as on Linux, and a descriptor CAN be past
// it: the kernel's table grows to a RAM-sized ceiling. So the macros
// ignore such an fd -- FD_SET/FD_CLR do nothing, FD_ISSET is false --
// rather than write past the set (glibc's _FORTIFY_SOURCE aborts there).
#define FD_SETSIZE 1024

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define __FD_OK(fd)   ((unsigned)(fd) < FD_SETSIZE)
#define __FD_WORD(fd) ((unsigned)(fd) / (8 * sizeof(unsigned long)))
#define __FD_MASK(fd) (1UL << ((unsigned)(fd) % (8 * sizeof(unsigned long))))
#define FD_ZERO(s)     do { for (unsigned __i = 0; __i < sizeof((s)->fds_bits) / sizeof((s)->fds_bits[0]); __i++) \
                                (s)->fds_bits[__i] = 0; } while (0)
#define FD_SET(fd, s)   ((void)(__FD_OK(fd) && ((s)->fds_bits[__FD_WORD(fd)] |= __FD_MASK(fd))))
#define FD_CLR(fd, s)   ((void)(__FD_OK(fd) && ((s)->fds_bits[__FD_WORD(fd)] &= ~__FD_MASK(fd), 1)))
#define FD_ISSET(fd, s) (__FD_OK(fd) && ((s)->fds_bits[__FD_WORD(fd)] & __FD_MASK(fd)) != 0)

#endif
