#ifndef ULIB_LINK_H
#define ULIB_LINK_H

// dl_iterate_phdr() -- every object loaded into this process, with its
// program headers. What an unwinder or a profiler uses to find the code
// it is looking at; glibc's and the BSDs' interface, field for field.
//
// The executable is reported first with an empty name, then each shared
// library in load order. /lib/ld-toy.so itself is NOT reported: its
// headers are not mapped (it is a fixed-base program, not a library),
// and it holds no code a caller could be unwinding through. A library
// is reported only if its program headers are inside one of its loaded
// segments, which every `ld -shared` output is.
#include <stddef.h>
#include <elf.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ElfW(type) Elf64_##type

struct dl_phdr_info {
    ElfW(Addr)        dlpi_addr;   // load bias: run-time minus link-time address
    const char       *dlpi_name;
    const ElfW(Phdr) *dlpi_phdr;
    ElfW(Half)        dlpi_phnum;
    unsigned long long dlpi_adds;  // objects ever loaded; changes on a dlopen
    unsigned long long dlpi_subs;  // objects ever unloaded; always 0 here
    size_t            dlpi_tls_modid;
    void             *dlpi_tls_data;
};

// Calls `cb` once per object, stopping early at the first non-zero
// return, which is then dl_iterate_phdr's. `size` is sizeof(struct
// dl_phdr_info), so a caller built against a shorter one can tell.
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *info, size_t size, void *data),
                    void *data);

#ifdef __cplusplus
}
#endif

#endif
