// `int 0x80` is not a syscall entry any more: SYSCALL is the only way
// in (kernel/arch/x86_64/idt.c). A ring-3 `int $0x80` must FAULT, and
// the crash report must say why, since a program built before SYSCALL
// is the one thing that still does it. Run by tools/faulttest_run.py,
// which asserts on the kernel's report.
#include <stdint.h>
#include "rt/sys.h"
#include "syscall_abi.h"

int main(void) {
    static const char msg[] = "int80_test: calling getpid through int $0x80\n";
    sys_write(1, msg, sizeof msg - 1);
    uint64_t r = SYS_GETPID;
    __asm__ volatile ("int $0x80" : "+a"(r) :: "memory");
    // Reached only if the old gate still answers.
    static const char bad[] = "int80_test: int $0x80 STILL REACHES THE KERNEL\n";
    sys_write(1, bad, sizeof bad - 1);
    return 1;
}
