// /dev/null, /dev/zero, /dev/random and /dev/urandom (kernel/core/devnodes.c),
// through the C library a ported program uses -- open(), read(), fopen().
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "base/utest.h"

int main(void) {
    utest_begin("devnode_test", "/dev/null, /dev/zero, /dev/random, /dev/urandom", 0);
    unsigned char buf[4096];

    int fd = open("/dev/null", O_RDWR);
    utest_check(fd >= 0, "/dev/null opens read-write");
    utest_check(write(fd, "dropped", 7) == 7, "a write to /dev/null takes every byte");
    utest_check(read(fd, buf, sizeof buf) == 0, "a read of /dev/null is end of file");
    close(fd);

    FILE *f = fopen("/dev/null", "w");
    utest_check(f && fprintf(f, "%d", 42) == 2 && fclose(f) == 0,
          "fopen(\"/dev/null\", \"w\") and fprintf through it");

    fd = open("/dev/zero", O_RDONLY);
    memset(buf, 0xAA, sizeof buf);
    int zeros = fd >= 0 && read(fd, buf, sizeof buf) == (ssize_t)sizeof buf;
    for (size_t i = 0; zeros && i < sizeof buf; i++) if (buf[i]) zeros = 0;
    utest_check(zeros, "/dev/zero reads as zeros, a whole 4 KiB at once");
    close(fd);

    // Two reads of 64 bytes the same would be a 2^-512 chance; all zeros
    // (an unfilled buffer) is what a broken read gives.
    static const char *const names[] = { "/dev/urandom", "/dev/random" };
    for (int n = 0; n < 2; n++) {
        unsigned char a[64], b[64];
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        fd = open(names[n], O_RDONLY);
        int got = fd >= 0 && read(fd, a, sizeof a) == 64 && read(fd, b, sizeof b) == 64;
        int nonzero = 0;
        for (int i = 0; i < 64; i++) if (a[i]) nonzero = 1;
        utest_checkf(got && nonzero && memcmp(a, b, sizeof a) != 0,
                     "%s gives fresh random bytes on every read", names[n]);
        if (fd >= 0) close(fd);
    }

    fd = open("/dev/nonesuch", O_RDONLY);
    utest_check(fd < 0, "a name under /dev that is not a device is not found");
    return utest_end();
}
