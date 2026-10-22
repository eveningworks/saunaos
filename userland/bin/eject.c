// eject -- make a USB stick safe to pull out: unmount everything on it,
// then tell the disk (SYS_EJECT). util-linux's eject has the same shape:
// the unmounting is part of the command, not the caller's chore. The work
// is lib/udrive.h's, shared with the auto-mount service.
#include "rt/sys.h"
#include "lib/uargs.h"
#include "lib/udrive.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const struct uargs_opt OPTS[] = { { 0 } };

static const struct uargs_prog PROG = {
    .name = "eject",
    .usage = "DISK",
    .summary = "Unmount everything on DISK (as lsblk names it: usb0) and eject it,\n"
               "so it can be pulled out.",
    .opts = OPTS,
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc != 1) return uargs_error(&PROG, a.argc ? "one disk at a time" : "which disk?");
    const char *disk = a.argv[0];
    if (udrive_eject(disk) < 0) {
        int e = errno;
        printf("eject: %s: %s\n", disk,
               e == ENOENT ? "no such disk" :
               e == ENOTSUP ? "not a removable disk" :
               e == EPERM ? "the system is running from it" :
               e == EBUSY ? "still in use -- close what has a file open on it" : strerror(e));
        return 1;
    }
    printf("eject: %s can be pulled out\n", disk);
    return 0;
}
