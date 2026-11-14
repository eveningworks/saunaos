// lin16.h -- w16's linear address space (lin16.c): the one arena every byte a
// program reaches through a selector is allocated from, so each selector has
// the 32-bit LINEAR base DPMI and Win386 programs need.
#ifndef W16_LIN16_H
#define W16_LIN16_H

#include <stdint.h>

struct cpu16;

// Reserve the arena (once per process) and point the CPU's linear fields at
// it. 0, or -1 when it cannot be mapped.
int lin16_init(struct cpu16 *c);

// Zeroed bytes in the arena, aligned to `align` (at least 16); NULL when full.
void *lin16_alloc(uint32_t size, uint32_t align);
// Grown in place when it can be, else moved; new bytes zero. NULL on failure (the old block is kept).
void *lin16_realloc(void *p, uint32_t size);
void lin16_free(void *p);
// A block's size as allocated (rounded up), 0 for a pointer the arena did not hand out.
uint32_t lin16_size_of(const void *p);
uint32_t lin16_free_bytes(void);
uint32_t lin16_largest_free(void);

#endif
