// uACPI's memory primitives, mapped onto the kernel's (UACPI_OVERRIDE_LIBC).
//
// THE TRAP this file exists for: uACPI's default is __builtin_memcpy and
// friends, which GCC lowers to a CALL to memcpy() for a length it cannot
// see -- and the kernel defines no memcpy, deliberately (the Makefile's
// -fno-tree-loop-distribute-patterns note), so that would be a link
// error. The string functions (strlen, strcmp, snprintf...) are left to
// uACPI's own implementations in stdlib.c.
#pragma once

#include <stddef.h>
#include "string.h"   // k_memcpy, k_memmove, k_memset, k_memcmp

static inline void *uacpi_glue_memcpy(void *d, const void *s, size_t n) {
    k_memcpy(d, s, n);
    return d;
}

static inline void *uacpi_glue_memmove(void *d, const void *s, size_t n) {
    k_memmove(d, s, n);
    return d;
}

static inline void *uacpi_glue_memset(void *d, int c, size_t n) {
    k_memset(d, (uint8_t)c, n);
    return d;
}

#define uacpi_memcpy  uacpi_glue_memcpy
#define uacpi_memmove uacpi_glue_memmove
#define uacpi_memset  uacpi_glue_memset
#define uacpi_memcmp  k_memcmp
