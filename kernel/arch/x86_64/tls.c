// FS.base, the thread pointer. See kernel/include/kernel/tls.h.
#include "tls.h"
#include "msr.h"

#define MSR_IA32_FS_BASE 0xC0000100u

void arch_set_fs_base(uint64_t base) {
    wrmsr(MSR_IA32_FS_BASE, base);
}

uint64_t arch_get_fs_base(void) {
    return rdmsr(MSR_IA32_FS_BASE);
}
