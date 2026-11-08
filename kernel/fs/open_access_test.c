// SYS_OPEN's flags as an access decision (fs_open_access()): which mode
// the description gets and which unveil bits its path must carry. The
// read-write mode end to end -- write, seek, read back on one fd -- is
// userland/tests/libc3_test.c's, through open(O_RDWR) and fopen("w+").
#include "ktest.h"
#include "syscalls.h"
#include "syscall_abi.h"
#include "sandbox_abi.h"

KTEST("fd", "open flags decide a reader, a writer or both, and the unveil bits for it") {
    static const struct {
        uint32_t flags;
        enum fd_mode mode;
        unsigned unveil;
    } cases[] = {
        { 0,                                        FD_MODE_READ,  UNVEIL_R },
        { SYS_O_WRITE,                              FD_MODE_WRITE, UNVEIL_W },
        { SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC,  FD_MODE_WRITE, UNVEIL_W | UNVEIL_C },
        { SYS_O_APPEND,                             FD_MODE_WRITE, UNVEIL_W },
        { SYS_O_RDWR,                               FD_MODE_RDWR,  UNVEIL_R | UNVEIL_W },
        // A mode, not a modifier: SYS_O_WRITE beside it changes nothing.
        { SYS_O_RDWR | SYS_O_WRITE,                 FD_MODE_RDWR,  UNVEIL_R | UNVEIL_W },
        { SYS_O_RDWR | SYS_O_CREAT | SYS_O_EXCL,    FD_MODE_RDWR,  UNVEIL_R | UNVEIL_W | UNVEIL_C },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unsigned unveil = 0;
        KTEST_ASSERT_EQ(fs_open_access(cases[i].flags, &unveil), cases[i].mode);
        KTEST_ASSERT_EQ(unveil, cases[i].unveil);
    }
}

KTEST("fd", "a read-write description is both a reader and a writer") {
    KTEST_ASSERT(fd_mode_reads(FD_MODE_READ) && !fd_mode_writes(FD_MODE_READ));
    KTEST_ASSERT(!fd_mode_reads(FD_MODE_WRITE) && fd_mode_writes(FD_MODE_WRITE));
    KTEST_ASSERT(fd_mode_reads(FD_MODE_RDWR) && fd_mode_writes(FD_MODE_RDWR));
}
