#ifndef SAUNAOS_TLS_ENTROPY_H
#define SAUNAOS_TLS_ENTROPY_H

// How good is this machine's randomness, asked before a key is drawn
// from it.
//
// Separate from `SYS_GETRANDOM`, which always answers and deliberately
// never grades its answer. See saunaos_platform.c for why keying a TLS
// connection is the one case where the grade has to be looked at.
#include <stddef.h>

enum saunaos_entropy_grade {
    SAUNAOS_ENTROPY_NONE = 0, // krandom_init() has not run; a fixed sequence
    SAUNAOS_ENTROPY_WEAK,     // TSC jitter -- software timing software, under QEMU
    SAUNAOS_ENTROPY_GOOD,     // virtio-rng or RDSEED/RDRAND
    SAUNAOS_ENTROPY_UNKNOWN,  // the query failed; treat as no better than WEAK
};

// Fills `name` with the source's own description ("TSC jitter",
// "hardware (RDSEED/RDRAND)") when `cap` is non-zero, so a caller can
// say WHICH source it is refusing rather than only that it refused.
enum saunaos_entropy_grade saunaos_entropy_grade(char *name, size_t cap);

#endif
