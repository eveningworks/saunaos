// cpu16_check.c -- the driver of tools/cpu16_hostcheck.py: w16's 16-bit
// interpreter (userland/w16/cpu16.c) against THE REAL CPU. Each case is
// one random instruction with random registers, flags and memory; it is
// run by the interpreter, and natively in a 16-bit code segment the
// host kernel's LDT describes (modify_ldt), entered from 64-bit mode by
// a far call -- Wine's route for 16-bit code, used here as an oracle.
// Registers, flags (the instruction's undefined ones masked) and both
// segments' bytes must match.
//
// Built -no-pie: the far call's return address and the stack the 16-bit
// code saves are 32-bit, so the code and that stack live below 4 GiB.
#define _GNU_SOURCE
#include <asm/ldt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "cpu16.h"

#define E_CODE 1
#define E_DATA 2
#define E_STACK 3
#define SEL(e) ((uint16_t)((e) << 3 | 7))

#define IN_BLOCK  0xFF00     // in the stack segment: registers and flags in
#define OUT_BLOCK 0xFF20     // ... and out
#define SAVE_AREA 0xFFF0     // the host's ESP and SS
#define TEST_SP   0xF000
#define CMP_STACK 0xFF00     // stack bytes compared: below the blocks

static uint8_t *g_code, *g_data, *g_stack, *g_lowstack;
static uint8_t g_idata[0x10000], g_istack[0x10000], g_icode[0x10000];
static struct cpu16_desc g_desc[CPU16_MAX_DESC];
static uint64_t g_saved_rsp;
static uint32_t g_rng = 1;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void *low_map(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

static void set_ldt(int entry, void *base, int code) {
    struct user_desc d = {
        .entry_number = (unsigned)entry, .base_addr = (unsigned)(uintptr_t)base, .limit = 0xFFFF,
        .seg_32bit = 0, .contents = code ? MODIFY_LDT_CONTENTS_CODE : MODIFY_LDT_CONTENTS_DATA,
        .read_exec_only = 0, .limit_in_pages = 0, .seg_not_present = 0, .useable = 1,
    };
    if (syscall(SYS_modify_ldt, 1, &d, sizeof d) != 0) { perror("modify_ldt"); exit(3); }
}

// --- the native run --------------------------------------------------------

static unsigned emit(uint8_t *p, unsigned at, const uint8_t *b, unsigned n) {
    memcpy(p + at, b, n);
    return at + n;
}

// The 16-bit code: save the host's SS:ESP, switch to our stack, set
// DS=ES=data, load the registers and flags, run the test, store them,
// restore the host's stack and `o32 retf` back to 64-bit mode.
static unsigned build_native(const uint8_t *test, unsigned len) {
    uint8_t *p = g_code;
    unsigned a = 0;
    const uint8_t pro[] = {
        0xB8, SEL(E_STACK) & 0xFF, SEL(E_STACK) >> 8,     // mov ax, stack
        0x8E, 0xD8,                                       // mov ds, ax
        0x66, 0x89, 0x26, SAVE_AREA & 0xFF, SAVE_AREA >> 8,   // mov [save], esp
        0x8C, 0x16, (SAVE_AREA + 4) & 0xFF, (SAVE_AREA + 4) >> 8,   // mov [save+4], ss
        0x8E, 0xD0,                                       // mov ss, ax
        0xBC, TEST_SP & 0xFF, TEST_SP >> 8,               // mov sp, TEST_SP
        0x68, SEL(E_DATA) & 0xFF, SEL(E_DATA) >> 8, 0x1F, // push data; pop ds
        0x68, SEL(E_DATA) & 0xFF, SEL(E_DATA) >> 8, 0x07, // push data; pop es
        0x36, 0xFF, 0x36, 0x10, 0xFF,                     // push word [ss:IN+0x10]
        0x9D,                                             // popf
        0x36, 0xA1, 0x00, 0xFF,                           // mov ax, [ss:IN]
        0x36, 0x8B, 0x0E, 0x02, 0xFF,                     // mov cx, [ss:IN+2]
        0x36, 0x8B, 0x16, 0x04, 0xFF,                     // mov dx
        0x36, 0x8B, 0x1E, 0x06, 0xFF,                     // mov bx
        0x36, 0x8B, 0x2E, 0x0A, 0xFF,                     // mov bp
        0x36, 0x8B, 0x36, 0x0C, 0xFF,                     // mov si
        0x36, 0x8B, 0x3E, 0x0E, 0xFF,                     // mov di
    };
    a = emit(p, a, pro, sizeof pro);
    unsigned at = a;
    a = emit(p, a, test, len);
    const uint8_t epi[] = {
        0x36, 0xA3, 0x20, 0xFF,                           // mov [ss:OUT], ax
        0x36, 0x89, 0x0E, 0x22, 0xFF,                     // cx
        0x36, 0x89, 0x16, 0x24, 0xFF,                     // dx
        0x36, 0x89, 0x1E, 0x26, 0xFF,                     // bx
        0x36, 0x89, 0x26, 0x28, 0xFF,                     // sp
        0x36, 0x89, 0x2E, 0x2A, 0xFF,                     // bp
        0x36, 0x89, 0x36, 0x2C, 0xFF,                     // si
        0x36, 0x89, 0x3E, 0x2E, 0xFF,                     // di
        0x9C,                                             // pushf
        0x36, 0x8F, 0x06, 0x30, 0xFF,                     // pop word [ss:OUT+0x10]
        0x36, 0x66, 0x0F, 0xB2, 0x26, SAVE_AREA & 0xFF, SAVE_AREA >> 8,   // lss esp, [ss:save]
        0x66, 0xCB,                                       // o32 retf
    };
    emit(p, a, epi, sizeof epi);
    return at;
}

// The FPU state goes in by FRSTOR before the far call and comes out by
// FNSAVE after it, neither of which waits on what the test left pending.
static void run_native(const uint8_t *fin, uint8_t *fout) {
    static struct { uint32_t off; uint16_t sel; } __attribute__((packed)) fp;
    fp.off = 0;
    fp.sel = SEL(E_CODE);
    uint64_t top = (uint64_t)(uintptr_t)(g_lowstack + 65536 - 64);
    __asm__ volatile(
        "push %%rbx\n push %%rbp\n push %%r12\n push %%r13\n push %%r14\n push %%r15\n"
        "mov %%rsp, %[saved]\n"
        "mov %[top], %%rsp\n"
        "frstor (%[fin])\n"
        "lcall *%[fp]\n"
        "fnsave (%[fout])\n"
        "mov %[saved], %%rsp\n"
        "pop %%r15\n pop %%r14\n pop %%r13\n pop %%r12\n pop %%rbp\n pop %%rbx\n"
        : [saved] "+m"(g_saved_rsp)
        : [top] "r"(top), [fp] "m"(fp), [fin] "r"(fin), [fout] "r"(fout)
        : "memory", "cc", "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
}

// --- the cases ---------------------------------------------------------------

struct regs { uint16_t r[8], flags; };

#define U_AF F_AF
#define U_ALL (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

static unsigned rand_reg_not_sp(void) {
    unsigned r;
    do r = rnd() % 8; while (r == R_SP);
    return r;
}

// A memory operand that stays inside the segments: [SI], [DI], [BX],
// [BX+SI+d8], [BP+DI] -- the registers are kept small by the caller.
static uint8_t rand_mem_modrm(unsigned reg, uint8_t *disp, unsigned *dlen) {
    static const uint8_t forms[][2] = { {0, 4}, {0, 5}, {0, 7}, {1, 0}, {0, 3}, {1, 6} };
    unsigned f = rnd() % 6;
    *dlen = forms[f][0];
    if (*dlen) *disp = (uint8_t)(rnd() % 0x80);
    return (uint8_t)(forms[f][0] << 6 | reg << 3 | forms[f][1]);
}

// One test instruction into b; returns its length and the flags it leaves
// undefined in *undef.
static unsigned gen(uint8_t *b, uint16_t *undef, struct regs *in) {
    unsigned n = 0, dlen;
    uint8_t disp = 0;
    *undef = 0;
    unsigned kind = rnd() % 16;
    int w = rnd() & 1;
    // dispatch-ok: one case per instruction family the oracle covers, bounded by the ISA.
    switch (kind) {
    case 0: case 1: {                                     // ALU reg,reg / reg,mem
        unsigned op = (rnd() % 8) << 3 | (rnd() % 4);
        w = op & 1;
        unsigned reg = rnd() % 8, rm = rnd() % 8;
        int dest_is_rm = !(op & 2);
        if (w && ((dest_is_rm && rm == R_SP) || (!dest_is_rm && reg == R_SP))) { rm = R_AX; reg = R_BX; }
        b[n++] = (uint8_t)op;
        if (rnd() & 1) b[n++] = (uint8_t)(0xC0 | reg << 3 | rm);
        else {
            if (w && reg == R_SP) reg = R_AX;
            b[n++] = rand_mem_modrm(reg, &disp, &dlen);
            if (dlen) b[n++] = disp;
        }
        if ((op >> 3) == 1 || (op >> 3) == 4 || (op >> 3) == 6) *undef = U_AF;
        break;
    }
    case 2: {                                             // ALU acc,imm / group 1
        unsigned op = (rnd() % 8);
        if (rnd() & 1) {
            b[n++] = (uint8_t)(op << 3 | 4 | w);
            b[n++] = (uint8_t)rnd();
            if (w) b[n++] = (uint8_t)rnd();
        } else {
            uint8_t opc = (uint8_t)(0x80 | (rnd() % 4));
            if (opc == 0x82) opc = 0x80;
            unsigned rm = rnd() % 8;
            if ((opc & 1) && rm == R_SP) rm = R_DX;
            b[n++] = opc;
            b[n++] = (uint8_t)(0xC0 | op << 3 | rm);
            b[n++] = (uint8_t)rnd();
            if (opc == 0x81) b[n++] = (uint8_t)rnd();
        }
        if (op == 1 || op == 4 || op == 6) *undef = U_AF;
        break;
    }
    case 3: {                                             // inc/dec
        unsigned r = rand_reg_not_sp();
        b[n++] = (uint8_t)((rnd() & 1 ? 0x40 : 0x48) | r);
        break;
    }
    case 4: {                                             // shifts and rotates
        unsigned op = rnd() % 8, rm = rand_reg_not_sp(), form = rnd() % 3;
        uint8_t opc = form == 0 ? (uint8_t)(0xC0 | w) : form == 1 ? (uint8_t)(0xD0 | w) : (uint8_t)(0xD2 | w);
        b[n++] = opc;
        b[n++] = (uint8_t)(0xC0 | op << 3 | rm);
        unsigned count = form == 1 ? 1 : form == 0 ? rnd() % 40 : (uint8_t)in->r[R_CX];
        if (form == 0) b[n++] = (uint8_t)count;
        if (form == 2 && rm == R_CX) { b[1] = (uint8_t)(0xC0 | op << 3 | R_DX); }
        count &= 0x1F;
        *undef = U_AF;
        if (count != 1) *undef |= F_OF;
        unsigned bits = w ? 16 : 8;
        if ((op == 4 || op == 5 || op == 6) && count >= bits) *undef |= F_CF;
        if (op < 4 && count == 0) *undef = 0;
        break;
    }
    case 5: {                                             // group 3
        unsigned op = rnd() % 8;
        unsigned rm = rand_reg_not_sp();
        b[n++] = (uint8_t)(0xF6 | w);
        b[n++] = (uint8_t)(0xC0 | op << 3 | rm);
        if (op <= 1) { b[n++] = (uint8_t)rnd(); if (w) b[n++] = (uint8_t)rnd(); *undef = U_AF; }
        if (op == 4 || op == 5) *undef = F_SF | F_ZF | F_AF | F_PF;
        if (op >= 6) *undef = U_ALL;
        break;
    }
    case 6: {                                             // mov forms
        unsigned r = rand_reg_not_sp();
        switch (rnd() % 4) {
        case 0: b[n++] = (uint8_t)(0xB8 | r); b[n++] = (uint8_t)rnd(); b[n++] = (uint8_t)rnd(); break;
        case 1: b[n++] = (uint8_t)(0xB0 | (rnd() % 8)); b[n++] = (uint8_t)rnd(); break;
        case 2:
            b[n++] = (uint8_t)(0x88 | (rnd() % 4));
            if ((b[0] & 3) == 3 && r == R_SP) r = R_AX;
            b[n++] = rand_mem_modrm(b[0] & 2 ? r : rnd() % 8, &disp, &dlen);
            if (dlen) b[n++] = disp;
            break;
        default:                                          // lea
            b[n++] = 0x8D;
            b[n++] = rand_mem_modrm(r, &disp, &dlen);
            if (dlen) b[n++] = disp;
            break;
        }
        break;
    }
    case 7: {                                             // xchg, cbw, cwd, flags ops
        static const uint8_t ones[] = { 0x98, 0x99, 0x9E, 0x9F, 0xF5, 0xF8, 0xF9, 0xFC, 0xFD, 0xD6,
                                        0x91, 0x92, 0x93, 0x95, 0x96, 0x97 };
        b[n++] = ones[rnd() % sizeof ones];
        break;
    }
    case 8: {                                             // BCD
        static const uint8_t bcd[] = { 0x27, 0x2F, 0x37, 0x3F, 0xD4, 0xD5 };
        uint8_t op = bcd[rnd() % 6];
        b[n++] = op;
        if (op == 0xD4 || op == 0xD5) b[n++] = (uint8_t)(rnd() % 2 ? 10 : 1 + rnd() % 255);
        *undef = op == 0x27 || op == 0x2F ? F_OF
               : op == 0x37 || op == 0x3F ? (F_OF | F_SF | F_ZF | F_PF)
               : (F_OF | F_AF | F_CF);
        break;
    }
    case 9: {                                             // string ops
        static const uint8_t s[] = { 0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };
        unsigned pre = rnd() % 3;
        if (pre) b[n++] = pre == 1 ? 0xF3 : 0xF2;
        b[n++] = s[rnd() % sizeof s];
        in->r[R_CX] = (uint16_t)(rnd() % 40);
        break;
    }
    case 10: {                                            // push/pop pairs
        unsigned a = rand_reg_not_sp(), z = rand_reg_not_sp();
        switch (rnd() % 4) {
        case 0: b[n++] = (uint8_t)(0x50 | a); b[n++] = (uint8_t)(0x58 | z); break;
        case 1: b[n++] = 0x6A; b[n++] = (uint8_t)rnd(); b[n++] = (uint8_t)(0x58 | z); break;
        case 2: b[n++] = 0x68; b[n++] = (uint8_t)rnd(); b[n++] = (uint8_t)rnd(); b[n++] = (uint8_t)(0x58 | z); break;
        default: b[n++] = 0x60; b[n++] = 0x61; break;
        }
        break;
    }
    case 11: {                                            // conditional jumps over `inc ax`
        uint8_t op = rnd() % 4 ? (uint8_t)(0x70 | (rnd() % 16)) : (uint8_t)(0xE0 + rnd() % 4);
        b[n++] = op;
        b[n++] = 1;
        b[n++] = 0x40;
        break;
    }
    case 12: {                                            // imul r, rm, imm
        unsigned r = rand_reg_not_sp();
        int big = rnd() & 1;
        b[n++] = big ? 0x69 : 0x6B;
        b[n++] = (uint8_t)(0xC0 | r << 3 | (rnd() % 8));
        b[n++] = (uint8_t)rnd();
        if (big) b[n++] = (uint8_t)rnd();
        *undef = F_SF | F_ZF | F_AF | F_PF;
        break;
    }
    case 13: {                                            // xlat, test r/m, xchg r/m
        switch (rnd() % 3) {
        case 0: b[n++] = 0xD7; break;
        case 1:
            b[n++] = (uint8_t)(0x84 | w);
            b[n++] = (uint8_t)(0xC0 | (rnd() % 64));
            *undef = U_AF;
            break;
        default: {
            unsigned a = rand_reg_not_sp(), z = rand_reg_not_sp();
            b[n++] = (uint8_t)(0x86 | w);
            b[n++] = (uint8_t)(0xC0 | a << 3 | z);
            break;
        }
        }
        break;
    }
    case 14: {                                            // enter/leave
        b[n++] = 0xC8;
        b[n++] = (uint8_t)(rnd() % 64);
        b[n++] = 0;
        b[n++] = (uint8_t)(rnd() % 3);
        b[n++] = 0xC9;
        break;
    }
    default: {                                            // group 1 on memory, inc/dec memory
        if (rnd() & 1) {
            b[n++] = (uint8_t)(0x80 | (rnd() & 1));
            b[n++] = rand_mem_modrm(rnd() % 8, &disp, &dlen);
            if (dlen) b[n++] = disp;
            b[n++] = (uint8_t)rnd();
            if (b[0] & 1) b[n++] = (uint8_t)rnd();
            unsigned op = (b[1] >> 3) & 7;
            if (op == 1 || op == 4 || op == 6) *undef = U_AF;
        } else {
            b[n++] = (uint8_t)(0xFE | w);
            b[n++] = rand_mem_modrm(rnd() % 2, &disp, &dlen);
            if (dlen) b[n++] = disp;
        }
        break;
    }
    }
    return n;
}

// --- the x87 cases ---------------------------------------------------------

// One ESC instruction, sometimes behind a segment override, sometimes
// followed by FNSTSW AX (which does not wait, so a pending exception is
// read, not raised).
static unsigned gen_x87(uint8_t *b) {
    unsigned n = 0, dlen;
    uint8_t disp = 0;
    if (rnd() % 8 == 0) b[n++] = rnd() & 1 ? 0x26 : 0x36;
    b[n++] = (uint8_t)(0xD8 + rnd() % 8);
    if (rnd() & 1) b[n++] = (uint8_t)(0xC0 + rnd() % 64);
    else {
        b[n++] = rand_mem_modrm(rnd() % 8, &disp, &dlen);
        if (dlen) b[n++] = disp;
    }
    if (rnd() % 4 == 0) { b[n++] = 0xDF; b[n++] = 0xE0; }
    return n;
}

// An 80-bit value of a random class: ordinary numbers near 1, the edges
// of double's and float's range (so stores overflow and underflow),
// zeros, denormals, infinities, NaNs and a few unsupported encodings.
static void rand_ext(uint8_t *p) {
    uint64_t m = (uint64_t)rnd() << 32 | rnd();
    unsigned e, cls = rnd() % 10;
    switch (cls) {
    case 0: e = 0; m = 0; break;
    case 1: e = 0; m &= ~(1ull << 63); break;                      // denormal
    case 2: e = 0x7FFF; m = 1ull << 63; break;                     // infinity
    case 3: e = 0x7FFF; m |= 1ull << 63; break;                    // NaN
    case 4: e = 0x43FE + rnd() % 4; m |= 1ull << 63; break;        // past double
    case 5: e = 0x3BFF - rnd() % 64; m |= 1ull << 63; break;       // under double
    case 6: e = 0x407E + rnd() % 4; m |= 1ull << 63; break;        // past float
    case 7: e = rnd() & 0x7FFF; break;                             // anything
    default: e = 0x3FFF - 32 + rnd() % 64; m |= 1ull << 63; break;
    }
    if (rnd() & 1) e |= 0x8000;
    memcpy(p, &m, 8);
    p[8] = (uint8_t)e;
    p[9] = (uint8_t)(e >> 8);
}

// FNSAVE's image: random registers, tags, TOP, condition codes and control
// word; the sticky flags only ever masked ones, as a state that has not
// trapped yet is.
static void rand_x87(uint8_t *img) {
    memset(img, 0, 108);
    uint16_t cw = (uint16_t)((rnd() & 0x1F3F) | 0x0040);
    if ((cw & 0x0300) == 0x0100) cw ^= 0x0100;                     // PC 01 is reserved
    if (rnd() % 3) cw |= 0x3F;                                     // mostly masked
    uint16_t sw = (uint16_t)(rnd() & 0x4700 & 0xFFFF) | (uint16_t)((rnd() % 8) << 11);
    sw |= (uint16_t)(rnd() & cw & 0x3F);
    if (sw & 0x01) sw |= rnd() & 0x40;
    uint16_t tw = 0;
    for (int i = 0; i < 8; i++) tw |= (uint16_t)((rnd() % 4 == 0 ? 3 : 0) << (2 * i));
    memcpy(img + 0, &cw, 2);
    memcpy(img + 4, &sw, 2);
    memcpy(img + 8, &tw, 2);
    for (int i = 0; i < 8; i++) rand_ext(img + 28 + 10 * i);
}

static uint16_t w16at(const uint8_t *p, unsigned at) { return (uint16_t)(p[at] | p[at + 1] << 8); }

// The two FNSAVE images: control, status and tag words, the registers,
// and the last instruction's and operand's pointers (the native test ran
// `at` bytes into its code segment).
static int x87_differs(const uint8_t *cpu, const uint8_t *w16, unsigned at, unsigned len) {
    if (w16at(cpu, 0) != w16at(w16, 0) || w16at(cpu, 4) != w16at(w16, 4) || w16at(cpu, 8) != w16at(w16, 8))
        return 1;
    if (memcmp(cpu + 28, w16 + 28, 80)) return 1;
    uint16_t ip = w16at(cpu, 12);
    int in_test = w16at(w16, 16) == SEL(E_CODE) && w16at(w16, 12) < len;   // else a loaded value
    if (ip != (in_test ? (uint16_t)(w16at(w16, 12) + at) : w16at(w16, 12))) return 1;
    return w16at(cpu, 16) != w16at(w16, 16) || (w16at(cpu, 18) & 0x7FF) != w16at(w16, 18) ||
           w16at(cpu, 20) != w16at(w16, 20) || w16at(cpu, 24) != w16at(w16, 24);
}

static void x87_print(const uint8_t *p) {
    printf("cw %04x sw %04x tw %04x ip %04x cs %04x op %03x dp %04x ds %04x\n          ", w16at(p, 0),
           w16at(p, 4), w16at(p, 8), w16at(p, 12), w16at(p, 16), w16at(p, 18) & 0x7FF, w16at(p, 20), w16at(p, 24));
    for (int i = 0; i < 8; i++) {
        for (int j = 9; j >= 0; j--) printf("%02x", p[28 + 10 * i + j]);
        printf(i == 3 ? "\n          " : " ");
    }
    printf("\n");
}

static void dis(const uint8_t *b, unsigned n, char *out) {
    out[0] = 0;
    for (unsigned i = 0; i < n; i++) sprintf(out + 3 * i, "%02X ", b[i]);
}

// `fuzz N`: random BYTES as code, random registers and selectors, a
// thunk that pops random amounts -- run under ASan/UBSan by the wrapper.
// The interpreter must fault or finish; never touch memory outside the
// segments' buffers.
static int fuzz_thunk(struct cpu16 *c, uint16_t sel, uint16_t off) {
    (void)c; (void)sel;
    return off & 1 ? -1 : (int)(off % 40);
}

static int fuzz_int(struct cpu16 *c, uint8_t n) {
    if (n == 0x21 && (c->r[R_AX] >> 8) == 0x4C) { c->exited = 1; return 0; }
    return n & 1 ? 0 : -1;
}

static int fuzz(unsigned cases) {
    static uint8_t code[0x400], data[0x300], stack[0x200];
    unsigned faults = 0, exits = 0, budget = 0;
    for (unsigned k = 0; k < cases; k++) {
        struct cpu16 c;
        memset(&c, 0, sizeof c);
        memset(g_desc, 0, sizeof g_desc);
        c.desc = g_desc;
        c.thunk = fuzz_thunk;
        c.intr = fuzz_int;
        for (unsigned i = 0; i < sizeof code; i++) code[i] = (uint8_t)rnd();
        for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)rnd();
        // Small segments with odd limits, so the limit checks are what holds.
        g_desc[1] = (struct cpu16_desc){ code, sizeof code - 1, D_CODE, 1 };
        g_desc[2] = (struct cpu16_desc){ data, sizeof data - 1 - (rnd() % 64), D_DATA, 2 };
        g_desc[3] = (struct cpu16_desc){ stack, sizeof stack - 1, D_DATA, 3 };
        g_desc[4] = (struct cpu16_desc){ 0, 0xFFFF, D_THUNK, 0 };
        g_desc[5] = (struct cpu16_desc){ code, sizeof code - 1, D_CODE, 1 };
        for (int i = 0; i < 8; i++) c.r[i] = (uint16_t)rnd();
        c.r[R_SP] = (uint16_t)(rnd() % 0x220);
        c.sreg[S_CS] = SEL(1);
        c.sreg[S_DS] = SEL(rnd() % 6);
        c.sreg[S_ES] = SEL(rnd() % 6);
        c.sreg[S_SS] = SEL(3);
        c.ip = (uint16_t)(rnd() % sizeof code);
        c.flags = (uint16_t)(rnd() & 0x0FD5);
        cpu16_run(&c, 2000);
        if (c.exited) exits++;
        else if (c.fault == FAULT_LIMIT) budget++;
        else faults++;
    }
    printf("fuzz %u: %u faulted, %u exited, %u ran out of steps\n", cases, faults, exits, budget);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "fuzz")) {
        g_rng = argc > 3 ? (uint32_t)atoi(argv[3]) * 2654435761u : 99;
        return fuzz(argc > 2 ? (unsigned)atoi(argv[2]) : 20000);
    }
    unsigned cases = argc > 1 ? (unsigned)atoi(argv[1]) : 100000;
    g_rng = (argc > 2 ? (uint32_t)atoi(argv[2]) : 1) * 2654435761u ^ 0x9E3779B9u;
    if (!g_rng) g_rng = 1;
    g_code = low_map(0x10000);
    g_data = low_map(0x10000);
    g_stack = low_map(0x10000);
    g_lowstack = low_map(0x10000);
    set_ldt(E_CODE, g_code, 1);
    set_ldt(E_DATA, g_data, 0);
    set_ldt(E_STACK, g_stack, 0);

    unsigned bad = 0, skipped = 0, ran = 0;
    for (unsigned k = 0; k < cases && bad < 20; k++) {
        struct regs in;
        for (int i = 0; i < 8; i++) in.r[i] = (uint16_t)(rnd() % 0x7000);
        in.r[R_AX] = (uint16_t)rnd();
        in.r[R_DX] = (uint16_t)rnd();
        in.r[R_CX] = (uint16_t)rnd();
        in.r[R_SP] = TEST_SP;
        in.flags = (uint16_t)((rnd() & (F_CF | F_PF | F_AF | F_ZF | F_SF | F_DF | F_OF)) | F_IF | 2);
        uint8_t test[16];
        uint16_t undef = 0;
        int fp = rnd() % 4 == 0;
        uint8_t fin[108], fout[108], fw16[108];
        unsigned len = fp ? gen_x87(test) : gen(test, &undef, &in);
        if (fp) rand_x87(fin);
        else {
            __asm__ volatile("fninit\n\tfnsave %0" : "=m"(fin) : : "memory");
        }
        if (test[0] == 0xF2 || test[0] == 0xF3 || (test[0] >= 0xA4 && test[0] <= 0xAF)) {
            in.r[R_SI] = (uint16_t)(0x200 + rnd() % 0x7000);
            in.r[R_DI] = (uint16_t)(0x200 + rnd() % 0x7000);
        }
        for (unsigned i = 0; i < 0x10000; i += 4) {
            uint32_t v = rnd();
            memcpy(g_idata + i, &v, 4);
        }
        memcpy(g_data, g_idata, 0x10000);
        memset(g_istack, 0, sizeof g_istack);
        for (unsigned i = 0; i < CMP_STACK; i++) g_istack[i] = (uint8_t)(i * 7);
        memcpy(g_stack, g_istack, 0x10000);

        // The interpreter.
        struct cpu16 c;
        memset(&c, 0, sizeof c);
        memset(g_desc, 0, sizeof g_desc);
        c.desc = g_desc;
        memset(g_icode, 0x90, sizeof g_icode);
        memcpy(g_icode, test, len);
        g_desc[E_CODE] = (struct cpu16_desc){ g_icode, 0xFFFF, D_CODE, 1 };
        g_desc[E_DATA] = (struct cpu16_desc){ g_idata, 0xFFFF, D_DATA, 2 };
        g_desc[E_STACK] = (struct cpu16_desc){ g_istack, 0xFFFF, D_DATA, 3 };
        memcpy(c.r, in.r, sizeof c.r);
        c.flags = in.flags;
        c.sreg[S_CS] = SEL(E_CODE);
        c.sreg[S_DS] = c.sreg[S_ES] = SEL(E_DATA);
        c.sreg[S_SS] = SEL(E_STACK);
        c.ip = 0;
        cpu16_x87_load(&c, fin);
        for (int s = 0; s < 64 && c.ip < len && !c.fault; s++) cpu16_step(&c);
        if (c.fault) {           // divide errors and the like: not run natively
            skipped++;
            continue;
        }

        // The CPU.
        memcpy(g_stack + IN_BLOCK, in.r, 16);
        memcpy(g_stack + IN_BLOCK + 0x10, &in.flags, 2);
        unsigned at = build_native(test, len);
        run_native(fin, fout);
        cpu16_x87_save(&c, fw16);
        struct regs out;
        memcpy(out.r, g_stack + OUT_BLOCK, 16);
        memcpy(&out.flags, g_stack + OUT_BLOCK + 0x10, 2);
        ran++;

        // The epilogue's pushf wrote the flags just below the final SP, where
        // the interpreter (which runs no epilogue) left the bytes alone.
        uint16_t below = (uint16_t)(out.r[R_SP] - 2);
        if (below < CMP_STACK - 1) memcpy(g_stack + below, g_istack + below, 2);
        uint16_t fmask = (uint16_t)((F_CF | F_PF | F_AF | F_ZF | F_SF | F_DF | F_OF) & ~undef);
        int fdiff = fp && x87_differs(fout, fw16, at, len);
        int diff = memcmp(out.r, c.r, sizeof c.r) || ((out.flags ^ c.flags) & fmask)
                || memcmp(g_data, g_idata, 0x10000) || memcmp(g_stack, g_istack, CMP_STACK) || fdiff;
        if (diff) {
            char d[64];
            dis(test, len, d);
            printf("FAIL %s\n  in   ", d);
            for (int i = 0; i < 8; i++) printf("%04x ", in.r[i]);
            printf("fl %04x\n  cpu  ", in.flags);
            for (int i = 0; i < 8; i++) printf("%04x ", out.r[i]);
            printf("fl %04x\n  w16  ", out.flags & fmask);
            for (int i = 0; i < 8; i++) printf("%04x ", c.r[i]);
            printf("fl %04x%s%s\n", c.flags & fmask,
                   memcmp(g_data, g_idata, 0x10000) ? "  (data differs)" : "",
                   memcmp(g_stack, g_istack, CMP_STACK) ? "  (stack differs)" : "");
            if (fp) {
                printf("  fpu in   ");
                x87_print(fin);
                printf("  fpu cpu  ");
                x87_print(fout);
                printf("  fpu w16  ");
                x87_print(fw16);
            }
            bad++;
        }
    }
    printf("cases %u ran %u skipped %u failed %u\n", ran + skipped, ran, skipped, bad);
    return bad ? 1 : 0;
}
