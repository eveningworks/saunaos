#ifndef ULIB_SYS_PRCTL_H
#define ULIB_SYS_PRCTL_H

// Linux's prctl(), for the options the kernel takes (SYS_PRCTL):
// PR_GET_DUMPABLE answers 0 or 1; PR_SET_DUMPABLE 0 asks that a fault
// killing this process write no crash report, 1 restores it -- EPERM for
// 0 once the process is sandboxed. Anything else is -1 with errno EINVAL.

#define PR_GET_DUMPABLE 3
#define PR_SET_DUMPABLE 4

#ifdef __cplusplus
extern "C" {
#endif

int prctl(int option, ...);

#ifdef __cplusplus
}
#endif

#endif
