#ifndef LDSO_API_H
#define LDSO_API_H

#include <stdint.h>

// HOW A PROGRAM REACHES THE LOADER, and the whole of dlopen's plumbing.
//
// /lib/ld-toy.so is a fixed-base ET_EXEC with no `.dynsym` (see its own
// top comment for why it is not ET_DYN), so nothing can resolve a
// symbol OUT of it the ELF way. The reverse direction works: the loader
// already looks symbols UP in the objects it loaded, which is how the
// ABI stamp is read. So libc DEFINES this vector and the loader FILLS
// IT IN before jumping to the entry point.
//
// `dlopen()` and friends in libc are then one indirect call each. A
// statically linked program never runs the loader, so the vector stays
// zeroed there and dlopen() answers "no dynamic loader" rather than
// jumping through a null pointer.
#define LDSO_API_MAGIC 0x4F534C44u // 'DLSO'

// What dl_iterate_phdr() reports per loaded object: <link.h>'s
// `struct dl_phdr_info`, field for field (glibc's layout, which libgcc
// and libunwind read by offset). libc's dlfcn.c asserts the two agree;
// this copy exists because the loader may include no libc header.
struct ldso_phdr_info {
    uint64_t    addr;    // load bias: run-time address minus link-time
    const char *name;    // "" for the executable, as glibc reports it
    const void *phdr;    // its mapped program headers
    uint16_t    phnum;
    uint64_t    adds;    // objects ever loaded / unloaded, so a cache
    uint64_t    subs;    // keyed on them knows when to re-walk
    uint64_t    tls_modid;
    void       *tls_data;
};

struct ldso_api {
    uint32_t magic;      // LDSO_API_MAGIC once the loader has filled it
    uint32_t reserved;
    void *(*dlopen)(const char *path, int flags);
    void *(*dlsym)(void *handle, const char *name);
    int   (*dlclose)(void *handle);
    const char *(*dlerror)(void);
    // Added with constructors (docs/cc-design.md, stage 2); a loader
    // older than that leaves them NULL, and libc checks.
    //
    // run_init: every LIBRARY's DT_INIT then DT_INIT_ARRAY, a library's
    // dependencies before it. NOT the executable's -- crt0 passes its
    // own arrays to libc, which runs them after this returns. Called by
    // libc after TLS exists, which is why the loader cannot run them
    // before jumping to the entry the way glibc's does.
    void (*run_init)(int argc, char **argv, char **envp);
    // run_fini: the libraries' DT_FINI_ARRAY (last entry first) then
    // DT_FINI, in the reverse of the order run_init and dlopen ran them.
    void (*run_fini)(void);
    int  (*iterate_phdr)(int (*cb)(struct ldso_phdr_info *info, uint64_t size, void *data),
                         void *data);
};

#endif // LDSO_API_H
