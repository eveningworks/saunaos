// cpu16_check.c -- the driver of tools/hostchecks/cpu16_hostcheck.py: w16's 16-bit
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
#include <cpuid.h>
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

#define IN_BLOCK  0xFF00     // in the stack segment: EAX..EDI and the flags in
#define OUT_BLOCK 0xFF40     // ... and out
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

// The 16-bit code: save the host's SS:ESP, switch to our stack (ESP's
// high word 0), set DS=ES=GS=data, load the 32-bit registers and the
// flags, run the test, store them, restore the host's stack and `o32
// retf` back to 64-bit mode.
static unsigned build_native(const uint8_t *test, unsigned len) {
    uint8_t *p = g_code;
    unsigned a = 0;
    const uint8_t pro[] = {
        0xB8, SEL(E_STACK) & 0xFF, SEL(E_STACK) >> 8,     // mov ax, stack
        0x8E, 0xD8,                                       // mov ds, ax
        0x66, 0x89, 0x26, SAVE_AREA & 0xFF, SAVE_AREA >> 8,   // mov [save], esp
        0x8C, 0x16, (SAVE_AREA + 4) & 0xFF, (SAVE_AREA + 4) >> 8,   // mov [save+4], ss
        0x8E, 0xD0,                                       // mov ss, ax
        0x66, 0xBC, TEST_SP & 0xFF, TEST_SP >> 8, 0, 0,   // mov esp, TEST_SP
        0x68, SEL(E_DATA) & 0xFF, SEL(E_DATA) >> 8, 0x1F, // push data; pop ds
        0x68, SEL(E_DATA) & 0xFF, SEL(E_DATA) >> 8, 0x07, // push data; pop es
        0x68, SEL(E_DATA) & 0xFF, SEL(E_DATA) >> 8, 0x0F, 0xA9,   // push data; pop gs
        0x36, 0xFF, 0x36, 0x20, 0xFF,                     // push word [ss:IN+0x20]
        0x9D,                                             // popf
        0x66, 0x36, 0xA1, 0x00, 0xFF,                     // mov eax, [ss:IN]
        0x66, 0x36, 0x8B, 0x0E, 0x04, 0xFF,               // mov ecx, [ss:IN+4]
        0x66, 0x36, 0x8B, 0x16, 0x08, 0xFF,               // mov edx
        0x66, 0x36, 0x8B, 0x1E, 0x0C, 0xFF,               // mov ebx
        0x66, 0x36, 0x8B, 0x2E, 0x14, 0xFF,               // mov ebp
        0x66, 0x36, 0x8B, 0x36, 0x18, 0xFF,               // mov esi
        0x66, 0x36, 0x8B, 0x3E, 0x1C, 0xFF,               // mov edi
    };
    a = emit(p, a, pro, sizeof pro);
    unsigned at = a;
    a = emit(p, a, test, len);
    const uint8_t epi[] = {
        0x66, 0x36, 0xA3, 0x40, 0xFF,                     // mov [ss:OUT], eax
        0x66, 0x36, 0x89, 0x0E, 0x44, 0xFF,               // ecx
        0x66, 0x36, 0x89, 0x16, 0x48, 0xFF,               // edx
        0x66, 0x36, 0x89, 0x1E, 0x4C, 0xFF,               // ebx
        0x66, 0x36, 0x89, 0x26, 0x50, 0xFF,               // esp
        0x66, 0x36, 0x89, 0x2E, 0x54, 0xFF,               // ebp
        0x66, 0x36, 0x89, 0x36, 0x58, 0xFF,               // esi
        0x66, 0x36, 0x89, 0x3E, 0x5C, 0xFF,               // edi
        0x9C,                                             // pushf
        0x36, 0x8F, 0x06, 0x60, 0xFF,                     // pop word [ss:OUT+0x20]
        0x36, 0x66, 0x0F, 0xB2, 0x26, SAVE_AREA & 0xFF, SAVE_AREA >> 8,   // lss esp, [ss:save]
        0x66, 0xCB,                                       // o32 retf
    };
    emit(p, a, epi, sizeof epi);
    return at;
}

// The FPU state goes in by FRSTOR before the far call and comes out by
// FNSAVE after it, neither of which waits on what the test left pending.
// NO REGISTER SURVIVES COMPATIBILITY MODE: R8-R15 and the high halves of
// the rest are undefined after it (an Intel host loses them), so what the
// code after the far call needs comes from memory -- and a test may leave
// DF set, which glibc's `rep stosb` memset then runs backwards through.
static void run_native(const uint8_t *fin, uint8_t *fout) {
    static struct { uint32_t off; uint16_t sel; } __attribute__((packed)) fp;
    static uint8_t *out;
    fp.off = 0;
    fp.sel = SEL(E_CODE);
    out = fout;
    uint64_t top = (uint64_t)(uintptr_t)(g_lowstack + 65536 - 64);
    __asm__ volatile(
        "push %%rbx\n push %%rbp\n push %%r12\n push %%r13\n push %%r14\n push %%r15\n"
        "mov %%rsp, %[saved]\n"
        "mov %[top], %%rsp\n"
        "frstor (%[fin])\n"
        "lcall *%[fp]\n"
        "cld\n"
        "mov %[out], %%rax\n"
        "fnsave (%%rax)\n"
        "mov %[saved], %%rsp\n"
        "pop %%r15\n pop %%r14\n pop %%r13\n pop %%r12\n pop %%rbp\n pop %%rbx\n"
        : [saved] "+m"(g_saved_rsp)
        : [top] "r"(top), [fp] "m"(fp), [fin] "r"(fin), [out] "m"(out)
        : "memory", "cc", "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
}

// --- the cases ---------------------------------------------------------------

struct regs { uint16_t r[8], rh[8], flags; };

#define U_AF F_AF
#define U_ALL (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

// The case's prefixes: 66 (a dword where the instruction says a word) and
// 67 (a 32-bit address in a memory operand), set by gen() for the body.
static int g_o32, g_a32;

static unsigned rand_reg_not_sp(void) {
    unsigned r;
    do r = rnd() % 8; while (r == R_SP);
    return r;
}

// A word immediate -- a dword under 66.
static unsigned imm_word(uint8_t *b, unsigned n) {
    for (unsigned i = 0; i < (g_o32 ? 4u : 2u); i++) b[n++] = (uint8_t)rnd();
    return n;
}

// A memory operand that stays inside the segments, ModRM and what follows
// it: [SI], [DI], [BX], [BX+SI+d8], [BP+DI] -- the registers are kept small
// by the caller -- or under 67 [ESI], [EDI], [EBX+d8], [disp32], and SIB
// forms with a scale ([EBX+ESI], [ESI+EDI*2+d8], [EBP+ESI] on the stack,
// [ESI*4+disp32]); their registers' high words are zero.
static unsigned mem_modrm(uint8_t *b, unsigned n, unsigned reg) {
    if (!g_a32) {
        static const uint8_t forms[][2] = { {0, 4}, {0, 5}, {0, 7}, {1, 0}, {0, 3}, {1, 6} };
        unsigned f = rnd() % 6;
        b[n++] = (uint8_t)(forms[f][0] << 6 | reg << 3 | forms[f][1]);
        if (forms[f][0]) b[n++] = (uint8_t)(rnd() % 0x80);
        return n;
    }
    switch (rnd() % 7) {
    case 0: b[n++] = (uint8_t)(reg << 3 | 6); break;                          // [esi]
    case 1: b[n++] = (uint8_t)(reg << 3 | 7); break;                          // [edi]
    case 2: b[n++] = (uint8_t)(0x40 | reg << 3 | 3); b[n++] = (uint8_t)(rnd() % 0x80); break;   // [ebx+d8]
    case 3: {                                                                 // [disp32]
        uint32_t d = rnd() % 0xFF00;
        b[n++] = (uint8_t)(reg << 3 | 5);
        for (int i = 0; i < 4; i++) b[n++] = (uint8_t)(d >> 8 * i);
        break;
    }
    case 4: b[n++] = (uint8_t)(reg << 3 | 4); b[n++] = 0x33; break;           // [ebx+esi]
    case 5:                                                                   // [esi+edi*2+d8]
        b[n++] = (uint8_t)(0x40 | reg << 3 | 4); b[n++] = (uint8_t)(1 << 6 | 7 << 3 | 6);
        b[n++] = (uint8_t)(rnd() % 0x80);
        break;
    default:                                                                  // [ebp+esi], SS
        b[n++] = (uint8_t)(0x40 | reg << 3 | 4); b[n++] = (uint8_t)(0 << 6 | 6 << 3 | 5); b[n++] = 0;
        break;
    }
    return n;
}

// The 386's 0F instructions; returns the body's length.
static unsigned gen_0f(uint8_t *b, uint16_t *undef, struct regs *in) {
    unsigned n = 0;
    unsigned r = rand_reg_not_sp();
    int w32 = g_o32;
    // dispatch-ok: one case per 0F family the oracle covers, bounded by the ISA.
    switch (rnd() % 9) {
    case 0:                                               // movzx, movsx
        b[n++] = 0x0F;
        b[n++] = (uint8_t)((rnd() & 1 ? 0xB6 : 0xBE) | (rnd() & 1));
        if (rnd() & 1) b[n++] = (uint8_t)(0xC0 | r << 3 | (rnd() % 8));
        else n = mem_modrm(b, n, r);
        break;
    case 1:                                               // setcc
        b[n++] = 0x0F;
        b[n++] = (uint8_t)(0x90 | (rnd() % 16));
        if (rnd() & 1) b[n++] = (uint8_t)(0xC0 | (rnd() % 8));
        else n = mem_modrm(b, n, 0);
        break;
    case 2: {                                             // bt bts btr btc r/m, r
        static const uint8_t ops[] = { 0xA3, 0xAB, 0xB3, 0xBB };
        b[n++] = 0x0F;
        b[n++] = ops[rnd() % 4];
        if (rnd() % 4) b[n++] = (uint8_t)(0xC0 | (rnd() % 8) << 3 | r);
        else {                                            // the bit offset in BX, SI or DI: small, positive
            static const uint8_t offs[] = { R_BX, R_SI, R_DI };
            unsigned o = offs[rnd() % 3];
            in->rh[o] = 0;
            n = mem_modrm(b, n, o);
        }
        *undef = F_OF | F_SF | F_AF | F_PF | F_ZF;
        break;
    }
    case 3:                                               // bt bts btr btc r/m, imm8
        b[n++] = 0x0F;
        b[n++] = 0xBA;
        if (rnd() & 1) b[n++] = (uint8_t)(0xC0 | (4 + rnd() % 4) << 3 | r);
        else n = mem_modrm(b, n, 4 + rnd() % 4);
        b[n++] = (uint8_t)rnd();
        *undef = F_OF | F_SF | F_AF | F_PF | F_ZF;
        break;
    case 4: {                                             // shld, shrd: by imm8 or CL, a word no further than 16
        int cl = rnd() & 1;
        unsigned count = rnd() % (w32 ? 32 : 17);
        b[n++] = 0x0F;
        b[n++] = (uint8_t)((rnd() & 1 ? 0xA4 : 0xAC) | cl);
        unsigned rm = rand_reg_not_sp();
        if (cl && rm == R_CX) rm = R_DX;
        b[n++] = (uint8_t)(0xC0 | (rnd() % 8) << 3 | rm);
        if (cl) in->r[R_CX] = (uint16_t)((in->r[R_CX] & 0xFF00) | count);
        else b[n++] = (uint8_t)count;
        *undef = U_AF;
        if (count != 1) *undef |= F_OF;
        break;
    }
    case 5:                                               // bsf, bsr; a zero source sometimes
        b[n++] = 0x0F;
        b[n++] = (uint8_t)(0xBC | (rnd() & 1));
        {
            unsigned src = rand_reg_not_sp();
            if (rnd() % 4 == 0) { in->r[src] = 0; in->rh[src] = 0; }
            b[n++] = (uint8_t)(0xC0 | r << 3 | src);
        }
        *undef = F_CF | F_OF | F_SF | F_AF | F_PF;
        break;
    case 6:                                               // imul r, r/m
        b[n++] = 0x0F;
        b[n++] = 0xAF;
        if (rnd() & 1) b[n++] = (uint8_t)(0xC0 | r << 3 | (rnd() % 8));
        else n = mem_modrm(b, n, r);
        *undef = F_SF | F_ZF | F_AF | F_PF;
        break;
    case 7:                                               // jcc near over `inc ax`
        b[n++] = 0x0F;
        b[n++] = (uint8_t)(0x80 | (rnd() % 16));
        b[n++] = 1;
        b[n++] = 0;
        if (w32) { b[n++] = 0; b[n++] = 0; }
        b[n++] = 0x40;
        break;
    default:                                              // push gs, pop it into a register
        b[n++] = 0x0F;
        b[n++] = 0xA8;
        b[n++] = (uint8_t)(0x58 | r);
        break;
    }
    return n;
}

// One test instruction's body (after its prefixes) into b; returns its
// length and the flags it leaves undefined in *undef.
static unsigned gen_body(uint8_t *b, uint16_t *undef, struct regs *in, unsigned kind) {
    unsigned n = 0;
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
            n = mem_modrm(b, n, reg);
        }
        if ((op >> 3) == 1 || (op >> 3) == 4 || (op >> 3) == 6) *undef = U_AF;
        break;
    }
    case 2: {                                             // ALU acc,imm / group 1
        unsigned op = (rnd() % 8);
        if (rnd() & 1) {
            b[n++] = (uint8_t)(op << 3 | 4 | w);
            if (w) n = imm_word(b, n);
            else b[n++] = (uint8_t)rnd();
        } else {
            uint8_t opc = (uint8_t)(0x80 | (rnd() % 4));
            if (opc == 0x82) opc = 0x80;
            unsigned rm = rnd() % 8;
            if ((opc & 1) && rm == R_SP) rm = R_DX;
            b[n++] = opc;
            b[n++] = (uint8_t)(0xC0 | op << 3 | rm);
            if (opc == 0x81) n = imm_word(b, n);
            else b[n++] = (uint8_t)rnd();
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
        unsigned bits = w ? (g_o32 ? 32 : 16) : 8;
        if ((op == 4 || op == 5 || op == 6) && count >= bits) *undef |= F_CF;
        if (op < 4 && count == 0) *undef = 0;
        break;
    }
    case 5: {                                             // group 3
        unsigned op = rnd() % 8;
        unsigned rm = rand_reg_not_sp();
        b[n++] = (uint8_t)(0xF6 | w);
        b[n++] = (uint8_t)(0xC0 | op << 3 | rm);
        if (op <= 1) {
            if (w) n = imm_word(b, n);
            else b[n++] = (uint8_t)rnd();
            *undef = U_AF;
        }
        if (op == 4 || op == 5) *undef = F_SF | F_ZF | F_AF | F_PF;
        if (op >= 6) *undef = U_ALL;
        break;
    }
    case 6: {                                             // mov forms
        unsigned r = rand_reg_not_sp();
        switch (rnd() % 4) {
        case 0: b[n++] = (uint8_t)(0xB8 | r); n = imm_word(b, n); break;
        case 1: b[n++] = (uint8_t)(0xB0 | (rnd() % 8)); b[n++] = (uint8_t)rnd(); break;
        case 2: {
            uint8_t op = (uint8_t)(0x88 | (rnd() % 4));
            b[n++] = op;
            if ((op & 3) == 3 && r == R_SP) r = R_AX;
            n = mem_modrm(b, n, op & 2 ? r : rnd() % 8);
            break;
        }
        default:                                          // lea
            b[n++] = 0x8D;
            n = mem_modrm(b, n, r);
            break;
        }
        break;
    }
    case 7: {                                             // xchg, cbw/cwde, cwd/cdq, flags ops
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
    case 9: {                                             // string ops: SI and DI clear of either end
        static const uint8_t s[] = { 0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };
        unsigned pre = rnd() % 3;
        if (pre) b[n++] = pre == 1 ? 0xF3 : 0xF2;
        b[n++] = s[rnd() % sizeof s];
        in->r[R_CX] = (uint16_t)(rnd() % 40);
        in->r[R_SI] = (uint16_t)(0x200 + rnd() % 0x7000);
        in->r[R_DI] = (uint16_t)(0x200 + rnd() % 0x7000);
        in->rh[R_CX] = in->rh[R_SI] = in->rh[R_DI] = 0;
        break;
    }
    case 10: {                                            // push/pop pairs
        unsigned a = rand_reg_not_sp(), z = rand_reg_not_sp();
        switch (rnd() % 4) {
        case 0: b[n++] = (uint8_t)(0x50 | a); b[n++] = (uint8_t)(0x58 | z); break;
        case 1: b[n++] = 0x6A; b[n++] = (uint8_t)rnd(); b[n++] = (uint8_t)(0x58 | z); break;
        case 2: b[n++] = 0x68; n = imm_word(b, n); b[n++] = (uint8_t)(0x58 | z); break;
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
        if (big) n = imm_word(b, n);
        else b[n++] = (uint8_t)rnd();
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
    case 15: {                                            // group 1 on memory, inc/dec memory
        if (rnd() & 1) {
            uint8_t op = (uint8_t)(0x80 | (rnd() & 1));
            unsigned reg = rnd() % 8;
            b[n++] = op;
            n = mem_modrm(b, n, reg);
            if (op & 1) n = imm_word(b, n);
            else b[n++] = (uint8_t)rnd();
            if (reg == 1 || reg == 4 || reg == 6) *undef = U_AF;
        } else {
            b[n++] = (uint8_t)(0xFE | w);
            n = mem_modrm(b, n, rnd() % 2);
        }
        break;
    }
    default:
        n = gen_0f(b, undef, in);
        break;
    }
    return n;
}

// One test instruction: its prefixes -- sometimes 66, 67 or a GS
// override -- then the body.
static unsigned gen(uint8_t *b, uint16_t *undef, struct regs *in) {
    unsigned kind = rnd() % 20, n = 0;
    *undef = 0;
    g_o32 = kind != 8 && rnd() % 3 == 0;                  // BCD has no operand size
    g_a32 = rnd() % 4 == 0;
    if (g_a32) {                                          // what a 32-bit address may be made of
        in->rh[R_BX] = in->rh[R_SI] = in->rh[R_DI] = in->rh[R_BP] = 0;
        if (kind == 9 || kind == 11) in->rh[R_CX] = (uint16_t)(rnd() % 2);   // ECX counts under 67
    }
    if (g_o32) b[n++] = 0x66;
    if (g_a32) b[n++] = 0x67;
    if (rnd() % 8 == 0) b[n++] = 0x65;
    return n + gen_body(b + n, undef, in, kind);
}

// --- the x87 cases ---------------------------------------------------------

// One ESC instruction, sometimes behind a segment override, sometimes
// followed by FNSTSW AX (which does not wait, so a pending exception is
// read, not raised).
static unsigned gen_x87(uint8_t *b) {
    unsigned n = 0;
    g_a32 = 0;
    if (rnd() % 8 == 0) b[n++] = rnd() & 1 ? 0x26 : 0x36;
    b[n++] = (uint8_t)(0xD8 + rnd() % 8);
    if (rnd() & 1) b[n++] = (uint8_t)(0xC0 + rnd() % 64);
    else n = mem_modrm(b, n, rnd() % 8);
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

// What a later Intel host's FNSAVE no longer stores, though a 387 did and
// w16 does: CPUID 7's EBX bit 13, FCS and FDS always 0; bit 6, FDP (and
// with it FOP) kept only for an unmasked exception.
static int g_no_fcs_fds, g_fdp_on_exception;

static void host_x87_quirks(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return;
    g_no_fcs_fds = (b >> 13) & 1;
    g_fdp_on_exception = (b >> 6) & 1;
}

// The two FNSAVE images: control, status and tag words, the registers,
// and the last instruction's and operand's pointers (the native test ran
// `at` bytes into its code segment) -- those the host stores.
static int x87_differs(const uint8_t *cpu, const uint8_t *w16, unsigned at, unsigned len) {
    if (w16at(cpu, 0) != w16at(w16, 0) || w16at(cpu, 4) != w16at(w16, 4) || w16at(cpu, 8) != w16at(w16, 8))
        return 1;
    if (memcmp(cpu + 28, w16 + 28, 80)) return 1;
    uint16_t ip = w16at(cpu, 12);
    int in_test = w16at(w16, 16) == SEL(E_CODE) && w16at(w16, 12) < len;   // else a loaded value
    if (ip != (in_test ? (uint16_t)(w16at(w16, 12) + at) : w16at(w16, 12))) return 1;
    if (!g_no_fcs_fds && (w16at(cpu, 16) != w16at(w16, 16) || w16at(cpu, 24) != w16at(w16, 24))) return 1;
    if (g_fdp_on_exception && !(w16at(cpu, 4) & 0x80)) return 0;
    return (w16at(cpu, 18) & 0x7FF) != w16at(w16, 18) || w16at(cpu, 20) != w16at(w16, 20);
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

// Where two images first differ, and eight bytes from there in each.
static void first_difference(const char *what, const uint8_t *cpu, const uint8_t *w16, unsigned n) {
    unsigned i = 0;
    while (i < n && cpu[i] == w16[i]) i++;
    if (i == n) return;
    printf("  %s differs at %04x: cpu", what, i);
    for (unsigned k = i; k < i + 8 && k < n; k++) printf(" %02x", cpu[k]);
    printf(", w16");
    for (unsigned k = i; k < i + 8 && k < n; k++) printf(" %02x", w16[k]);
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
        for (int i = 0; i < 8; i++) { c.r[i] = (uint16_t)rnd(); c.rh[i] = (uint16_t)rnd(); }
        c.r[R_SP] = (uint16_t)(rnd() % 0x220);
        c.sreg[S_CS] = SEL(1);
        c.sreg[S_DS] = SEL(rnd() % 6);
        c.sreg[S_ES] = SEL(rnd() % 6);
        c.sreg[S_SS] = SEL(3);
        c.sreg[S_FS] = SEL(rnd() % 6);
        c.sreg[S_GS] = SEL(rnd() % 6);
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
    host_x87_quirks();
    if (g_no_fcs_fds || g_fdp_on_exception)
        printf("host x87: FCS/FDS %s, FDP/FOP %s\n", g_no_fcs_fds ? "not stored, not compared" : "compared",
               g_fdp_on_exception ? "compared only after an unmasked exception" : "compared");
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
        for (int i = 0; i < 8; i++) {
            in.r[i] = (uint16_t)(rnd() % 0x7000);
            in.rh[i] = (uint16_t)rnd();
        }
        in.r[R_AX] = (uint16_t)rnd();
        in.r[R_DX] = (uint16_t)rnd();
        in.r[R_CX] = (uint16_t)rnd();
        in.r[R_SP] = TEST_SP;
        in.rh[R_SP] = 0;
        in.flags = (uint16_t)((rnd() & (F_CF | F_PF | F_AF | F_ZF | F_SF | F_DF | F_OF)) | F_IF | 2);
        uint8_t test[24];
        uint16_t undef = 0;
        int fp = rnd() % 4 == 0;
        uint8_t fin[108], fout[108], fw16[108];
        unsigned len = fp ? gen_x87(test) : gen(test, &undef, &in);
        if (fp) rand_x87(fin);
        else {
            __asm__ volatile("fninit\n\tfnsave %0" : "=m"(fin) : : "memory");
        }
        for (unsigned i = 0; i < 0x10000; i += 4) {
            uint32_t v = rnd();
            memcpy(g_idata + i, &v, 4);
        }
        memcpy(g_data, g_idata, 0x10000);
        memset(g_istack, 0, sizeof g_istack);
        for (unsigned i = 0; i < CMP_STACK; i++) g_istack[i] = (uint8_t)(i * 7);
        memcpy(g_istack + TEST_SP - 2, &in.flags, 2);    // where the native prologue's popf took them from
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
        memcpy(c.rh, in.rh, sizeof c.rh);
        c.flags = in.flags;
        c.sreg[S_CS] = SEL(E_CODE);
        c.sreg[S_DS] = c.sreg[S_ES] = c.sreg[S_GS] = SEL(E_DATA);
        c.sreg[S_SS] = SEL(E_STACK);
        c.ip = 0;
        cpu16_x87_load(&c, fin);
        for (int s = 0; s < 64 && c.ip < len && !c.fault; s++) cpu16_step(&c);
        if (c.fault) {           // divide errors and the like: not run natively
            skipped++;
            continue;
        }

        // The CPU.
        for (int i = 0; i < 8; i++) {
            uint32_t e = (uint32_t)in.rh[i] << 16 | in.r[i];
            memcpy(g_stack + IN_BLOCK + 4 * i, &e, 4);
        }
        memcpy(g_stack + IN_BLOCK + 0x20, &in.flags, 2);
        unsigned at = build_native(test, len);
        run_native(fin, fout);
        cpu16_x87_save(&c, fw16);
        struct regs out;
        for (int i = 0; i < 8; i++) {
            uint32_t e;
            memcpy(&e, g_stack + OUT_BLOCK + 4 * i, 4);
            out.r[i] = (uint16_t)e;
            out.rh[i] = (uint16_t)(e >> 16);
        }
        memcpy(&out.flags, g_stack + OUT_BLOCK + 0x20, 2);
        ran++;

        // The epilogue's pushf wrote the flags just below the final SP, where
        // the interpreter (which runs no epilogue) left the bytes alone.
        uint16_t below = (uint16_t)(out.r[R_SP] - 2);
        if (below < CMP_STACK - 1) memcpy(g_stack + below, g_istack + below, 2);
        uint16_t fmask = (uint16_t)((F_CF | F_PF | F_AF | F_ZF | F_SF | F_DF | F_OF) & ~undef);
        int fdiff = fp && x87_differs(fout, fw16, at, len);
        int diff = memcmp(out.r, c.r, sizeof c.r) || memcmp(out.rh, c.rh, sizeof c.rh) || ((out.flags ^ c.flags) & fmask)
                || memcmp(g_data, g_idata, 0x10000) || memcmp(g_stack, g_istack, CMP_STACK) || fdiff;
        if (diff) {
            char d[64];
            dis(test, len, d);
            printf("FAIL %s\n  in   ", d);
            for (int i = 0; i < 8; i++) printf("%04x%04x ", in.rh[i], in.r[i]);
            printf("fl %04x\n  cpu  ", in.flags);
            for (int i = 0; i < 8; i++) printf("%04x%04x ", out.rh[i], out.r[i]);
            printf("fl %04x\n  w16  ", out.flags & fmask);
            for (int i = 0; i < 8; i++) printf("%04x%04x ", c.rh[i], c.r[i]);
            printf("fl %04x\n", c.flags & fmask);
            first_difference("data", g_data, g_idata, 0x10000);
            first_difference("stack", g_stack, g_istack, CMP_STACK);
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
