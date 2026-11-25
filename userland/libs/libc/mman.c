// mmap()/munmap() -- thin over rt/sys.c's stubs, which already speak
// POSIX's argument shape and MAP_FAILED. See <sys/mman.h> for what
// this mmap deliberately does not do.
#include <sys/mman.h>
#include "rt/sys.h"

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, long offset) {
    return sys_mmap(addr, length, prot, flags, fd, (uint64_t)offset);
}

// A length need not be a whole number of pages (POSIX; Linux rounds it up
// in the kernel). SYS_MUNMAP and SYS_MPROTECT refuse a partial page, so
// it is rounded up here, where a port's call arrives -- yescrypt frees
// its 16 MiB plus a few bytes this way. The address must still be
// page-aligned.
static size_t whole_pages(size_t n) {
    return n > SIZE_MAX - 4095 ? n : (n + 4095) & ~(size_t)4095;
}

int munmap(void *addr, size_t length) {
    return sys_munmap(addr, whole_pages(length));
}

int mprotect(void *addr, size_t length, int prot) {
    return sys_mprotect(addr, whole_pages(length), prot);
}
