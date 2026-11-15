// jit16_x86.c -- the JIT's translator: one basic block of a Win386 program's 32-bit flat code,
// decoded and re-emitted as an x86-64 function `uint32_t block(struct cpu16 *c)` that runs it
// against the CPU's state and returns how many guest instructions it completed (jit16.h).
//
// THE GUEST IS x86 TOO, so most instructions are re-emitted AS THEMSELVES with their operands
// moved: a guest register is its slot in struct cpu16 ([rbx+n]), a guest memory operand is
// [base+rax] once its offset is computed and checked. The host instruction then sets exactly the
// flags the guest one would.
//
// FLAGS ARE LAZY -- the largest single saving DOSBox's and QEMU's translators get. A flag an
// instruction sets lives only in the HOST's flags (`pend`) until something needs c->flags: a
// check that clobbers the host's (every memory access, which may leave the block), a reader the
// host's no longer hold (`valid`), or the block's end. Most are overwritten first and never
// written back at all.
//
// THE CHECKS ARE THE INTERPRETER'S, made before the instruction has any effect: the segment
// limit, the arena's bounds and, for a store, whether the bytes hold translated code. A failed
// one leaves the block at that instruction with nothing changed, and the interpreter runs it --
// so a guest fault is always the interpreter's, reported as it always was.
#include "jit16_internal.h"
#include <string.h>

// --- the guest's state, as the host code addresses it (rbx = struct cpu16 *) ----------------

#define OFF_R(i)  ((int)offsetof(struct cpu16, r) + 4 * (i))
#define OFF_FLAGS ((int)offsetof(struct cpu16, flags))
#define OFF_IP    ((int)offsetof(struct cpu16, ip))
#define OFF_SREG(s) ((int)offsetof(struct cpu16, sreg) + 2 * (s))
_Static_assert(offsetof(struct cpu16, r) + 32 <= 127 && offsetof(struct cpu16, flags) <= 127 &&
               offsetof(struct cpu16, ip) <= 127 && offsetof(struct cpu16, sreg) + 12 <= 127,
               "the guest state is reached with 8-bit displacements");

#define ALL6 (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

// Host registers. rbx: the CPU; r12: DS's base; r13: the code-line map; r14: SS's base. rax,
// rcx, rdx, r8, r9 are scratch within one instruction.
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

// --- emitting bytes -----------------------------------------------------------------------

struct em { uint8_t *p, *start, *end; int bad; };

static void b1(struct em *e, unsigned v) {
    if (e->p < e->end) *e->p++ = (uint8_t)v;
    else e->bad = 1;
}
static void b4(struct em *e, uint32_t v) { for (int i = 0; i < 4; i++) b1(e, v >> (8 * i)); }
static void b8(struct em *e, uint64_t v) { for (int i = 0; i < 8; i++) b1(e, (unsigned)(v >> (8 * i))); }
static uint32_t pos(const struct em *e) { return (uint32_t)(e->p - e->start); }
static void patch32(struct em *e, uint32_t at, uint32_t target) {
    if (e->bad || at + 4 > (uint32_t)(e->end - e->start)) return;
    uint32_t rel = target - (at + 4);
    memcpy(e->start + at, &rel, 4);
}

// An operand in memory: a guest register's slot ([rbx+disp8]) or a guest byte ([base+rax]).
struct loc { int mem; int disp; int base; };

static struct loc greg(int i) { return (struct loc){ 0, OFF_R(i), 0 }; }
// An 8-bit register: AL CL DL BL, then AH CH DH BH -- the second byte of the first four.
static struct loc greg8(int r) { return (struct loc){ 0, r < 4 ? OFF_R(r) : OFF_R(r - 4) + 1, 0 }; }
static struct loc gwreg(int r, int w) { return w ? greg(r) : greg8(r); }
static struct loc gflags(void) { return (struct loc){ 0, OFF_FLAGS, 0 }; }

// A REX prefix when one is needed: a 64-bit operand, a reg field of r8-r15, or an r8-r15 base.
// The byte registers used here are AL, CL and DL, which a REX does not re-map.
static void rex(struct em *e, int w64, int reg, const struct loc *l) {
    unsigned r = 0x40 | (w64 ? 8 : 0) | (reg & 8 ? 4 : 0) | (l->mem && (l->base & 8) ? 1 : 0);
    if (r != 0x40) b1(e, r);
}

static void modrm(struct em *e, int reg, const struct loc *l) {
    if (!l->mem) {
        b1(e, 0x40 | (reg & 7) << 3 | RBX);              // [rbx + disp8]
        b1(e, (uint8_t)l->disp);
    } else if ((l->base & 7) == 5) {
        b1(e, 0x44 | (reg & 7) << 3);                     // [r13 + rax + 0]: mod 00 would mean disp32
        b1(e, 0x05);
        b1(e, 0);
    } else {
        b1(e, 0x04 | (reg & 7) << 3);                     // [base + rax]
        b1(e, l->base & 7);
    }
}

// One instruction with an r/m operand: `w` its size (0 byte, 1 word, 2 dword), `n` opcode bytes.
static void op_rm(struct em *e, int w, int w64, const uint8_t *opc, int n, int reg, const struct loc *l) {
    if (w == 1) b1(e, 0x66);
    rex(e, w64, reg, l);
    for (int i = 0; i < n; i++) b1(e, opc[i]);
    modrm(e, reg, l);
}
static void op1(struct em *e, int w, uint8_t opc, int reg, const struct loc *l) { op_rm(e, w, 0, &opc, 1, reg, l); }
static void op2(struct em *e, int w, uint8_t a, uint8_t b, int reg, const struct loc *l) {
    uint8_t o[2] = { a, b };
    op_rm(e, w, 0, o, 2, reg, l);
}
// An immediate of the operand's size.
static void immw(struct em *e, int w, uint32_t v) {
    if (w == 0) b1(e, v);
    else if (w == 1) { b1(e, v); b1(e, v >> 8); }
    else b4(e, v);
}

// --- the decoded guest instruction --------------------------------------------------------

struct jop {
    uint32_t len;
    int op;                          // the opcode; 0x100 + the second byte after 0F
    int o16;                         // 66: a word where the instruction says a dword
    int seg;                         // the memory operand's segment: S_DS or S_SS
    int has_modrm, mod, reg, rm;
    int base, index, scale;          // -1 for none
    int32_t disp;
    uint32_t imm;                    // the immediate, or a branch's displacement, sign-extended
    int imm2;                        // C2's second operand is the immediate; others none
};

struct rd { const uint8_t *p; uint32_t n, at; int bad; };
static uint32_t rd8(struct rd *r) { if (r->at >= r->n) { r->bad = 1; return 0; } return r->p[r->at++]; }
static uint32_t rd16(struct rd *r) { uint32_t a = rd8(r); return a | rd8(r) << 8; }
static uint32_t rd32(struct rd *r) { uint32_t a = rd16(r); return a | rd16(r) << 16; }

// A 32-bit ModRM (and SIB and displacement). The default segment is SS through EBP or ESP.
static void modrm32(struct rd *r, struct jop *o, int seg_override) {
    uint32_t m = rd8(r);
    o->has_modrm = 1;
    o->mod = m >> 6;
    o->reg = (m >> 3) & 7;
    o->rm = m & 7;
    o->base = o->index = -1;
    o->scale = 0;
    o->disp = 0;
    if (o->mod == 3) return;
    if (o->rm == 4) {
        uint32_t s = rd8(r);
        o->scale = s >> 6;
        o->index = (s >> 3) & 7;
        if (o->index == 4) o->index = -1;
        o->base = s & 7;
        if (o->base == 5 && o->mod == 0) { o->base = -1; o->disp = (int32_t)rd32(r); }
    } else if (o->rm == 5 && o->mod == 0) {
        o->disp = (int32_t)rd32(r);
    } else {
        o->base = o->rm;
    }
    if (o->mod == 1) o->disp = (int8_t)rd8(r);
    else if (o->mod == 2) o->disp = (int32_t)rd32(r);
    o->seg = seg_override >= 0 ? seg_override : (o->base == R_BP || o->base == R_SP) ? S_SS : S_DS;
}

// Decodes one instruction the translator handles; 0 for anything else (or too few bytes).
static int decode(const uint8_t *p, uint32_t n, struct jop *o) {
    struct rd r = { p, n, 0, 0 };
    int seg = -1;
    memset(o, 0, sizeof *o);
    o->base = o->index = -1;
    o->seg = S_DS;
    uint32_t op;
    for (int k = 0;; k++) {
        op = rd8(&r);
        if (r.bad || k > 4) return 0;
        if (op == 0x66) o->o16 = 1;
        else if (op == 0x3E) seg = S_DS;
        else if (op == 0x36) seg = S_SS;
        else if (op == 0x2E) seg = S_CS;             // LEA only (supported()): GCC's padding is `cs lea`
        else if (op == 0x26) seg = S_ES;
        else if (op == 0xF0) continue;               // LOCK: one CPU, nothing to lock against
        else break;                                  // ES/CS/FS/GS, 67, REP: the interpreter's
    }
    o->seg = seg >= 0 ? seg : S_DS;
    if (op == 0x0F) {
        op = 0x100 | rd8(&r);
        if ((op >= 0x180 && op <= 0x18F)) o->imm = rd32(&r);
        else if (op == 0x1A0 || op == 0x1A8) { /* PUSH FS, GS */ }
        else if ((op >= 0x190 && op <= 0x19F) || op == 0x1AF || op == 0x1B6 || op == 0x1B7 ||
                 op == 0x1BE || op == 0x1BF)
            modrm32(&r, o, seg);
        else return 0;
    } else if (op < 0x40 && (op & 7) < 6) {
        if ((op & 7) < 4) modrm32(&r, o, seg);
        else if ((op & 7) == 4) o->imm = rd8(&r);
        else o->imm = o->o16 ? rd16(&r) : rd32(&r);
    } else {
        // dispatch-ok: one case per x86 opcode the translator takes, a set the 386 fixed in 1985.
        switch (op) {
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0xC3: case 0xC9: case 0xF5: case 0xF8: case 0xF9: case 0xFC: case 0xFD:
            break;
        case 0x68: o->imm = o->o16 ? rd16(&r) : rd32(&r); break;
        case 0x6A: o->imm = (uint32_t)(int32_t)(int8_t)rd8(&r); break;
        case 0x69: modrm32(&r, o, seg); o->imm = o->o16 ? (uint32_t)(int32_t)(int16_t)rd16(&r) : rd32(&r); break;
        case 0x6B: modrm32(&r, o, seg); o->imm = (uint32_t)(int32_t)(int8_t)rd8(&r); break;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        case 0xE2: case 0xE3: case 0xEB:
            o->imm = (uint32_t)(int32_t)(int8_t)rd8(&r);
            break;
        case 0x80: case 0x83: case 0xC0: case 0xC1: case 0xC6:
            modrm32(&r, o, seg);
            o->imm = op == 0x83 ? (uint32_t)(int32_t)(int8_t)rd8(&r) : rd8(&r);
            break;
        case 0x81: case 0xC7:
            modrm32(&r, o, seg);
            o->imm = o->o16 ? rd16(&r) : rd32(&r);
            break;
        case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xFE: case 0xFF:
            modrm32(&r, o, seg);
            break;
        case 0xF6: case 0xF7:
            modrm32(&r, o, seg);
            if (o->reg == 0) o->imm = op == 0xF6 ? rd8(&r) : o->o16 ? rd16(&r) : rd32(&r);
            break;
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:         // MOV to and from moffs32
            o->disp = (int32_t)rd32(&r);
            o->seg = seg >= 0 ? seg : S_DS;
            break;
        case 0x06: case 0x0E: case 0x16: case 0x1E:          // PUSH ES, CS, SS, DS
            break;
        case 0xA8: o->imm = rd8(&r); break;
        case 0xA9: o->imm = o->o16 ? rd16(&r) : rd32(&r); break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            o->imm = rd8(&r);
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            o->imm = o->o16 ? rd16(&r) : rd32(&r);
            break;
        case 0xC2: o->imm = rd16(&r); o->imm2 = 1; break;
        case 0xE8: case 0xE9: o->imm = rd32(&r); break;
        default: return 0;
        }
    }
    if (r.bad) return 0;
    o->op = (int)op;
    o->len = r.at;
    return 1;
}

// --- instructions the block hands the interpreter ------------------------------------------

// Immediates by opcode: 0 none, 1 ib, 2 iw, 3 iz (the operand size's), 4 iw+ib (ENTER), 5 moffs
// (the address size's), 6 ptr16:z. `M` marks a ModRM. 0xFF: not an instruction this decodes.
#define M 0x80
static const uint8_t ONE[256] = {
    M,M,M,M,1,3,0,0, M,M,M,M,1,3,0,0xFF, M,M,M,M,1,3,0,0, M,M,M,M,1,3,0,0,
    M,M,M,M,1,3,0xFF,0, M,M,M,M,1,3,0xFF,0, M,M,M,M,1,3,0xFF,0, M,M,M,M,1,3,0xFF,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,M,M,0xFF,0xFF,0xFF,0xFF, 3,M|3,1,M|1,0,0,0,0, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    M|1,M|3,M|1,M|1,M,M,M,M, M,M,M,M,M,M,M,M, 0,0,0,0,0,0,0,0, 0,0,6,0,0,0,0,0,
    5,5,5,5,0,0,0,0, 1,3,0,0,0,0,0,0, 1,1,1,1,1,1,1,1, 3,3,3,3,3,3,3,3,
    M|1,M|1,2,0,M,M,M|1,M|3, 4,0,2,0,0,1,0,0, M,M,M,M,1,1,0,0, M,M,M,M,M,M,M,M,
    1,1,1,1,1,1,1,1, 3,3,6,1,0,0,0,0, 0xFF,0,0xFF,0xFF,0,0,M,M, 0,0,0,0,0,0,M,M,
};

// An instruction's length -- prefixes, opcode, ModRM, SIB, displacement, immediates -- in a
// 32-bit code segment; 0 when it is not one this knows. *op: the opcode (0F xx: 256 + xx),
// *reg: its ModRM reg field (the group), -1 without one.
static uint32_t insn_len(const uint8_t *p, uint32_t n, int *op, int *reg) {
    struct rd r = { p, n, 0, 0 };
    int o16 = 0, a16 = 0;
    uint32_t b;
    for (int k = 0;; k++) {
        b = rd8(&r);
        if (r.bad || k > 6) return 0;
        if (b == 0x66) o16 = 1;
        else if (b == 0x67) a16 = 1;
        else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 ||
                 b == 0xF0 || b == 0xF2 || b == 0xF3) continue;
        else break;
    }
    unsigned t;
    if (b == 0x0F) {
        uint32_t b2 = rd8(&r);
        *op = (int)(256 + b2);
        if (b2 >= 0x80 && b2 <= 0x8F) t = 3;
        else if ((b2 >= 0x90 && b2 <= 0x9F) || b2 == 0xA3 || b2 == 0xA5 || b2 == 0xAB || b2 == 0xAD || b2 == 0xAF ||
                 (b2 >= 0xB0 && b2 <= 0xB7) || (b2 >= 0xBB && b2 <= 0xBF) || b2 == 0xC0 || b2 == 0xC1)
            t = M;
        else if (b2 == 0xA4 || b2 == 0xAC || b2 == 0xBA) t = M | 1;
        else if (b2 == 0xA0 || b2 == 0xA1 || b2 == 0xA8 || b2 == 0xA9 || (b2 >= 0xC8 && b2 <= 0xCF)) t = 0;
        else return 0;
    } else {
        *op = (int)b;
        t = ONE[b];
        if (t == 0xFF) return 0;
    }
    *reg = -1;
    if (t & M) {
        uint32_t m = rd8(&r);
        int mod = (int)(m >> 6), rm = (int)(m & 7);
        *reg = (int)((m >> 3) & 7);
        if (mod != 3) {
            if (a16) {
                if (mod == 1) r.at += 1;
                else if (mod == 2 || (mod == 0 && rm == 6)) r.at += 2;
            } else {
                if (rm == 4) { uint32_t s = rd8(&r); if (mod == 0 && (s & 7) == 5) r.at += 4; }
                if (mod == 1) r.at += 1;
                else if (mod == 2 || (mod == 0 && rm == 5)) r.at += 4;
            }
        }
        // F6/F7's TEST takes the immediate the others lack.
        if ((b == 0xF6 || b == 0xF7) && *reg <= 1) t |= b == 0xF6 ? 1 : 3;
    }
    switch (t & 0x7F) {
    case 1: r.at += 1; break;
    case 2: r.at += 2; break;
    case 3: r.at += o16 ? 2 : 4; break;
    case 4: r.at += 3; break;
    case 5: r.at += a16 ? 2 : 4; break;
    case 6: r.at += (o16 ? 2 : 4) + 2; break;
    }
    return !r.bad && r.at <= n ? r.at : 0;
}
#undef M

// May the interpreter do it from inside a block? Not a transfer of control (the block's end), and
// nothing that can run the program's code again before it returns -- a far call into Windows, an
// interrupt, the x87's error interrupt -- since that nested run may translate, and flush the arena
// under the block that called out. Nor port I/O.
static int callable(int op, int reg) {
    if (op >= 0x70 && op <= 0x7F) return 0;
    if (op >= 0x180 && op <= 0x18F) return 0;
    if (op >= 0xD8 && op <= 0xDF) return 0;                 // x87: an unmasked error raises INT 75h
    if (op >= 0xE0 && op <= 0xEF) return 0;                 // LOOP, JCXZ, IN/OUT, CALL, JMP
    if (op >= 0x6C && op <= 0x6F) return 0;                 // string I/O
    switch (op) {
    case 0x62: case 0x9A: case 0x9B: case 0xC2: case 0xC3: case 0xCA: case 0xCB: case 0xCC: case 0xCD:
    case 0xCE: case 0xCF: case 0xF4: case 0x100: case 0x101:
        return 0;
    case 0xFF: return reg != 2 && reg != 3 && reg != 4 && reg != 5;
    }
    return 1;
}

// --- translating -------------------------------------------------------------------------

// A data segment as the block's accesses are compiled for it. ARENA: [base + ((off + k) mod 2^32)]
// with the arena's bounds checked (and its code lines for a store); otherwise [base + off].
struct seg { int ok, arena; uint32_t limit, k; uint64_t base; int reg; };

struct fix { uint32_t at, insn; };

struct jx {
    struct cpu16 *c;
    struct em e;
    uint32_t epi;                    // the shared epilogue's offset
    uint16_t pend, valid;            // flags held only in the host's / flags the host's hold
    struct seg ds, ss;
    int lines;
    uint32_t cs_limit;
    uint32_t insn, ip, next;         // the instruction being made: its index, EIP, the next EIP
    struct fix fix[JIT_MAX_INSNS * 8];
    int nfix;
    struct fix out[JIT_MAX_INSNS];   // call-outs whose status says leave
    int nout;
    uint32_t ips[JIT_MAX_INSNS];     // each instruction's EIP, for its exit
};

static void fixup(struct jx *x) {     // a rel32 just emitted that must reach this instruction's exit
    if (x->nfix < (int)(sizeof x->fix / sizeof x->fix[0])) {
        x->fix[x->nfix].at = pos(&x->e) - 4;
        x->fix[x->nfix].insn = x->insn;
        x->nfix++;
    } else {
        x->e.bad = 1;
    }
}
static void jcc32(struct jx *x, uint8_t cc) { b1(&x->e, 0x0F); b1(&x->e, 0x80 | cc); b4(&x->e, 0); fixup(x); }

// c->flags (the bits in `m`) from rax, which holds the host's RFLAGS.
static void merge_rax(struct em *e, uint16_t m) {
    b1(e, 0x25); b4(e, m);                                  // and eax, m
    struct loc f = gflags();
    op1(e, 1, 0x81, 4, &f); immw(e, 1, (uint16_t)~m);       // and word [flags], ~m
    op1(e, 1, 0x09, RAX, &f);                               // or word [flags], ax
}

// The pending flags written back; the host's are no longer anybody's.
static void spill(struct jx *x) {
    if (x->pend) {
        b1(&x->e, 0x9C);                                    // pushfq
        b1(&x->e, 0x58);                                    // pop rax
        merge_rax(&x->e, x->pend);
    }
    x->pend = 0;
    x->valid = 0;
}

// The host's flags from c->flags, for a reader they do not hold.
static void load_flags(struct jx *x) {
    spill(x);
    struct loc f = gflags();
    op2(&x->e, 2, 0x0F, 0xB7, RAX, &f);                     // movzx eax, word [flags]
    b1(&x->e, 0x25); b4(&x->e, ALL6);                       // and eax, ALL6 (TF, DF, IF clear)
    b1(&x->e, 0x50);                                        // push rax
    b1(&x->e, 0x9D);                                        // popfq
    x->valid = ALL6;
}

// The host's CF from c->flags, for ADC, SBB, RCL, RCR, CMC.
static void need_cf(struct jx *x) {
    if (x->valid & F_CF) return;
    spill(x);
    struct loc f = gflags();
    op2(&x->e, 1, 0x0F, 0xBA, 4, &f);                       // bt word [flags], 0
    b1(&x->e, 0);
    x->valid = F_CF;
}

static void produce(struct jx *x, uint16_t w) {
    x->pend |= w;
    x->valid |= w;
}

static void exit_imm(struct jx *x, uint32_t ip, uint32_t n) {
    struct loc l = { 0, OFF_IP, 0 };
    op1(&x->e, 2, 0xC7, 0, &l); b4(&x->e, ip);               // mov dword [ip], ip
    b1(&x->e, 0xB8); b4(&x->e, n);                           // mov eax, n
    b1(&x->e, 0xE9); b4(&x->e, x->epi - (pos(&x->e) + 4));   // jmp epilogue
}

// The offset in eax, for a memory operand's ModRM.
static void ea(struct jx *x, const struct jop *o) {
    struct em *e = &x->e;
    if (o->base >= 0) { struct loc l = greg(o->base); op1(e, 2, 0x8B, RAX, &l); }
    if (o->index >= 0) { struct loc l = greg(o->index); op1(e, 2, 0x8B, RDX, &l); }
    if (o->base >= 0 && o->index >= 0) {
        b1(e, 0x8D); b1(e, 0x84); b1(e, (uint8_t)(o->scale << 6 | RDX << 3 | RAX)); b4(e, (uint32_t)o->disp);
    } else if (o->base >= 0) {
        if (o->disp) { b1(e, 0x8D); b1(e, 0x80); b4(e, (uint32_t)o->disp); }   // lea eax, [rax+disp32]
    } else if (o->index >= 0) {
        b1(e, 0x8D); b1(e, 0x04); b1(e, (uint8_t)(o->scale << 6 | RDX << 3 | 5)); b4(e, (uint32_t)o->disp);
    } else {
        b1(e, 0xB8); b4(e, (uint32_t)o->disp);
    }
}

// The checks for `size` bytes at offset eax in segment s; eax becomes the index from s's base.
// Every path that fails leaves the block before this instruction. 0: the access can never pass
// (a limit below the size), and the instruction is not translated.
static int check(struct jx *x, const struct seg *s, unsigned size, int write) {
    struct em *e = &x->e;
    if (s->limit < size - 1) return 0;
    if (s->limit != 0xFFFFFFFFu || size > 1) {
        b1(e, 0x3D); b4(e, s->limit - (size - 1));         // cmp eax, limit - (size - 1)
        jcc32(x, 0x7);                                      // ja: past the limit
    }
    if (!s->arena) return 1;
    if (s->k) { b1(e, 0x05); b4(e, s->k); }                 // add eax, k (mod 2^32, as the linear address)
    b1(e, 0x3D); b4(e, x->c->lin_size - size);              // cmp eax, lin_size - size
    jcc32(x, 0x7);                                          // ja: outside w16's memory
    if (write && x->lines) {
        // A store into a line translated code was made from: the interpreter does it, and drops them.
        b1(e, 0x89); b1(e, 0xC1);                           // mov ecx, eax
        b1(e, 0xC1); b1(e, 0xE9); b1(e, CPU16_JIT_LINE);    // shr ecx, 6
        b1(e, 0x41); b1(e, 0x0F); b1(e, 0xB6); b1(e, 0x4C); b1(e, 0x0D); b1(e, 0);   // movzx ecx, byte [r13+rcx]
        if (size > 1) {
            b1(e, 0x8D); b1(e, 0x50); b1(e, size - 1);      // lea edx, [rax + size-1]
            b1(e, 0xC1); b1(e, 0xEA); b1(e, CPU16_JIT_LINE); // shr edx, 6
            b1(e, 0x41); b1(e, 0x0A); b1(e, 0x4C); b1(e, 0x15); b1(e, 0);           // or cl, byte [r13+rdx]
        } else {
            b1(e, 0x85); b1(e, 0xC9);                       // test ecx, ecx
        }
        jcc32(x, 0x5);                                      // jnz
    }
    return 1;
}

// A memory operand: spilled, computed, checked. Its location for the access after.
static int mem_operand(struct jx *x, const struct jop *o, unsigned size, int write, struct loc *l) {
    const struct seg *s = o->seg == S_SS ? &x->ss : &x->ds;
    spill(x);
    ea(x, o);
    if (!check(x, s, size, write)) return 0;
    *l = (struct loc){ 1, 0, s->reg };
    return 1;
}

// The r/m operand: a register's slot, or the checked memory operand.
static int rm_operand(struct jx *x, const struct jop *o, int w, int write, struct loc *l) {
    if (o->mod == 3) { *l = gwreg(o->rm, w); return 1; }
    return mem_operand(x, o, w == 2 ? 4 : w ? 2 : 1, write, l);
}

static int size_of(const struct jop *o, int wbit) { return wbit ? (o->o16 ? 1 : 2) : 0; }

// What can be decided before emitting anything: 0 means the block ends before this instruction.
static int supported(struct jx *x, const struct jop *o) {
    int op = o->op;
    if (o->has_modrm && o->mod != 3 && o->op != 0x8D &&
        ((o->seg != S_DS && o->seg != S_SS) || !(o->seg == S_SS ? &x->ss : &x->ds)->ok))
        return 0;                                    // a CS or ES operand is the interpreter's
    if (!o->has_modrm && o->seg != S_DS && o->seg != S_SS) return 0;
    // dispatch-ok: one case per x86 opcode the translator takes, a set the 386 fixed in 1985.
    switch (op) {
    case 0x68: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x6A: case 0xC2: case 0xC3: case 0xC9: case 0xE8:
        return !o->o16 && x->ss.ok;
    case 0x8D: return o->mod != 3;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: return (o->seg == S_SS ? &x->ss : &x->ds)->ok;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x1A0: case 0x1A8: return !o->o16 && x->ss.ok;
    case 0xC6: case 0xC7: return o->reg == 0;
    case 0xFE: return o->reg <= 1;
    case 0xFF:
        if (o->reg <= 1) return 1;
        if (o->reg == 2 || o->reg == 4 || o->reg == 6) return !o->o16 && (o->reg == 4 || x->ss.ok);
        return 0;
    case 0xF6: case 0xF7:
        if (o->reg == 1) return 0;
        if (o->reg >= 6) return op == 0xF7 && !o->o16;     // DIV and IDIV of a dword only
        return 1;
    case 0x1B7: case 0x1BF: return !o->o16;
    case 0xE2: case 0xE3: return !o->o16;
    case 0xE9: return !o->o16;
    }
    if (op >= 0x180 && op <= 0x18F) return !o->o16;
    if (op >= 0x190 && op <= 0x19F) return o->mod == 3;  // SETcc into memory: the interpreter's
    return 1;
}

// The condition a Jcc/SETcc reads.
static uint16_t cc_reads(int cc) {
    static const uint16_t r[8] = { F_OF, F_CF, F_ZF, F_CF | F_ZF, F_SF, F_PF, F_SF | F_OF, F_ZF | F_SF | F_OF };
    return r[(cc >> 1) & 7];
}

// A two-way end: Jcc `cc` to `target`, else on to `fall`. Pending flags are written back on
// both arms, from one PUSHFQ taken before the branch reads them.
static void two_way(struct jx *x, int cc, uint32_t target, uint32_t fall) {
    struct em *e = &x->e;
    uint16_t m = x->pend;
    if (cc_reads(cc) & ~x->valid) { load_flags(x); m = 0; }
    if (m) b1(e, 0x9C);                                     // pushfq: the pending flags, for both arms
    b1(e, 0x0F); b1(e, 0x80 | cc); b4(e, 0);                // jcc taken
    uint32_t j = pos(e) - 4;
    if (m) { b1(e, 0x58); merge_rax(e, m); }
    exit_imm(x, fall, x->insn + 1);
    patch32(e, j, pos(e));
    if (m) { b1(e, 0x58); merge_rax(e, m); }
    exit_imm(x, target, x->insn + 1);
}

// The stack's next slot for a push (r8d = the new ESP, eax = its index), checked.
static int push_slot(struct jx *x) {
    struct em *e = &x->e;
    struct loc sp = greg(R_SP);
    spill(x);
    op1(e, 2, 0x8B, RAX, &sp);                              // mov eax, [esp]
    b1(e, 0x8D); b1(e, 0x40); b1(e, 0xFC);                  // lea eax, [rax-4]
    b1(e, 0x41); b1(e, 0x89); b1(e, 0xC0);                  // mov r8d, eax
    return check(x, &x->ss, 4, 1);
}

// The stack's top for a pop of 4 + `extra` bytes (r8d = the new ESP), checked; r9d = the dword.
static int pop_slot(struct jx *x, int from_ebp, uint32_t extra) {
    struct em *e = &x->e;
    struct loc sp = greg(from_ebp ? R_BP : R_SP);
    spill(x);
    op1(e, 2, 0x8B, RAX, &sp);                              // mov eax, [esp] (or [ebp], LEAVE)
    b1(e, 0x44); b1(e, 0x8D); b1(e, 0x80); b4(e, 4 + extra); // lea r8d, [rax + 4 + extra]
    if (!check(x, &x->ss, 4, 0)) return 0;
    struct loc s = { 1, 0, x->ss.reg };
    op1(e, 2, 0x8B, R9, &s);                                // mov r9d, [ss:esp]
    return 1;
}

static void set_esp_r8(struct jx *x) { struct loc sp = greg(R_SP); op1(&x->e, 2, 0x89, R8, &sp); }

// A computed target in r9d inside CS's limit, or the instruction is the interpreter's.
static void check_target(struct jx *x) {
    if (x->cs_limit == 0xFFFFFFFFu) return;
    b1(&x->e, 0x41); b1(&x->e, 0x81); b1(&x->e, 0xF9); b4(&x->e, x->cs_limit);   // cmp r9d, limit
    jcc32(x, 0x7);
}

// One instruction the translator lacks, done by the interpreter from here (jit16_callout()). A
// status other than 0 leaves: 1 with this instruction done, 2 without (a fault).
static void callout(struct jx *x, uint32_t next, int op) {
    struct em *e = &x->e;
    spill(x);
    struct loc l = { 0, OFF_IP, 0 };
    op1(e, 2, 0xC7, 0, &l); b4(e, x->ip);                     // mov dword [ip], this instruction
    b1(e, 0x48); b1(e, 0x89); b1(e, 0xDF);                    // mov rdi, rbx
    b1(e, 0xBE); b4(e, next);                                 // mov esi, next
    b1(e, 0xBA); b4(e, (uint32_t)op);                         // mov edx, op
    b1(e, 0x48); b1(e, 0xB8); b8(e, (uint64_t)(uintptr_t)jit16_callout);   // mov rax, jit16_callout
    b1(e, 0xFF); b1(e, 0xD0);                                 // call rax
    b1(e, 0x85); b1(e, 0xC0);                                 // test eax, eax
    b1(e, 0x0F); b1(e, 0x85); b4(e, 0);                       // jnz: leave
    if (x->nout < (int)(sizeof x->out / sizeof x->out[0])) {
        x->out[x->nout].at = pos(e) - 4;
        x->out[x->nout].insn = x->insn;
        x->nout++;
    } else {
        e->bad = 1;
    }
}

static void exit_r9(struct jx *x, uint32_t n) {
    struct loc l = { 0, OFF_IP, 0 };
    op1(&x->e, 2, 0x89, R9, &l);                            // mov [ip], r9d
    b1(&x->e, 0xB8); b4(&x->e, n);
    b1(&x->e, 0xE9); b4(&x->e, x->epi - (pos(&x->e) + 4));
}

// Translates one instruction. 1: done, go on; 2: done, and the block ended with it; 0: cannot.
static int insn(struct jx *x, const struct jop *o) {
    struct em *e = &x->e;
    int op = o->op;
    struct loc l, rl;

    if (op < 0x40 && (op & 7) < 6) {                        // ADD OR ADC SBB AND SUB XOR CMP
        int alu = op >> 3, w = size_of(o, op & 1), wr = alu != 7;
        uint16_t reads = (alu == 2 || alu == 3) ? F_CF : 0;
        switch (op & 7) {
        case 0: case 1:                                      // r/m op= reg
            if (!rm_operand(x, o, w, wr, &l)) return 0;
            rl = gwreg(o->reg, w);
            op1(e, w, w ? 0x8B : 0x8A, RCX, &rl);
            if (reads) need_cf(x);
            op1(e, w, (uint8_t)(alu << 3 | (w ? 1 : 0)), RCX, &l);
            break;
        case 2: case 3:                                      // reg op= r/m
            if (!rm_operand(x, o, w, 0, &l)) return 0;
            op1(e, w, w ? 0x8B : 0x8A, RCX, &l);
            rl = gwreg(o->reg, w);
            if (reads) need_cf(x);
            op1(e, w, (uint8_t)(alu << 3 | (w ? 1 : 0)), RCX, &rl);
            break;
        default:                                             // AL/AX/EAX op= imm
            rl = gwreg(R_AX, w);
            if (reads) need_cf(x);
            op1(e, w, w ? 0x81 : 0x80, alu, &rl);
            immw(e, w, o->imm);
            break;
        }
        produce(x, ALL6);
        return 1;
    }
    if (op >= 0x40 && op <= 0x4F) {                         // INC, DEC r
        int w = size_of(o, 1);
        l = greg(op & 7);
        op1(e, w, 0xFF, op >= 0x48, &l);
        produce(x, ALL6 & ~F_CF);
        return 1;
    }
    if (op >= 0x50 && op <= 0x57) {                         // PUSH r
        struct loc r = greg(op & 7);
        op1(e, 2, 0x8B, R9, &r);                            // the old ESP, for PUSH ESP
        if (!push_slot(x)) return 0;
        struct loc s = { 1, 0, x->ss.reg };
        op1(e, 2, 0x89, R9, &s);
        set_esp_r8(x);
        return 1;
    }
    if (op >= 0x58 && op <= 0x5F) {                         // POP r
        if (!pop_slot(x, 0, 0)) return 0;
        set_esp_r8(x);
        struct loc r = greg(op & 7);                         // after ESP: POP ESP loads the value
        op1(e, 2, 0x89, R9, &r);
        return 1;
    }
    if (op >= 0x70 && op <= 0x7F) {                         // Jcc rel8
        uint32_t t = x->next + o->imm;
        if (t > x->cs_limit) return 0;
        two_way(x, op & 15, t, x->next);
        return 2;
    }
    if (op >= 0x180 && op <= 0x18F) {                       // Jcc rel32
        uint32_t t = x->next + o->imm;
        if (t > x->cs_limit) return 0;
        two_way(x, op & 15, t, x->next);
        return 2;
    }
    if (op >= 0x190 && op <= 0x19F) {                       // SETcc r8
        if (cc_reads(op & 15) & ~x->valid) load_flags(x);
        l = greg8(o->rm);
        op2(e, 0, 0x0F, (uint8_t)(0x90 | (op & 15)), 0, &l);
        return 1;
    }
    if (op >= 0xB0 && op <= 0xB7) { l = greg8(op & 7); op1(e, 0, 0xC6, 0, &l); b1(e, o->imm); return 1; }
    if (op >= 0xB8 && op <= 0xBF) {
        int w = size_of(o, 1);
        l = greg(op & 7);
        op1(e, w, 0xC7, 0, &l);
        immw(e, w, o->imm);
        return 1;
    }
    if (op >= 0x91 && op <= 0x97) {                         // XCHG eAX, r
        int w = size_of(o, 1);
        struct loc a = greg(R_AX), r = greg(op & 7);
        op1(e, w, 0x8B, RCX, &a);
        op1(e, w, 0x8B, RDX, &r);
        op1(e, w, 0x89, RDX, &a);
        op1(e, w, 0x89, RCX, &r);
        return 1;
    }

    // dispatch-ok: one case per x86 opcode the translator takes, a set the 386 fixed in 1985.
    switch (op) {
    case 0x90: return 1;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {            // MOV AL/eAX to and from moffs32
        int w = size_of(o, op & 1), store = op >= 0xA2;
        if (!mem_operand(x, o, w == 2 ? 4 : w ? 2 : 1, store, &l)) return 0;
        rl = gwreg(R_AX, w);
        if (store) { op1(e, w, w ? 0x8B : 0x8A, RCX, &rl); op1(e, w, w ? 0x89 : 0x88, RCX, &l); }
        else { op1(e, w, w ? 0x8B : 0x8A, RCX, &l); op1(e, w, w ? 0x89 : 0x88, RCX, &rl); }
        return 1;
    }
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x1A0: case 0x1A8: {   // PUSH sreg: ESP-4, a word
        int s = op == 0x1A0 ? S_FS : op == 0x1A8 ? S_GS : op >> 3;
        struct loc sr = { 0, OFF_SREG(s), 0 };
        op2(e, 2, 0x0F, 0xB7, R9, &sr);                      // movzx r9d, word [sreg]
        if (!push_slot(x)) return 0;
        struct loc st = { 1, 0, x->ss.reg };
        op1(e, 1, 0x89, R9, &st);                            // mov word [ss:esp], r9w
        set_esp_r8(x);
        return 1;
    }
    case 0x68: case 0x6A: {                                  // PUSH imm
        b1(e, 0x41); b1(e, 0xB9); b4(e, o->imm);             // mov r9d, imm
        if (!push_slot(x)) return 0;
        struct loc s = { 1, 0, x->ss.reg };
        op1(e, 2, 0x89, R9, &s);
        set_esp_r8(x);
        return 1;
    }
    case 0x69: case 0x6B: {                                  // IMUL reg, r/m, imm
        int w = size_of(o, 1);
        if (!rm_operand(x, o, w, 0, &l)) return 0;
        op1(e, w, 0x8B, RCX, &l);
        if (w == 1) b1(e, 0x66);
        if (op == 0x6B) { b1(e, 0x6B); b1(e, 0xC1); b1(e, o->imm); }
        else { b1(e, 0x69); b1(e, 0xC1); immw(e, w, o->imm); }
        rl = greg(o->reg);
        op1(e, w, 0x89, RAX, &rl);
        produce(x, ALL6);
        return 1;
    }
    case 0x80: case 0x81: case 0x83: {                      // group 1: r/m op= imm
        int w = op == 0x80 ? 0 : size_of(o, 1), alu = o->reg;
        if (!rm_operand(x, o, w, alu != 7, &l)) return 0;
        if (alu == 2 || alu == 3) need_cf(x);
        if (op == 0x83 && w) { op1(e, w, 0x83, alu, &l); b1(e, o->imm); }
        else { op1(e, w, w ? 0x81 : 0x80, alu, &l); immw(e, w, o->imm); }
        produce(x, ALL6);
        return 1;
    }
    case 0x84: case 0x85: {                                  // TEST r/m, reg
        int w = size_of(o, op & 1);
        if (!rm_operand(x, o, w, 0, &l)) return 0;
        rl = gwreg(o->reg, w);
        op1(e, w, w ? 0x8B : 0x8A, RCX, &rl);
        op1(e, w, w ? 0x85 : 0x84, RCX, &l);
        produce(x, ALL6);
        return 1;
    }
    case 0xA8: case 0xA9: {                                  // TEST AL/eAX, imm
        int w = size_of(o, op & 1);
        l = gwreg(R_AX, w);
        op1(e, w, w ? 0xF7 : 0xF6, 0, &l);
        immw(e, w, o->imm);
        produce(x, ALL6);
        return 1;
    }
    case 0x86: case 0x87: {                                  // XCHG r/m, reg
        int w = size_of(o, op & 1);
        if (!rm_operand(x, o, w, 1, &l)) return 0;
        rl = gwreg(o->reg, w);
        op1(e, w, w ? 0x8B : 0x8A, RDX, &l);
        op1(e, w, w ? 0x8B : 0x8A, RCX, &rl);
        op1(e, w, w ? 0x89 : 0x88, RCX, &l);
        op1(e, w, w ? 0x89 : 0x88, RDX, &rl);
        return 1;
    }
    case 0x88: case 0x89: {                                  // MOV r/m, reg
        int w = size_of(o, op & 1);
        if (!rm_operand(x, o, w, 1, &l)) return 0;
        rl = gwreg(o->reg, w);
        op1(e, w, w ? 0x8B : 0x8A, RCX, &rl);
        op1(e, w, w ? 0x89 : 0x88, RCX, &l);
        return 1;
    }
    case 0x8A: case 0x8B: {                                  // MOV reg, r/m
        int w = size_of(o, op & 1);
        if (!rm_operand(x, o, w, 0, &l)) return 0;
        op1(e, w, w ? 0x8B : 0x8A, RCX, &l);
        rl = gwreg(o->reg, w);
        op1(e, w, w ? 0x89 : 0x88, RCX, &rl);
        return 1;
    }
    case 0x8D:                                               // LEA
        ea(x, o);
        rl = greg(o->reg);
        op1(e, size_of(o, 1), 0x89, RAX, &rl);
        return 1;
    case 0x98: {                                             // CWDE, CBW
        struct loc a = greg(R_AX), a8 = greg8(R_AX);
        if (o->o16) { op2(e, 1, 0x0F, 0xBE, RCX, &a8); op1(e, 1, 0x89, RCX, &a); }
        else { op2(e, 2, 0x0F, 0xBF, RCX, &a); op1(e, 2, 0x89, RCX, &a); }
        return 1;
    }
    case 0x99: {                                             // CDQ, CWD
        int w = size_of(o, 1);
        struct loc a = greg(R_AX), d = greg(R_DX);
        op1(e, w, 0x8B, RAX, &a);
        if (w == 1) b1(e, 0x66);
        b1(e, 0x99);
        op1(e, w, 0x89, RDX, &d);
        return 1;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: {            // shifts and rotates by a constant
        int w = size_of(o, op & 1), sub = o->reg == 6 ? 4 : o->reg;
        unsigned n = (op <= 0xC1 ? o->imm : 1) & 0x1F;
        if (!rm_operand(x, o, w, n != 0, &l)) return 0;
        if (!n) return 1;                                    // no change at all, flags included
        if (sub == 2 || sub == 3) need_cf(x);
        op1(e, w, w ? 0xC1 : 0xC0, sub, &l);
        b1(e, n);
        produce(x, sub <= 3 ? (F_CF | F_OF) : ALL6);
        return 1;
    }
    case 0xD2: case 0xD3: {                                  // ... by CL: a count of 0 changes nothing
        int w = size_of(o, op & 1), sub = o->reg == 6 ? 4 : o->reg;
        if (!rm_operand(x, o, w, 1, &l)) return 0;
        spill(x);
        struct loc cl = greg8(R_CX);
        op2(e, 2, 0x0F, 0xB6, RCX, &cl);                     // movzx ecx, byte [cl]
        b1(e, 0xF6); b1(e, 0xC1); b1(e, 0x1F);               // test cl, 31
        b1(e, 0x74); b1(e, 0);                               // jz past
        uint32_t jz = pos(e) - 1;
        if (sub == 2 || sub == 3) { struct loc f = gflags(); op2(e, 1, 0x0F, 0xBA, 4, &f); b1(e, 0); }
        op1(e, w, w ? 0xD3 : 0xD2, sub, &l);
        b1(e, 0x9C); b1(e, 0x58);                             // the result's flags, written back now
        merge_rax(e, sub <= 3 ? (F_CF | F_OF) : ALL6);
        if (!e->bad) e->start[jz] = (uint8_t)(pos(e) - (jz + 1));
        x->pend = 0;
        x->valid = 0;
        return 1;
    }
    case 0xC2: case 0xC3:                                    // RET, RET n
        if (!pop_slot(x, 0, op == 0xC2 ? (o->imm & 0xFFFF) : 0)) return 0;
        check_target(x);
        set_esp_r8(x);
        exit_r9(x, x->insn + 1);
        return 2;
    case 0xC6: case 0xC7: {                                  // MOV r/m, imm
        int w = size_of(o, op & 1);
        if (!rm_operand(x, o, w, 1, &l)) return 0;
        op1(e, w, w ? 0xC7 : 0xC6, 0, &l);
        immw(e, w, o->imm);
        return 1;
    }
    case 0xC9: {                                             // LEAVE
        if (!pop_slot(x, 1, 0)) return 0;
        set_esp_r8(x);
        struct loc bp = greg(R_BP);
        op1(e, 2, 0x89, R9, &bp);
        return 1;
    }
    case 0xE2: case 0xE3: {                                  // LOOP, JECXZ
        uint32_t t = x->next + o->imm;
        if (t > x->cs_limit) return 0;
        struct loc cx = greg(R_CX);
        uint16_t m = x->pend;
        op1(e, 2, 0x8B, RCX, &cx);                           // mov ecx, [ecx]
        if (op == 0xE2) {
            b1(e, 0x8D); b1(e, 0x49); b1(e, 0xFF);           // lea ecx, [rcx-1]: no flags
            op1(e, 2, 0x89, RCX, &cx);
        }
        if (m) b1(e, 0x9C);
        b1(e, 0x67); b1(e, 0xE3); b1(e, 0);                  // jecxz
        uint32_t j = pos(e) - 1;
        // LOOP: ECX 0 falls through; JECXZ: ECX 0 branches.
        uint32_t first = op == 0xE2 ? t : x->next, second = op == 0xE2 ? x->next : t;
        if (m) { b1(e, 0x58); merge_rax(e, m); }
        exit_imm(x, first, x->insn + 1);
        uint32_t d = pos(e) - (j + 1);
        if (d > 127) return 0;
        if (!e->bad) e->start[j] = (uint8_t)d;
        if (m) { b1(e, 0x58); merge_rax(e, m); }
        exit_imm(x, second, x->insn + 1);
        return 2;
    }
    case 0xE8: {                                             // CALL rel32
        uint32_t t = x->next + o->imm;
        if (t > x->cs_limit) return 0;
        if (!push_slot(x)) return 0;
        struct loc s = { 1, 0, x->ss.reg };
        op1(e, 2, 0xC7, 0, &s); b4(e, x->next);
        set_esp_r8(x);
        exit_imm(x, t, x->insn + 1);
        return 2;
    }
    case 0xE9: case 0xEB: {                                  // JMP rel
        uint32_t t = x->next + o->imm;
        if (t > x->cs_limit) return 0;
        spill(x);
        exit_imm(x, t, x->insn + 1);
        return 2;
    }
    case 0xF5:                                               // CMC
        need_cf(x);
        b1(e, 0xF5);
        produce(x, F_CF);
        return 1;
    case 0xF8: b1(e, 0xF8); produce(x, F_CF); return 1;      // CLC
    case 0xF9: b1(e, 0xF9); produce(x, F_CF); return 1;      // STC
    case 0xFC: case 0xFD: {                                  // CLD, STD
        spill(x);
        struct loc f = gflags();
        op1(e, 1, 0x81, op == 0xFC ? 4 : 1, &f);
        immw(e, 1, op == 0xFC ? (uint16_t)~F_DF : F_DF);
        return 1;
    }
    case 0xF6: case 0xF7: {                                  // group 3
        int w = size_of(o, op & 1);
        int writes = o->reg == 2 || o->reg == 3;
        if (!rm_operand(x, o, w, writes, &l)) return 0;
        struct loc a = greg(R_AX), d = greg(R_DX);
        switch (o->reg) {
        case 0: op1(e, w, w ? 0xF7 : 0xF6, 0, &l); immw(e, w, o->imm); produce(x, ALL6); return 1;
        case 2: op1(e, w, w ? 0xF7 : 0xF6, 2, &l); return 1;
        case 3: op1(e, w, w ? 0xF7 : 0xF6, 3, &l); produce(x, ALL6); return 1;
        case 4: case 5:                                      // MUL, IMUL: eDX:eAX (AX) = eAX * r/m
            op1(e, w, w ? 0x8B : 0x8A, RCX, &l);
            if (w) op1(e, w, 0x8B, RAX, &a);
            else { struct loc al = greg8(R_AX); op1(e, 0, 0x8A, RAX, &al); }
            if (w == 1) b1(e, 0x66);
            b1(e, w ? 0xF7 : 0xF6); b1(e, o->reg == 4 ? 0xE1 : 0xE9);
            if (w) { op1(e, w, 0x89, RAX, &a); op1(e, w, 0x89, RDX, &d); }
            else op1(e, 1, 0x89, RAX, &a);
            produce(x, ALL6);
            return 1;
        case 6:                                              // DIV r/m32: a zero or a quotient past
            op1(e, 2, 0x8B, RCX, &l);                        // 32 bits is the interpreter's #DE
            spill(x);
            op1(e, 2, 0x8B, RAX, &a);
            op1(e, 2, 0x8B, RDX, &d);
            b1(e, 0x85); b1(e, 0xC9); jcc32(x, 0x4);         // test ecx, ecx; jz
            b1(e, 0x39); b1(e, 0xCA); jcc32(x, 0x3);         // cmp edx, ecx; jae
            b1(e, 0xF7); b1(e, 0xF1);                        // div ecx
            op1(e, 2, 0x89, RAX, &a);
            op1(e, 2, 0x89, RDX, &d);
            x->valid = 0;                                    // undefined, and the host's are not the guest's
            return 1;
        default: {                                           // IDIV r/m32, in 64 bits: the overflow is
            op1(e, 2, 0x8B, RCX, &l);                        // found, never raised
            spill(x);
            op1(e, 2, 0x8B, RAX, &a);
            op1(e, 2, 0x8B, RDX, &d);
            b1(e, 0x48); b1(e, 0xC1); b1(e, 0xE2); b1(e, 32); // shl rdx, 32
            b1(e, 0x48); b1(e, 0x09); b1(e, 0xD0);            // or rax, rdx: the signed dividend
            b1(e, 0x48); b1(e, 0x63); b1(e, 0xC9);            // movsxd rcx, ecx
            b1(e, 0x48); b1(e, 0x85); b1(e, 0xC9); jcc32(x, 0x4);   // test rcx, rcx; jz
            b1(e, 0x48); b1(e, 0x83); b1(e, 0xF9); b1(e, 0xFF);     // cmp rcx, -1
            b1(e, 0x75); b1(e, 0);                                  // jne: past the INT64_MIN test
            uint32_t jne = pos(e) - 1;
            b1(e, 0x48); b1(e, 0xBA); b8(e, 0x8000000000000000ULL); // mov rdx, INT64_MIN
            b1(e, 0x48); b1(e, 0x39); b1(e, 0xD0); jcc32(x, 0x4);   // cmp rax, rdx; je
            if (!e->bad) e->start[jne] = (uint8_t)(pos(e) - (jne + 1));
            b1(e, 0x48); b1(e, 0x99);                               // cqo
            b1(e, 0x48); b1(e, 0xF7); b1(e, 0xF9);                  // idiv rcx
            b1(e, 0x4C); b1(e, 0x63); b1(e, 0xC0);                  // movsxd r8, eax
            b1(e, 0x4C); b1(e, 0x39); b1(e, 0xC0); jcc32(x, 0x5);   // cmp rax, r8; jne: past 32 bits
            op1(e, 2, 0x89, RAX, &a);
            op1(e, 2, 0x89, RDX, &d);
            x->valid = 0;
            return 1;
        }
        }
    }
    case 0xFE: case 0xFF: {                                  // group 4/5
        int w = size_of(o, op & 1);
        if (o->reg <= 1) {
            if (!rm_operand(x, o, w, 1, &l)) return 0;
            op1(e, w, w ? 0xFF : 0xFE, o->reg, &l);
            produce(x, ALL6 & ~F_CF);
            return 1;
        }
        if (!rm_operand(x, o, 2, 0, &l)) return 0;
        op1(e, 2, 0x8B, R9, &l);                             // the target, or the dword to push
        spill(x);                                            // a register operand spilled nothing yet
        if (o->reg == 4) {                                   // JMP r/m32
            check_target(x);
            exit_r9(x, x->insn + 1);
            return 2;
        }
        if (o->reg == 2) check_target(x);
        if (!push_slot(x)) return 0;
        struct loc s = { 1, 0, x->ss.reg };
        if (o->reg == 6) { op1(e, 2, 0x89, R9, &s); set_esp_r8(x); return 1; }
        op1(e, 2, 0xC7, 0, &s); b4(e, x->next);             // CALL r/m32
        set_esp_r8(x);
        exit_r9(x, x->insn + 1);
        return 2;
    }
    case 0x1AF: {                                            // IMUL reg, r/m
        int w = size_of(o, 1);
        if (!rm_operand(x, o, w, 0, &l)) return 0;
        op1(e, w, 0x8B, RCX, &l);
        rl = greg(o->reg);
        op1(e, w, 0x8B, RAX, &rl);
        if (w == 1) b1(e, 0x66);
        b1(e, 0x0F); b1(e, 0xAF); b1(e, 0xC1);               // imul eax, ecx
        op1(e, w, 0x89, RAX, &rl);
        produce(x, ALL6);
        return 1;
    }
    case 0x1B6: case 0x1B7: case 0x1BE: case 0x1BF: {        // MOVZX, MOVSX
        int byte = op == 0x1B6 || op == 0x1BE;
        if (!rm_operand(x, o, byte ? 0 : 1, 0, &l)) return 0;
        op2(e, 2, 0x0F, (uint8_t)(op & 0xFF), RCX, &l);
        rl = greg(o->reg);
        op1(e, o->o16 ? 1 : 2, 0x89, RCX, &rl);
        return 1;
    }
    }
    return 0;
}

static int seg_setup(struct cpu16 *c, int s, struct seg *g, int reg) {
    const struct cpu16_desc *d = cpu16_seg(c, s);
    memset(g, 0, sizeof *g);
    g->reg = reg;
    if (!d || d->kind != D_DATA) return 0;
    g->limit = d->limit;
    if (d->flags & CPU16_DF_LIN) {
        if (!c->lin) return 0;
        g->arena = 1;
        g->base = (uint64_t)(uintptr_t)c->lin;
        g->k = d->lin - c->lin_origin;
    } else if (!d->base) {
        return 0;
    } else if (c->lin && d->base >= c->lin && (uint64_t)(d->base - c->lin) + d->limit < c->lin_size) {
        g->arena = 1;                                        // an ordinary segment inside the arena: its
        g->base = (uint64_t)(uintptr_t)c->lin;               // stores need the code-line test too
        g->k = (uint32_t)(d->base - c->lin);
    } else {
        g->base = (uint64_t)(uintptr_t)d->base;
    }
    g->ok = 1;
    return 1;
}

uint32_t jit16_translate(struct cpu16 *c, struct jblock *b, const uint8_t *host, uint32_t avail,
                         uint8_t *buf, uint32_t cap, int *stop_op, int *stop_0f) {
    static struct jx xs;                                     // big; one CPU thread translates
    struct jx *x = &xs;
    memset(x, 0, offsetof(struct jx, fix));
    x->nfix = 0;
    x->nout = 0;
    x->c = c;
    x->e = (struct em){ buf, buf, buf + cap, 0 };
    struct em *e = &x->e;

    const struct cpu16_desc *cs = cpu16_seg(c, S_CS);
    if (!cs || cs->kind != D_CODE || !(cs->flags & CPU16_DF_BIG)) return 0;
    x->cs_limit = cs->limit;
    seg_setup(c, S_DS, &x->ds, R12);
    if (seg_setup(c, S_SS, &x->ss, R14) && !(cpu16_seg(c, S_SS)->flags & CPU16_DF_BIG)) x->ss.ok = 0;
    x->lines = c->jit_lines != 0;

    // The epilogue first, so every exit's jump to it is backwards and known.
    x->epi = 0;
    b1(e, 0x48); b1(e, 0x83); b1(e, 0xC4); b1(e, 8);         // add rsp, 8
    b1(e, 0x41); b1(e, 0x5E);                                // pop r14
    b1(e, 0x41); b1(e, 0x5D);                                // pop r13
    b1(e, 0x41); b1(e, 0x5C);                                // pop r12
    b1(e, 0x5B);                                             // pop rbx
    b1(e, 0xC3);                                             // ret
    while (pos(e) & 15) b1(e, 0xCC);
    uint32_t entry = pos(e);
    b1(e, 0x53);                                             // push rbx
    b1(e, 0x41); b1(e, 0x54);                                // push r12
    b1(e, 0x41); b1(e, 0x55);                                // push r13
    b1(e, 0x41); b1(e, 0x56);                                // push r14
    b1(e, 0x48); b1(e, 0x83); b1(e, 0xEC); b1(e, 8);         // sub rsp, 8: 16-aligned for a call-out
    b1(e, 0x48); b1(e, 0x89); b1(e, 0xFB);                   // mov rbx, rdi
    b1(e, 0x49); b1(e, 0xBC); b8(e, x->ds.base);             // mov r12, DS's base
    b1(e, 0x49); b1(e, 0xBD); b8(e, (uint64_t)(uintptr_t)c->jit_lines);   // mov r13, the lines
    b1(e, 0x49); b1(e, 0xBE); b8(e, x->ss.base);             // mov r14, SS's base

    uint32_t at = 0, ip = c->ip;
    int ended = 0;
    while (x->insn < JIT_MAX_INSNS && !ended) {
        struct jop o;
        if (cap - pos(e) < 1024) break;                      // room for one more and the exits
        // What the translator lacks but the interpreter may do from inside the block: called out.
        int cop = -1, creg = -1;
        uint32_t clen = at < avail ? insn_len(host + at, avail - at, &cop, &creg) : 0;
        int native = clen && decode(host + at, avail - at, &o) && o.len == clen && supported(x, &o);
        if (clen && !native && callable(cop, creg)) {
            x->ip = x->ips[x->insn] = ip;
            callout(x, ip + clen, cop);
            if (e->bad) break;
            at += clen;
            ip += clen;
            x->insn++;
            continue;
        }
        if (at >= avail || !decode(host + at, avail - at, &o)) {
            if (at < avail) {
                // Name the opcode that stopped it, prefixes skipped, for the timing build's list.
                uint32_t k = at;
                while (k < avail && (host[k] == 0x66 || host[k] == 0x67 || host[k] == 0xF2 || host[k] == 0xF3 ||
                                     host[k] == 0x26 || host[k] == 0x2E || host[k] == 0x36 || host[k] == 0x3E ||
                                     host[k] == 0x64 || host[k] == 0x65 || host[k] == 0xF0))
                    k++;
                if (k < avail) { *stop_op = host[k]; if (host[k] == 0x0F && k + 1 < avail) *stop_0f = host[k + 1]; }
            }
            break;
        }
        if (o.len > avail - at || !supported(x, &o)) { *stop_op = o.op & 0xFF; if (o.op > 0xFF) *stop_op = 0x0F, *stop_0f = o.op & 0xFF; break; }
        x->ip = x->ips[x->insn] = ip;
        x->next = ip + o.len;
        uint8_t *mark = e->p;
        uint16_t pend = x->pend, valid = x->valid;
        int nfix = x->nfix;
        int r = insn(x, &o);
        if (!r || e->bad) {                                  // undone: called out, or the block ends here
            e->p = mark;
            e->bad = 0;
            x->pend = pend;
            x->valid = valid;
            x->nfix = nfix;
            if (clen && callable(cop, creg)) {
                callout(x, ip + clen, cop);
                if (!e->bad) {
                    at += clen;
                    ip += clen;
                    x->insn++;
                    continue;
                }
                e->p = mark;
                e->bad = 0;
            }
            *stop_op = o.op & 0xFF;
            break;
        }
        at += o.len;
        ip += o.len;
        x->insn++;
        if (r == 2) ended = 1;
    }
    if (!x->insn) return 0;
    if (!ended) {                                            // fell off the end: on at the next EIP
        spill(x);
        exit_imm(x, ip, x->insn);
    }

    // The exits: one per instruction that has one, each leaving with the EIP and count before it.
    uint32_t stub[JIT_MAX_INSNS];
    for (int i = 0; i < JIT_MAX_INSNS; i++) stub[i] = 0;
    for (int i = 0; i < x->nout; i++) {
        uint32_t n = x->out[i].insn;
        patch32(e, x->out[i].at, pos(e));
        b1(e, 0x83); b1(e, 0xF8); b1(e, 2);                  // cmp eax, 2: a fault?
        b1(e, 0xB8); b4(e, n + 1);                            // mov eax, n + 1 (flags kept)
        b1(e, 0x75); b1(e, 5);                                // jne: done, this one counted
        b1(e, 0xB8); b4(e, n);                                // mov eax, n: it faulted
        b1(e, 0xE9); b4(e, x->epi - (pos(e) + 4));            // jmp epilogue -- c->ip is the interpreter's
    }
    for (int i = 0; i < x->nfix; i++) {
        uint32_t n = x->fix[i].insn;
        if (!stub[n]) {
            stub[n] = pos(e);
            exit_imm(x, x->ips[n], n);
        }
        patch32(e, x->fix[i].at, stub[n]);
    }
    if (e->bad) return 0;

    b->ninsn = x->insn;
    b->len = at;
    b->cs = c->sreg[S_CS];
    b->ds = c->sreg[S_DS];
    b->ss = c->sreg[S_SS];
    b->cs_d = *cs;
    if (cpu16_seg(c, S_DS)) b->ds_d = *cpu16_seg(c, S_DS);
    if (cpu16_seg(c, S_SS)) b->ss_d = *cpu16_seg(c, S_SS);

    b->entry = entry;                                        // the epilogue is in front of it
    return pos(e);
}
