// AN UNVEIL ENTRY CANNOT BE STEPPED AROUND BY SPELLING A PATH IN ANOTHER CASE.
//
// The unveil list is matched as a string before any filesystem sees the
// path, and /boot is FAT32, which resolves names case-insensitively. With
// "/boot" readable and "/boot/grub" hidden, "/boot/GRUB/grub.cfg" missed
// the hiding entry, fell to the wider one, and FAT opened the file. The
// match is made both ways now and both must allow (kernel/proc/sandbox.c,
// check()). The other direction is checked too: folding must not WIDEN an
// allow, on /tmp (ramfs), where "/tmp/u/a" and "/tmp/u/A" are two files.
//
// The sandbox runs in a child: unveil cannot be undone.
//
// It must be SPAWNED, not `run` -- it has a child.
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"
#include "sandbox_abi.h"

#define SELF "/tests/unveilcase_test"
#define CFG  "/boot/grub/grub.cfg"

static int opens(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd >= 0) close(fd);
    return fd >= 0;
}

// Exit code: bit 0 the exact path opened (wrong), bit 1 the other case
// opened (the bypass), bit 2 the control under /boot did not open, bit 3
// folding widened the /tmp allow, bit 4 the setup failed.
static int child(void) {
    int code = 0;
    if (sys_unveil("/boot", UNVEIL_R) || sys_unveil("/boot/grub", 0) ||
        sys_unveil("/tmp/unveilcase/a", UNVEIL_R) || sys_unveil(0, 0)) return 16;
    if (opens(CFG)) code |= 1;
    if (opens("/boot/GRUB/grub.cfg") || opens("/BOOT/grub/GRUB.CFG")) code |= 2;
    struct sys_stat st;
    if (sys_stat("/boot", &st) != 0) code |= 4;  // still readable: the entry works
    if (!opens("/tmp/unveilcase/a")) code |= 4;
    if (opens("/tmp/unveilcase/A")) code |= 8;
    return code;
}

static void touch(const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT);
    if (fd >= 0) { write(fd, "x", 1); close(fd); }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) return child();
    utest_begin("unveilcase_test", "an unveil entry holds whatever case the path is in",
                UTEST_VERDICT_FILE);
    utest_check(opens(CFG), "the fixture: " CFG " is there");
    mkdir("/tmp/unveilcase", 0755);
    touch("/tmp/unveilcase/a");
    touch("/tmp/unveilcase/A");
    int pid = sys_spawn(SELF, "--child", -1);
    int code = -1;
    if (pid <= 0 || sys_waitpid(pid, &code) != pid) code = -1;
    unlink("/tmp/unveilcase/a");
    unlink("/tmp/unveilcase/A");
    rmdir("/tmp/unveilcase");
    utest_checkf(code >= 0 && code < 16, "the sandboxed child ran (exit %d)", code);
    utest_checkf(code >= 0 && !(code & 1), "the hidden file is hidden");
    utest_checkf(code >= 0 && !(code & 2), "...and spelling it in another case does not open it");
    utest_checkf(code >= 0 && !(code & 4), "what the list allows still opens");
    utest_checkf(code >= 0 && !(code & 8), "folding does not widen an allow to another file");
    return utest_end();
}
