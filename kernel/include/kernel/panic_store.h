#ifndef KERNEL_PANIC_STORE_H
#define KERNEL_PANIC_STORE_H

#include <stdint.h>

// THE PANIC STORE: a fixed range of physical RAM that a panic copies the
// kernel log into and the NEXT boot reads back -- Linux's pstore/ramoops
// shape. It survives only a WARM reset (panic.c's countdown ends in one),
// and only on firmware that does not scrub memory. A record that did not
// survive fails its checksum, so the failure is "no record", never a
// wrong one.
//
// **THE RANGE COMES FROM THE BOOT LINE, NEVER FROM ANYTHING SET UP**,
// because two things need it before anything could be: KASLR (reloc.c)
// must not copy the kernel over it, and pmm must not hand it out. 32 MiB
// by default -- above the kernel image, below where GRUB's heap grows
// down from -- or `panic_store=<addr>`, per machine, the way Linux's
// ramoops takes its range from memmap=, for a machine whose firmware or
// bootloader writes there (`panic_store=survey` finds one that does not).
// panic_store_probe() refuses a range, and says so, on a machine where
// it is not RAM clear of the image and GRUB.
#define PANIC_STORE_BASE 0x2000000ULL   // the default
#define PANIC_STORE_SIZE 0x8000ULL   // the whole 16 KiB klog ring, and a header

// `panic_store=` after the '=': a hex address (0x...) or a size (96M),
// 4 KiB-aligned, from 1 MiB to below 4 GiB (the identity map); `survey`;
// NULL for no word. A parser that rejects rather than guesses: a bad
// value is PANIC_STORE_INVALID and the store is not used that boot.
enum { PANIC_STORE_DEFAULT, PANIC_STORE_EXPLICIT, PANIC_STORE_SURVEY, PANIC_STORE_INVALID };
int panic_store_parse(const char *value, uint64_t *base);

// THE SURVEY (`panic_store=survey`): no store; each candidate range is
// stamped with a pattern at boot, and the NEXT survey boot says which
// came back intact -- how a machine's surviving range is found without
// a panic per try. One line each in the log, then the one to use.
#define PANIC_STORE_CANDIDATES 14

// The i-th range this boot keeps from KASLR and the allocator: the
// store's, or each survey candidate's. 0 past the last. Pure, from the
// boot line, so reloc.c can call it before relocation.
int panic_store_range(int i, uint64_t *base);
// After panic_store_probe(): whether range i is one it found usable --
// NOT contiguous, so callers walk all PANIC_STORE_CANDIDATES.
int panic_store_kept(int i, uint64_t *base);

// Called from pmm_init(), before the frame allocator exists: is the range
// RAM clear of the image and GRUB's allocations, and does it hold a
// record from the previous boot? pmm reserves the range iff usable.
void panic_store_probe(void);
int panic_store_usable(void);

// At panic: copy the kernel log's tail in. No locks, no heap, IF off.
void panic_store_save(void);

// The previous boot's record, or NULL when there is none (or it has been
// cleared). The text stays where it is -- the range is reserved.
const char *panic_store_text(uint32_t *len, uint64_t *uptime_ns, const char **build);
void panic_store_clear(void);

// The record format, as pure functions over a caller's buffer so the
// KTESTs can reach it. The text is written at PANIC_RECORD_TEXT(buf)
// first, then sealed; check() answers the text length, or -1.
#define PANIC_RECORD_HDR 72u
#define PANIC_RECORD_TEXT(buf) ((char *)(buf) + PANIC_RECORD_HDR)
int panic_record_seal(void *buf, uint32_t cap, uint32_t len,
                      uint64_t uptime_ns, const char *build);
int panic_record_check(const void *buf, uint32_t cap);

#endif
