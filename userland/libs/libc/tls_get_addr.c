// __tls_get_addr() -- the address of one thread-local variable of a
// shared object, for global- and local-dynamic code (-fpic's default).
//
// HERE AND NOT IN THE LOADER, because ld-toy has no .dynsym for a library
// to resolve against (abi/ldso_api.h); glibc keeps it in ld.so. The fast
// path is the psABI's: the DTV pointer at %fs:8, the module's block
// already there. Anything else -- a module dlopen()ed after this thread
// started, or a block not mapped yet -- is the loader's (tls_get_addr).
#include <stdint.h>
#include "ldso_api.h"

extern struct ldso_api __ldso_api;

struct tls_index { uint64_t module, offset; };

void *__tls_get_addr(struct tls_index *ti) {
    uint64_t *dtv;
    __asm__ volatile ("mov %%fs:8, %0" : "=r"(dtv));
    if (dtv && ti->module <= dtv[0] && dtv[ti->module])
        return (char *)(uintptr_t)dtv[ti->module] + ti->offset;
    return __ldso_api.tls_get_addr(ti->module, ti->offset);
}
