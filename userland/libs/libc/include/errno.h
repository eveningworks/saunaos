#ifndef ULIB_ERRNO_H
#define ULIB_ERRNO_H

// C's <errno.h>: the codes, plus `errno` itself as an lvalue.
//
// The CODES are the kernel's -- abi/errno.h, reached as <kerrno.h>
// because this file has taken the plain name (see that header for the
// naming pattern). One list, so a number means the same thing on both
// sides of the syscall boundary.
//
// `errno` is set by every libsys wrapper whose failure value is -1, and
// READ ONLY AFTER A CALL HAS REPORTED FAILURE: it is not cleared on
// success, which is POSIX's rule, so a stale value from an earlier
// failure is still here after a hundred successful calls.
//
// rt/sys.h's sys_errno() is the same storage under the older name and
// keeps working; this is the C spelling of it, not a second copy.
#include <kerrno.h>

int *__errno_location(void);

#define errno (*__errno_location())

// **THE SAME VALUE AS EAGAIN, which POSIX explicitly permits and every
// Unix does.** They are one condition with two names -- "try again" and
// "this would have blocked" -- and code that tests both with `||` is
// correct either way.
#define EWOULDBLOCK EAGAIN

// RAISED BY THE C LIBRARY, NEVER RETURNED BY THE KERNEL -- which is why
// they are here and not in abi/errno.h, whose list is what a kernel
// handler distinguishes. C itself requires EDOM and EILSEQ; Linux's
// numbers, as the rest are (abi/errno.h says why).
#define EDOM         33  // a maths argument outside the function's domain
#define EILSEQ       84  // bytes that are not a character: iconv(), mbrtowc()
#define EAFNOSUPPORT 97  // inet_pton()/inet_ntop() given a family they do not know

#endif
