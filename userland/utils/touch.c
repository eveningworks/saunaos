// touch -- set a file's modification time to now, creating it empty if
// it does not exist.
//
// Creating is SYS_OPEN with O_WRITE|O_CREAT, so the creation rules stay
// in one place. An existing file gets SYS_UTIME (utime() with NULL) --
// which only tfs3 can do: on FAT32 or exFAT it says so and fails, rather
// than quietly doing nothing under a familiar name.
#include "base/cmd.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>
#include <utime.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        cmd_usage("touch <file> [file...]");
        return 1;
    }
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        if (access(argv[i], F_OK) == 0) {
            if (utime(argv[i], NULL) != 0) {
                cmd_fail("touch", argv[i]);
                failed = 1;
            }
            continue;
        }
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            cmd_fail("touch", argv[i]);
            failed = 1;
        } else {
            close(fd);
        }
    }
    return failed;
}
