#ifndef ULIB_SYS_AUXV_H
#define ULIB_SYS_AUXV_H

// The auxiliary vector the kernel left after envp, by type -- Linux's
// getauxval(): the value, or 0 with errno ENOENT for a type not passed.
// <elf.h> names the types (AT_RANDOM, AT_PHDR, ...).

#include <elf.h>
#ifdef __cplusplus
extern "C" {
#endif

unsigned long getauxval(unsigned long type);

#ifdef __cplusplus
}
#endif

#endif
