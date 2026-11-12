// A /tmp FILE HAS A LARGEST SIZE, AND A WRITE OR TRUNCATE PAST IT IS REFUSED.
//
// ramfs (/tmp) indexes its chunks with 32 bits. A write at a huge offset
// truncated that index: at 8 TiB the chunk table's doubling wrapped to 0
// and spun forever inside the syscall, holding /tmp's lock (and, with
// interrupts off in a syscall, the whole machine); at chunk 0xFFFFFFFF
// it indexed a NULL table; at 16 TiB it landed in chunk 0 of the file.
// RAMFS_MAX_BYTES (kernel/fs/ramfs.c) bounds them now -- Linux's
// s_maxbytes.
//
// Each probe runs in a child: a hang is the harness's timeout, a panic
// the end of the run, and either is red.
//
// It must be SPAWNED, not `run` -- it has children.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/bigoff_test"
#define FILE_PATH "/tmp/bigoff_test.dat"

// Writes one byte at `off`. 0 when the write was refused and the file's
// first bytes are still "keep" -- nothing aliased into chunk 0.
static int write_far(uint64_t off) {
    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0 || write(fd, "keep", 4) != 4) return 2;
    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) return 3;
    long n = write(fd, "Z", 1);
    char back[4] = {0};
    lseek(fd, 0, SEEK_SET);
    long r = read(fd, back, 4);
    close(fd);
    unlink(FILE_PATH);
    if (n == 1) return 1;                              // accepted: wrong
    return r == 4 && !memcmp(back, "keep", 4) ? 0 : 4;
}

static int truncate_far(void) {
    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) return 2;
    int rc = ftruncate(fd, (off_t)(1ULL << 50));
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    unlink(FILE_PATH);
    return rc != 0 && size == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--probe")) {
        if (!strcmp(argv[2], "spin"))     return write_far(1ULL << 43);
        if (!strcmp(argv[2], "wild"))     return write_far(0xFFFFFFFFULL * 4096);
        if (!strcmp(argv[2], "alias"))    return write_far(1ULL << 44);
        if (!strcmp(argv[2], "truncate")) return truncate_far();
        return 9;
    }
    utest_begin("bigoff_test", "a /tmp file past its largest size is refused", UTEST_VERDICT_FILE);
    static const struct { const char *name, *what; } probes[] = {
        { "spin",     "a write at 8 TiB is refused, not a kernel that spins" },
        { "wild",     "a write at chunk 0xFFFFFFFF is refused, not a wild index" },
        { "alias",    "a write at 16 TiB is refused and lands nowhere in the file" },
        { "truncate", "growing a file to 2^50 bytes is refused" },
    };
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
        char args[32];
        snprintf(args, sizeof args, "--probe %s", probes[i].name);
        int pid = sys_spawn(SELF, args, -1);
        int code = -1;
        if (pid <= 0 || sys_waitpid(pid, &code) != pid) code = -1;
        utest_checkf(code == 0, "%s (child exit %d)", probes[i].what, code);
    }
    return utest_end();
}
