#ifndef ABI_AUXV_H
#define ABI_AUXV_H

// The auxiliary vector: (type, value) pairs the kernel places after
// envp's NULL terminator, ending with AT_NULL -- SysV's shape, Linux's
// type numbers. EVERY process gets one, holding at least AT_RANDOM
// (the stack-protector canary's seed, read by rt/tls.c and ld-toy); a
// dynamic executable's also tells /lib/ld-toy.so where the program it
// must finish loading is.
//
// AT_PHDR points INTO THE MAPPED IMAGE: a dynamic executable's first
// PT_LOAD carries the ELF and program headers (userland/libs/rt/link-dyn.ld
// says FILEHDR PHDRS), so the kernel passes base + e_phoff and copies
// nothing.

#define AT_NULL  0
#define AT_PHDR  3 // the executable's program headers, mapped
#define AT_PHENT 4 // sizeof one program header
#define AT_PHNUM 5
#define AT_ENTRY 9 // the EXECUTABLE's entry -- where ld-toy jumps when done
#define AT_RANDOM 25 // AT_RANDOM_BYTES random bytes on the stack, ABOVE the strings
#define AT_RANDOM_BYTES 16

#include <stdint.h>

// Every type above is below this; a word at or past it ends a walk. A
// static program on a kernel older than AT_RANDOM had NO auxv, so what
// follows its envp NULL is string bytes -- huge as words -- not pairs.
#define AT_TYPE_LIMIT 64

// `auxv` is the first pair after envp's NULL; 0 for a type not there.
static inline __attribute__((always_inline))
uint64_t auxv_get(const uint64_t *auxv, uint64_t type) {
    for (; auxv[0] != AT_NULL && auxv[0] < AT_TYPE_LIMIT; auxv += 2)
        if (auxv[0] == type) return auxv[1];
    return 0;
}

// THE STACK CANARY, from AT_RANDOM's first 8 bytes with the low byte zero
// (glibc's rule: a string copy that reaches it stops there), or 0 with no
// AT_RANDOM. ld-toy and rt/tls.c both seed from this, so they agree. The
// bytes lie above the vector; a pointer that does not is no AT_RANDOM.
static inline __attribute__((always_inline))
uint64_t auxv_canary(const uint64_t *auxv) {
    uint64_t at = auxv_get(auxv, AT_RANDOM);
    if (at <= (uint64_t)(uintptr_t)auxv) return 0;
    uint64_t v;
    __builtin_memcpy(&v, (const void *)(uintptr_t)at, sizeof v);
    return v & ~(uint64_t)0xFF;
}

#endif
