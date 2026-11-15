// jit16.h -- w16's JIT: a Win386 program's hot 32-bit FLAT code translated to x86-64, one basic
// block at a time (docs/w16-jit-design.md, stage 1). The interpreter (cpu16.c) stays the
// reference and does everything a block does not: a block ends before the first instruction it
// does not handle, and any access it cannot prove in bounds -- or a store into memory it was made
// from -- leaves the block BEFORE that instruction, which the interpreter then runs and faults or
// invalidates exactly as it always did. Translated code therefore never faults.
//
// W^X: blocks are written into an RW buffer and copied into the code arena, whose pages are made
// RX again at once (mprotect), so no page is ever writable and executable. Under SaunaOS's
// sandbox that needs the `prot_exec` promise.
#ifndef W16_JIT16_H
#define W16_JIT16_H

#include <stdint.h>

struct cpu16;

// What the JIT has done, for the timing build and `--jit-stats`.
struct jit16_stats {
    uint64_t blocks;            // translated
    uint64_t insns;             // guest instructions in them
    uint64_t runs;              // block executions
    uint64_t run_insns;         // guest instructions executed in translated code
    uint64_t exit_slow;         // left before an access it could not prove (the interpreter ran it)
    uint64_t exit_end;          // left at the end of a block (a branch, or an instruction it lacks)
    uint64_t invalidated;       // blocks dropped by a store into their code
    uint64_t flushes;           // the whole arena dropped because it was full
    uint64_t refused;           // block starts it could not translate (first instruction unknown)
    uint64_t code_bytes;        // host code written
    uint64_t guard_miss;        // a block found with its segments changed under it
    uint32_t stop_op[256];      // the opcode each block ENDED at, when it ended for want of one
    uint32_t stop_0f[256];      // ...and 0F's second byte
    uint64_t run_stop[512];     // how often a RUN ended there, the interpreter taking over (0F xx at 256+)
    uint64_t callout[512];      // instructions a block handed the interpreter one at a time, and ran on
    uint64_t callout_exit;      // ...after which it left (a segment loaded, translated code dropped, a fault)
};

// Turns the JIT on for `c` (whose linear arena, if any, is set already). `threshold`: how many
// arrivals at a block start before it is translated (0 = the default). 0, or -1 when the code
// arena cannot be had -- the interpreter then runs alone.
int jit16_init(struct cpu16 *c, unsigned threshold);
void jit16_free(struct cpu16 *c);

// Runs translated blocks from CS:EIP for as long as there are blocks to run and `until` (0: no
// budget) is not reached, adding what they ran to c->steps. 0 when nothing ran -- the caller's
// interpreter takes the instruction.
int jit16_run(struct cpu16 *c, uint64_t until);

// Drops every translation (the C side has rewritten code the CPU will run, as a test driver does
// between cases).
void jit16_flush(struct cpu16 *c);

// A store into [rel, rel+len) of the arena: the blocks made from those bytes are dropped.
void jit16_store(struct cpu16 *c, uint32_t rel, uint32_t len);

const struct jit16_stats *jit16_stats(const struct cpu16 *c);

#endif
