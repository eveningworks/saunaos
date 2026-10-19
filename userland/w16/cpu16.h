// cpu16.h -- w16's CPU: an interpreter for the 16-bit x86 code a Windows
// 3.x program is made of (8086, 186 and 286 integer instructions, 286
// protected-mode segment loads). docs/w16-design.md says why it is
// emulated rather than run natively.
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

enum cpu16_reg { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum cpu16_sreg { S_ES, S_CS, S_SS, S_DS };

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

struct cpu16_desc {
    uint8_t *base;   // host memory, at least limit+1 bytes
    uint32_t limit;  // the highest valid offset
    uint8_t kind;    // enum cpu16_kind
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

struct cpu16;

// An import was reached: SS:SP holds the far return address, the
// arguments above it. Return the bytes of arguments to pop (the Pascal
// `retf n`), or a negative value after setting a fault.
typedef int (*cpu16_thunk_fn)(struct cpu16 *c, uint16_t sel, uint16_t off);
// INT n. Return 0 when handled, negative after setting a fault.
typedef int (*cpu16_int_fn)(struct cpu16 *c, uint8_t n);

struct cpu16 {
    uint16_t r[8];
    uint16_t ip, flags;
    uint16_t sreg[4];
    struct cpu16_desc *desc;     // CPU16_MAX_DESC entries; selector = index << 3 | 7
    cpu16_thunk_fn thunk;
    cpu16_int_fn intr;
    void *user;
    int fault;
    uint16_t fault_cs, fault_ip;
    uint16_t fault_r[8], fault_sreg[4];  // the registers AS THE FAULT FOUND THEM: the
                                         // calls it unwinds through restore r and sreg
    char fault_why[96];
    int exited;                  // the program ended (INT 21h 4Ch); exit_code says how
    int exit_code;
    unsigned depth;              // nested cpu16_call_far()s
    uint64_t steps;
};

// Selectors.
static inline uint16_t cpu16_sel(unsigned index) { return (uint16_t)(index << 3 | 7); }
static inline unsigned cpu16_index(uint16_t sel) { return sel >> 3; }

// A free descriptor slot (from 1; index 0 is the null selector), or 0.
uint16_t cpu16_alloc(struct cpu16 *c, uint8_t kind, uint8_t *base, uint32_t limit, uint16_t owner);

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
// A NUL-terminated string at sel:off, at most cap-1 bytes copied; -1 bad.
int cpu16_str(struct cpu16 *c, uint16_t sel, uint16_t off, char *out, unsigned cap);

void cpu16_set_fault(struct cpu16 *c, int kind, const char *why);
const char *cpu16_fault_name(int kind);

#endif
