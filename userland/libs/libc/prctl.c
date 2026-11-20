// prctl() over SYS_PRCTL. See <sys/prctl.h>.
#include <sys/prctl.h>
#include <stdarg.h>
#include "rt/sys.h"

int prctl(int option, ...) {
    va_list ap;
    va_start(ap, option);
    unsigned long arg = va_arg(ap, unsigned long);   // Linux's arg2; the rest are unused
    va_end(ap);
    return sys_prctl(option, arg);
}
