// SYS_GETRANDOM from ring 3 -- a self-checking diagnostic, run by
// tools/usertest_run.py as well as by hand (`run random_test`).
//
// Like the kernel-side KTESTs, this does not try to measure randomness:
// a statistical test here would be a number that looks like evidence
// and is not (see kernel/lib/krandom_test.c's top comment). What it
// checks is the SYSCALL's contract, and specifically the parts a
// plausible-looking implementation gets wrong:
//
//   * the buffer is filled and NOTHING past the requested length is
//     touched -- the kernel writes into a user pointer here, so an
//     overrun is a ring-0 write past a ring-3 buffer;
//   * two calls differ, which is what a stuck source looks like;
//   * a bad pointer is REFUSED rather than faulting the kernel, which
//     is the whole reason the dispatcher validates the range;
//   * an oversized count is refused, and a zero count is a legal no-op
//     rather than an error.
#include "rt/sys.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lib/utest.h"

int main(void) {
    utest_begin("random_test", "SYS_GETRANDOM", 0);

    // --- fills exactly, and not one byte more ---------------------
    unsigned char buf[64];
    memset(buf, 0xAA, sizeof buf);
    int n = sys_getrandom(buf + 8, 32);
    utest_check(n == 32, "returns the requested count");

    int guard_ok = 1;
    for (int i = 0; i < 8; i++) if (buf[i] != 0xAA) guard_ok = 0;
    for (int i = 40; i < 64; i++) if (buf[i] != 0xAA) guard_ok = 0;
    utest_check(guard_ok, "wrote nothing outside the requested range");

    int any_set = 0;
    for (int i = 8; i < 40; i++) if (buf[i] != 0xAA) any_set = 1;
    utest_check(any_set, "actually wrote into the buffer");

    // --- two calls differ ------------------------------------------
    unsigned char a[32], b[32];
    sys_getrandom(a, sizeof a);
    sys_getrandom(b, sizeof b);
    utest_check(memcmp(a, b, sizeof a) != 0, "two calls return different bytes");

    // A buffer that came back all-zero is the RDRAND-failure value and
    // also what a kernel that validated the pointer but forgot to fill
    // would leave behind -- worth its own check, since "two calls
    // differ" would not catch a source stuck at zero on both.
    int all_zero = 1;
    for (unsigned i = 0; i < sizeof a; i++) if (a[i]) all_zero = 0;
    utest_check(!all_zero, "output is not all zero");

    // --- refusals ---------------------------------------------------
    utest_check(sys_getrandom((void *)0x10, 16) == -1, "refuses an unmapped pointer");
    utest_check(sys_getrandom(buf, 100000) == -1, "refuses a count over the maximum");
    utest_check(sys_getrandom(buf, 0) == 0, "a zero-length request is a no-op, not an error");

    // --- getentropy(), the libc face -------------------------------
    // All or nothing at GETENTROPY_MAX, EINVAL one past it with the
    // buffer left alone -- POSIX 2024's contract, which OpenBSD's EIO
    // predates.
    unsigned char e[GETENTROPY_MAX + 8];
    memset(e, 0xAA, sizeof e);
    utest_check(getentropy(e, GETENTROPY_MAX) == 0, "getentropy fills GETENTROPY_MAX bytes");
    int e_tail = 1;
    for (unsigned i = GETENTROPY_MAX; i < sizeof e; i++) if (e[i] != 0xAA) e_tail = 0;
    utest_check(e_tail, "getentropy writes nothing past the length");
    memset(e, 0xAA, sizeof e);
    errno = 0;
    utest_check(getentropy(e, GETENTROPY_MAX + 1) == -1 && errno == EINVAL,
                "getentropy refuses GETENTROPY_MAX + 1 with EINVAL");
    utest_check(e[0] == 0xAA, "a refused getentropy leaves the buffer alone");

    return utest_end();
}
