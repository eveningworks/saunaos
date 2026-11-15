// jit16_internal.h -- what jit16.c (the block cache, the dispatcher, invalidation) and
// jit16_x86.c (decoding a guest block and emitting its x86-64) share. Split by concern, as
// userland/apps/system/toywm/ is: the cache knows nothing of x86 encodings, the translator
// nothing of hash tables.
#ifndef W16_JIT16_INTERNAL_H
#define W16_JIT16_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "cpu16.h"
#include "jit16.h"

#define JIT_MAX_INSNS 64             // a block's guest instructions, at most
#define JIT_CODE_MAX  (32u * 1024)   // its host code, at most (64 instructions' worst case and exits)
#define JIT_NO_REL    0xFFFFFFFFu    // a block whose code is outside the arena (a test's segments)

// A translated block, valid while CS, DS and SS are the selectors -- and the descriptors -- it
// was made for: its memory accesses are compiled against their bases and limits.
struct jblock {
    const uint8_t *host;             // the guest code's first byte
    uint32_t ip, len;                // its EIP and how many bytes of guest code it covers
    uint32_t ninsn;                  // instructions; a block that returns fewer left early
    uint32_t rel;                    // arena offset of `host`, or JIT_NO_REL
    uint16_t cs, ds, ss;
    struct cpu16_desc cs_d, ds_d, ss_d;
    uint32_t gen;                    // c->desc_gen when the descriptors were last seen equal
    uint32_t entry;                  // the function's offset in its code (an epilogue comes first)
    uint32_t (*fn)(struct cpu16 *c);
    struct jblock *page_next;        // the other blocks of its arena page
    struct jblock *all_next;         // every block, for the flush
    int16_t stop;                    // the opcode it ended BEFORE for want of it (0F xx: 256 + xx), or -1
    uint8_t live;
};

// A translated block's call into the interpreter for one instruction it lacks (at c->ip, flags
// written back): 0 to run on at `next`, 1 to leave (c->ip is right), 2 for a fault (leave, the
// instruction not counted). `op` (0F xx at 256 + xx) is counted.
uint32_t jit16_callout(struct cpu16 *c, uint32_t next, uint32_t op);

// The translator: decodes guest code at `host` (`avail` bytes are readable) for CPU `c`, whose
// segment registers it reads, and emits a block function into buf[0..cap). Fills b->ninsn,
// b->len and b's segment snapshot, returns the host code's length -- 0 when not even the first
// instruction can be translated, with *stop_op/*stop_0f naming it.
uint32_t jit16_translate(struct cpu16 *c, struct jblock *b, const uint8_t *host, uint32_t avail,
                         uint8_t *buf, uint32_t cap, int *stop_op, int *stop_0f);

#endif
