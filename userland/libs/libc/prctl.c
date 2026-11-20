// prctl() over SYS_PRCTL. See <sys/prctl.h>.
//
// **THROUGH sys_call(), NOT sys_prctl()**: libc.so finds its sys_*
// functions in the PROGRAM's own runtime, and a program built before
// sys_prctl existed (e871e149) has none -- loading this libc beside it
// failed with "ld-toy: undefined symbol: sys_prctl", exit 127 (ScummVM,
// an optional build made the day before). sys_call() is in every
// runtime there has been. checks/check_libc_imports.py keeps the rule.
#include <sys/prctl.h>
#include <stdarg.h>
#include <errno.h>
#include "rt/sys.h"
#include "syscall_abi.h"   // SYS_PRCTL

int prctl(int option, ...) {
    va_list ap;
    va_start(ap, option);
    unsigned long arg = va_arg(ap, unsigned long);   // Linux's arg2; the rest are unused
    va_end(ap);
    int64_t r = sys_call(SYS_PRCTL, (uint64_t)(int64_t)option, arg, 0);
    if (r < 0 && r >= -4094) {   // abi/errno.h's ERRNO_MAX: an error, not a value
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}
