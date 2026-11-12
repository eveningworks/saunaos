// A ZERO-BYTE READ OR WRITE MOVES NOTHING, AND AN ELF WHOSE SEGMENTS SHARE A
// PAGE IS REFUSED.
//
//   pipe     read(fd, buf, 0) on a pipe holding "AB" returns 0, and the next
//            read still gets "AB" -- the bounce buffer turned a count of 0
//            into 1 and popped a byte (kernel/proc/syscall_fd.c)
//   file     write(fd, "X", 0) leaves the file empty and the position at 0
//   overlap  an ELF with two PT_LOAD segments over the same pages does not
//            spawn: the second replaced the first's frames and leaked them
//            for the rest of the boot (kernel/proc/elf.c). Its control is
//            the same file with one segment, which does spawn.
//
// It must be SPAWNED, not `run` -- it starts a child.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"

#define FILE_PATH "/tmp/zerolen_test.dat"
#define ELF_PATH  "/tmp/zerolen_test.elf"
#define BASE      0x8000000000ull   // the image window (kernel/include/kernel/uaddr.h)

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// A static ELF64 of `segs` PT_LOADs, each mapping 64 KiB at BASE: the file
// itself, which ends in `jmp $` -- the entry -- so a started one spins
// until it is killed rather than crashing.
static int write_elf(int segs) {
    uint8_t f[64 + 2 * 56 + 2];
    memset(f, 0, sizeof f);
    uint32_t code = 64 + 56 * (uint32_t)segs, total = code + 2;
    f[code] = 0xEB; f[code + 1] = 0xFE;
    memcpy(f, "\x7f" "ELF", 4);
    f[4] = 2; f[5] = 1; f[6] = 1;              // 64-bit, little endian, version 1
    put16(f + 16, 2);                          // ET_EXEC
    put16(f + 18, 62);                         // EM_X86_64
    put32(f + 20, 1);
    put64(f + 24, BASE + code);                // e_entry: the jmp
    put64(f + 32, 64);                         // e_phoff
    put16(f + 52, 64);                         // e_ehsize
    put16(f + 54, 56);                         // e_phentsize
    put16(f + 56, (uint16_t)segs);             // e_phnum
    for (int i = 0; i < segs; i++) {
        uint8_t *ph = f + 64 + 56 * i;
        put32(ph, 1);                          // PT_LOAD
        put32(ph + 4, 5);                      // R X
        put64(ph + 16, BASE);                  // p_vaddr
        put64(ph + 24, BASE);                  // p_paddr
        put64(ph + 32, total);                 // p_filesz: from offset 0
        put64(ph + 40, 0x10000);               // p_memsz
        put64(ph + 48, 0x1000);                // p_align
    }
    int fd = open(ELF_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    int ok = fd >= 0 && write(fd, f, total) == (long)total;
    if (fd >= 0) close(fd);
    return ok;
}

// Spawns the generated file: 1 when the kernel started it.
static int spawns(int segs) {
    if (!write_elf(segs)) return -1;
    int pid = sys_spawn(ELF_PATH, 0, -1);
    if (pid > 0) { sys_kill(pid, SIGKILL); int code; sys_waitpid(pid, &code); }
    unlink(ELF_PATH);
    return pid > 0;
}

int main(void) {
    utest_begin("zerolen_test", "zero-byte transfers move nothing; overlapping segments refused",
                UTEST_VERDICT_FILE);

    int p[2];
    char buf[4] = {0};
    int piped = sys_pipe(p) == 0 && write(p[1], "AB", 2) == 2;
    long z = piped ? read(p[0], buf, 0) : -1;
    long n = piped ? read(p[0], buf, 2) : -1;
    utest_checkf(piped && z == 0 && n == 2 && !memcmp(buf, "AB", 2),
                 "pipe: a zero-byte read returns 0 and takes nothing (then read %ld: '%.2s')", n, buf);

    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC);
    long w = fd >= 0 ? write(fd, "X", 0) : -1;
    off_t pos = fd >= 0 ? lseek(fd, 0, SEEK_CUR) : -1;
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (fd >= 0) close(fd);
    unlink(FILE_PATH);
    utest_checkf(w == 0 && pos == 0 && size == 0,
                 "file: a zero-byte write writes nothing (returned %ld, position %ld, size %ld)",
                 w, (long)pos, (long)size);

    int one = spawns(1), two = spawns(2);
    utest_checkf(one == 1, "overlap: the control, one segment, spawns (%d)", one);
    utest_checkf(two == 0, "overlap: two segments over the same pages are refused (%d)", two);
    return utest_end();
}
