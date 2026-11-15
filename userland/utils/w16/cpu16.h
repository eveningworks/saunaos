// cpu16.h -- w16's CPU: an interpreter for the 16-bit x86 code a Windows
// 3.x program is made of (8086 to 386 integer instructions, 286
// protected-mode segment loads). The 386's part is what a compiler's /G3
// put in 16-bit segments: 32-bit registers, operand and address sizes
// (the 66 and 67 prefixes, SIB), FS and GS, and its 0F instructions --
// and 32-bit SEGMENTS (the D and B bits), for the Watcom Win386 programs
// whose 32-bit flat code runs in selectors DPMI made (dpmi16.c).
// docs/w16-design.md says why it is emulated rather than run natively.
//
// THE PROGRAM IS UNTRUSTED. A segment register holds a SELECTOR into
// w16's own table (struct cpu16_desc), never a host address, and every
// memory access is checked against that segment's limit: a bad access is
// a FAULT that stops the program with CS:IP and a reason, never a read
// outside the segment's host buffer.
//
// Imports are THUNK selectors: when control reaches one (a far call,
// jump or return), the interpreter runs the C function behind it with
// SS:SP at its far return address and its arguments above, then does
// the Pascal `retf n` itself. Calls back into the program (a window
// procedure) go through cpu16_call_far(), which runs the interpreter
// until the callee returns to a sentinel.
#ifndef W16_CPU16_H
#define W16_CPU16_H

#include <stdint.h>

#define CPU16_MAX_DESC 8192          // an LDT's worth
#define CPU16_OWNER_TILE 0xFFFE      // a descriptor's owner: the 2nd or later 64 KiB of a huge block
#define CPU16_OWNER_ALLOC 0xFFFB     // ...a selector the program made itself (AllocSelector, DPMI 0000h),
                                     // the only kind it may free or re-point

enum cpu16_reg { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum cpu16_sreg { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };
#define CPU16_SREGS 6

// Flags.
#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

enum cpu16_kind {
    D_FREE = 0,
    D_CODE,          // execute and read
    D_DATA,          // read and write
    D_THUNK,         // an import: the offset names the function
    D_SENTINEL,      // the return address of cpu16_call_far()
};

#define CPU16_DF_BIG 0x01    // the D bit (code: 32-bit operands, addresses, EIP) and B bit (stack: ESP)
#define CPU16_DF_LIN 0x02    // the base was SET as a linear address (`lin`): accesses go through the
                             // arena modulo 2^32 (lin16.c), `base` is not used

struct cpu16_desc {
    uint8_t *base;   // host memory, at least limit+1 bytes (unless CPU16_DF_LIN)
    uint32_t limit;  // the highest valid offset
    uint32_t lin;    // the linear base, when CPU16_DF_LIN
    uint8_t kind;    // enum cpu16_kind
    uint8_t flags;   // CPU16_DF_*
    uint16_t owner;  // the NE segment number, for fault reports; 0 none
};

enum cpu16_fault {
    FAULT_NONE = 0,
    FAULT_GP,        // a bad selector, or an access past a segment's limit
    FAULT_SS,        // the same on the stack
    FAULT_UD,        // an instruction w16 does not implement
    FAULT_DE,        // divide error
    FAULT_INT,       // a software interrupt nobody handles
    FAULT_HLT,       // a privileged instruction
    FAULT_API,       // an import failed or is not implemented
    FAULT_LIMIT,     // the step budget ran out
};

// The x87 (cpu16_x87.c), a 387 as Windows 3.1 on a 386 had one. THE HOST'S
// OWN FPU runs each instruction: the state is FNSAVE's 32-bit image,
// FRSTORed, the one instruction run on a host buffer, FNSAVEd back -- so
// the arithmetic, rounding, transcendentals and exception responses are a
// real x87's.
struct cpu16_x87 {
    uint8_t img[108];        // the state; its pointer fields are the host's, unused
    uint32_t fip;            // the last non-control instruction, as a 387 keeps it
    uint16_t fcs, fop;
    uint32_t fdp;            // ... and its memory operand
    uint16_t fds;
    uint8_t ready;           // 0: FNINIT's state, made on first use
};

struct cpu16;

#define CPU16_TICK_STEPS (1u << 16)   // ~2 ms of a program at w16's speed: what runs "at interrupt time"

// An import was reached: SS:SP holds the far return address, the
// arguments above it. Return the bytes of arguments to pop (the Pascal
// `retf n`), or a negative value after setting a fault.
typedef int (*cpu16_thunk_fn)(struct cpu16 *c, uint16_t sel, uint16_t off);
// INT n. Return 0 when handled, negative after setting a fault.
typedef int (*cpu16_int_fn)(struct cpu16 *c, uint8_t n);
// IN or OUT of `size` bytes (1, 2 or 4) at `port`: an IN sets *val. Return 0, or negative after
// setting a fault. Without one, port I/O is a fault.
typedef int (*cpu16_io_fn)(struct cpu16 *c, uint16_t port, unsigned size, int out, uint32_t *val);

struct cpu16 {
    uint16_t r[8];
    uint16_t rh[8];              // the 386's high words: EAX is rh[R_AX] << 16 | r[R_AX], so a 16-bit
                                 // write -- the API side's included -- keeps them, as a 386 does
    uint32_t ip;                 // EIP: 16 bits unless CS is a 32-bit segment
    uint8_t cs32;                // CS's D bit, as the instruction being run found it (cpu16.c keeps it)
    uint16_t flags;
    uint16_t sreg[CPU16_SREGS];
    struct cpu16_desc *desc;     // CPU16_MAX_DESC entries; selector = index << 3 | 7
    cpu16_thunk_fn thunk;
    cpu16_int_fn intr;
    cpu16_io_fn io;
    void *user;
    int fault;
    uint16_t fault_cs;
    uint32_t fault_ip;
    uint16_t fault_r[8], fault_sreg[CPU16_SREGS];  // the registers AS THE FAULT FOUND THEM: the
                                                   // calls it unwinds through restore r and sreg
    char fault_why[96];
    int exited;                  // the program ended (INT 21h 4Ch); exit_code says how
    int exit_code;
    unsigned depth;              // nested cpu16_call_far()s
    uint64_t steps;
    void (*tick)(struct cpu16 *c);   // every CPU16_TICK_STEPS instructions, when set
    struct cpu16_x87 x87;
    // The linear arena (lin16.c), or NULL: the CPU runs without one (the host check).
    uint8_t *lin;
    uint32_t lin_size, lin_origin;

    // THE 386'S DESCRIPTOR CACHE, in w16's shape (cpu16.c). Each segment register's descriptor
    // is found once, when the register's value changes, not on every access -- the entry is
    // still read live, so a limit or base w16 changes takes effect at once. CS's bytes are a
    // FETCH WINDOW: [fetch_lo, fetch_lo + fetch_n) of the segment at fetch_host, rebuilt when CS
    // or desc_gen changes; whoever changes a descriptor in place calls cpu16_desc_changed().
    const struct cpu16_desc *segc[CPU16_SREGS];
    uint16_t segc_sel[CPU16_SREGS];
    const uint8_t *fetch_host;
    uint32_t fetch_lo, fetch_n, fetch_gen, desc_gen;
    uint16_t fetch_sel;
    uint16_t sentinel;           // cpu16_call_far()'s return selector, 0 until the first call
};

// A descriptor was changed in place (its base, limit or kind): CS's fetch window is rebuilt.
static inline void cpu16_desc_changed(struct cpu16 *c) { c->desc_gen++; }

// Selectors.
static inline uint16_t cpu16_sel(unsigned index) { return (uint16_t)(index << 3 | 7); }
static inline unsigned cpu16_index(uint16_t sel) { return sel >> 3; }

// A free descriptor slot (from 1; index 0 is the null selector), or 0.
uint16_t cpu16_alloc(struct cpu16 *c, uint8_t kind, uint8_t *base, uint32_t limit, uint16_t owner);
// `n` selectors in a row, one step (8) apart, tiling `size` bytes at `base` 64 KiB each: a huge block.
// EACH ONE'S LIMIT RUNS TO THE BLOCK'S END, as Windows 3.1's enhanced mode set them: 16-bit code steps
// a selector at a time, and 386 code takes the first with a 32-bit offset (DK's WinG pixels).
// The first has owner `owner`, the others `tail_owner`. 0 when no run of n is free.
uint16_t cpu16_alloc_run(struct cpu16 *c, unsigned n, uint8_t kind, uint8_t *base, uint32_t size, uint16_t owner,
                         uint16_t tail_owner);

// A selector's LINEAR base (lin16.c): 0, or -1 for one that has no memory in the arena.
int cpu16_linear_base(struct cpu16 *c, uint16_t sel, uint32_t *out);
// Set it, as DPMI 0007h does: from then on the selector reaches the arena at that linear address.
void cpu16_set_linear_base(struct cpu16 *c, uint16_t sel, uint32_t lin);
// The host bytes [lin, lin+len) of the arena, or NULL when they are not all inside it.
uint8_t *cpu16_linear_ptr(struct cpu16 *c, uint32_t lin, uint32_t len);

// Load a segment register as a mov would: checked against the table.
int cpu16_load_sreg(struct cpu16 *c, int s, uint16_t sel);

// Run until the program exits, faults, or a cpu16_call_far() at this
// depth returns. max_steps 0: no budget.
int cpu16_run(struct cpu16 *c, uint64_t max_steps);

// One instruction (its prefixes included). Negative after a fault.
int cpu16_step(struct cpu16 *c);

// Call SEL:OFF with `nargs` words already pushed by the caller (Pascal
// order), DS and AX as the callee expects; returns when it returns.
// The result is in DX:AX. Negative after a fault.
int cpu16_call_far(struct cpu16 *c, uint16_t sel, uint16_t off);

// Stack and memory helpers for the API side (all checked; a failed one
// sets a fault and returns 0).
void cpu16_push(struct cpu16 *c, uint16_t v);
uint16_t cpu16_pop(struct cpu16 *c);
uint16_t cpu16_arg(struct cpu16 *c, unsigned byte_off);   // SS:SP+4+byte_off
uint8_t *cpu16_ptr(struct cpu16 *c, uint16_t sel, uint16_t off, uint32_t len, int write);
uint8_t *cpu16_ptr32(struct cpu16 *c, uint16_t sel, uint32_t off, uint32_t len, int write);   // a 32-bit offset
// A NUL-terminated string at sel:off, at most cap-1 bytes copied; -1 bad.
int cpu16_str(struct cpu16 *c, uint16_t sel, uint16_t off, char *out, unsigned cap);

void cpu16_set_fault(struct cpu16 *c, int kind, const char *why);
const char *cpu16_fault_name(int kind);

// --- the x87 ------------------------------------------------------------------
//
// An UNMASKED exception does what a PC's did: nothing until the next
// waiting FPU instruction or FWAIT, which then raises INT 75h (IRQ 13,
// the coprocessor's) with IP back at itself, so returning re-runs it.
// The INT 75h handler must clear the exception or that loops.

#define X87_IE 0x01
#define X87_DE 0x02
#define X87_ZE 0x04
#define X87_OE 0x08
#define X87_UE 0x10
#define X87_PE 0x20
#define X87_SF 0x40

// One ESC instruction (opcode D8-DF and its ModRM byte), for cpu16.c:
// `seg` is the memory operand's segment register (-1 for a register
// form), `off` its offset, `cs:ip` where the instruction began; `o32` the
// operand size, which picks FLDENV/FNSTENV/FRSTOR/FNSAVE's 16-bit (14/94
// bytes) or 32-bit (28/108) protected-mode layout.
void cpu16_x87_op(struct cpu16 *c, uint8_t op, uint8_t modrm, int seg, uint32_t off, uint16_t cs, uint32_t ip,
                  int o32);
// Does this ESC instruction wait (check for a pending exception first)?
int cpu16_x87_waits(uint8_t op, uint8_t modrm);
// The unmasked exceptions pending (X87_*), 0 when none.
unsigned cpu16_x87_pending(struct cpu16 *c);
// One instruction on a HOST buffer as its memory operand (`mem` is that
// many bytes, read and written as the instruction does), for an API that
// works on the program's FPU (WIN87EM). Returns the unmasked exceptions it
// raised, -1 for an instruction that is not a 387's.
int cpu16_x87_exec(struct cpu16 *c, uint8_t op, uint8_t modrm, uint8_t *mem);
// The state as FNSAVE's 32-bit image, as the program sees it (its control
// word, ES and B), and back.
void cpu16_x87_save(struct cpu16 *c, uint8_t out[108]);
void cpu16_x87_load(struct cpu16 *c, const uint8_t in[108]);
uint16_t cpu16_x87_status(struct cpu16 *c);
void cpu16_x87_set_cw(struct cpu16 *c, uint16_t cw);
void cpu16_x87_clear(struct cpu16 *c);      // FNCLEX
void cpu16_x87_init(struct cpu16 *c);       // FNINIT

#endif
