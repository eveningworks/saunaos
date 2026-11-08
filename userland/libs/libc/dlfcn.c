// dlopen() and friends -- four indirect calls into the loader.
//
// THE VECTOR IS DEFINED HERE AND FILLED IN BY /lib/ld-toy.so, which is
// the only direction that works: the loader has no `.dynsym` to resolve
// symbols out of, but it already looks symbols UP in what it loaded
// (abi/ldso_api.h). A statically linked program never runs the loader,
// so `magic` stays zero and every call below answers with a reason
// instead of jumping through a null pointer.
#include <dlfcn.h>
#include <link.h>
#include <stddef.h>
#include "ldso_api.h"

struct ldso_api __ldso_api;

static const char *g_no_loader =
    "no dynamic loader -- this program is statically linked";

static int have(void) {
    return __ldso_api.magic == LDSO_API_MAGIC && __ldso_api.dlopen != NULL;
}

void *dlopen(const char *path, int flags) {
    if (!have()) return NULL;
    return __ldso_api.dlopen(path, flags);
}

void *dlsym(void *handle, const char *name) {
    if (!have()) return NULL;
    return __ldso_api.dlsym(handle, name);
}

int dlclose(void *handle) {
    if (!have()) return -1;
    return __ldso_api.dlclose(handle);
}

const char *dlerror(void) {
    if (!have()) return g_no_loader;
    return __ldso_api.dlerror();
}

// --- dl_iterate_phdr -------------------------------------------------
//
// A dynamic program asks the loader, which has the object table. A
// static one is a single object whose headers are mapped at the base
// (link.ld's FILEHDR PHDRS); `__ehdr_start` is ld's name for them. Weak,
// because an image whose script maps no header has none -- and then
// this reports nothing rather than reading a guess.
_Static_assert(sizeof(struct dl_phdr_info) == sizeof(struct ldso_phdr_info) &&
               __builtin_offsetof(struct dl_phdr_info, dlpi_phdr) ==
                   __builtin_offsetof(struct ldso_phdr_info, phdr) &&
               __builtin_offsetof(struct dl_phdr_info, dlpi_phnum) ==
                   __builtin_offsetof(struct ldso_phdr_info, phnum) &&
               __builtin_offsetof(struct dl_phdr_info, dlpi_adds) ==
                   __builtin_offsetof(struct ldso_phdr_info, adds) &&
               __builtin_offsetof(struct dl_phdr_info, dlpi_tls_data) ==
                   __builtin_offsetof(struct ldso_phdr_info, tls_data),
               "<link.h> and abi/ldso_api.h must describe one layout");

extern const Elf64_Ehdr __ehdr_start __attribute__((weak, visibility("hidden")));

int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *info, size_t size, void *data),
                    void *data) {
    if (!cb) return 0;
    // A loader present but too old to answer: nothing, not the static
    // path -- in libc.so, __ehdr_start is libc.so's own header.
    if (__ldso_api.magic == LDSO_API_MAGIC)
        return __ldso_api.iterate_phdr
            ? __ldso_api.iterate_phdr(
                  (int (*)(struct ldso_phdr_info *, uint64_t, void *))(void *)cb, data)
            : 0;
    const Elf64_Ehdr *eh = &__ehdr_start;
    if (!eh || eh->e_phentsize != sizeof(Elf64_Phdr)) return 0;
    struct dl_phdr_info info = {
        .dlpi_addr  = 0,     // an ET_EXEC runs where it was linked
        .dlpi_name  = "",
        .dlpi_phdr  = (const Elf64_Phdr *)(const void *)((const char *)eh + eh->e_phoff),
        .dlpi_phnum = eh->e_phnum,
        .dlpi_adds  = 1,
        .dlpi_subs  = 0,
    };
    return cb(&info, sizeof info, data);
}
