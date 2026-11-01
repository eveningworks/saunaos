// cpu16.c -- the 16-bit x86 interpreter (cpu16.h). One instruction per
// step(): prefixes, then the opcode switch. Flags are computed eagerly.
// Every memory access goes through mem(), which checks the selector and
// the limit; a failure sets the fault and hands back a scratch buffer,
// and the run loop stops before the next instruction, so nothing after
// a fault can reach host memory.
//
// An operand's SIZE is `w`: 0 a byte, 1 a word, 2 a dword -- what a word
// becomes under the 386's 66 prefix. Values travel as uint32_t, masked to
// their size by whoever stores them. The segments stay 16-bit (the B and
// D bits clear, as Windows 3.1 made a program's): the stack is SP, IP is
// 16 bits, and a 67-prefixed 32-bit address past a segment's limit is a
// fault like any other.
#include "cpu16.h"

#include <stdio.h>
#include <string.h>

// What one instruction is decoding: its prefixes and where it began.
struct insn {
    int seg;            // segment override, or -1
    int rep;            // 0, 0xF2 (REPNE) or 0xF3 (REP/REPE)
    int o32, a32;       // 66 and 67: 32-bit operands, 32-bit addresses
    uint16_t cs, ip;    // the instruction's own address, for faults
};

struct modrm {
    int mod, reg, rm;
    int seg;            // the effective segment of a memory operand
    uint32_t off;       // 16 bits unless the address size is 32
};

static uint8_t g_dummy[8];

void cpu16_set_fault(struct cpu16 *c, int kind, const char *why) {
    if (c->fault) return;
    c->fault = kind;
    memcpy(c->fault_r, c->r, sizeof c->fault_r);
    memcpy(c->fault_sreg, c->sreg, sizeof c->fault_sreg);
    snprintf(c->fault_why, sizeof c->fault_why, "%s", why);
}

const char *cpu16_fault_name(int kind) {
    switch (kind) {
    case FAULT_GP:    return "general protection fault";
    case FAULT_SS:    return "stack fault";
    case FAULT_UD:    return "unimplemented instruction";
    case FAULT_DE:    return "divide error";
    case FAULT_INT:   return "unhandled interrupt";
    case FAULT_HLT:   return "privileged instruction";
    case FAULT_API:   return "Windows call failed";
    case FAULT_LIMIT: return "step budget exhausted";
    default:          return "no fault";
    }
}

uint16_t cpu16_alloc(struct cpu16 *c, uint8_t kind, uint8_t *base, uint32_t limit, uint16_t owner) {
    for (unsigned i = 1; i < CPU16_MAX_DESC; i++)
        if (c->desc[i].kind == D_FREE) {
            c->desc[i].kind = kind;
            c->desc[i].base = base;
            c->desc[i].limit = limit;
            c->desc[i].owner = owner;
            return cpu16_sel(i);
        }
    return 0;
}

uint16_t cpu16_alloc_run(struct cpu16 *c, unsigned n, uint8_t kind, uint8_t *base, uint32_t size, uint16_t owner,
                         uint16_t tail_owner) {
    unsigned run = 0;
    for (unsigned i = 1; i < CPU16_MAX_DESC; i++) {
        run = c->desc[i].kind == D_FREE ? run + 1 : 0;
        if (run < n) continue;
        unsigned first = i + 1 - n;
        for (unsigned k = 0; k < n; k++) {
            struct cpu16_desc *d = &c->desc[first + k];
            d->kind = kind;
            d->base = base + (size_t)k * 0x10000;
            d->limit = size - (uint32_t)k * 0x10000 - 1;   // to the block's end (cpu16.h)
            d->owner = k ? tail_owner : owner;
        }
        return cpu16_sel(first);
    }
    return 0;
}

static const struct cpu16_desc *desc_of(struct cpu16 *c, uint16_t sel) {
    unsigned i = cpu16_index(sel);
    if (i == 0 || i >= CPU16_MAX_DESC || (sel & 7) != 7 || c->desc[i].kind == D_FREE) return 0;
    return &c->desc[i];
}

// A checked pointer to `size` bytes at sreg:off. On failure: the fault
// is set and a scratch buffer comes back, so the caller need not check.
static uint8_t *mem(struct cpu16 *c, int s, uint32_t off, unsigned size, int write) {
    const struct cpu16_desc *d = desc_of(c, c->sreg[s]);
    int kind = s == S_SS ? FAULT_SS : FAULT_GP;
    if (!d || !d->base) {
        cpu16_set_fault(c, kind, "memory access through a null or unusable selector");
        return g_dummy;
    }
    if ((uint64_t)off + size - 1 > d->limit) {
        char why[96];
        snprintf(why, sizeof why, "offset %04x+%u past the segment limit %04x", (unsigned)off, size,
                 (unsigned)d->limit);
        cpu16_set_fault(c, kind, why);
        return g_dummy;
    }
    if (write && d->kind != D_DATA) {
        cpu16_set_fault(c, kind, "write to a code segment");
        return g_dummy;
    }
    if (d->kind != D_DATA && d->kind != D_CODE) {
        cpu16_set_fault(c, kind, "data access through a call gate selector");
        return g_dummy;
    }
    return d->base + off;
}

uint8_t *cpu16_ptr(struct cpu16 *c, uint16_t sel, uint16_t off, uint32_t len, int write) {
    const struct cpu16_desc *d = desc_of(c, sel);
    if (!d || !d->base || (d->kind != D_DATA && d->kind != D_CODE) || (write && d->kind != D_DATA)
        || len == 0 || (uint64_t)off + len - 1 > d->limit) {   // a huge block's limit reaches its end
        cpu16_set_fault(c, FAULT_API, "a Windows call was passed a bad pointer");
        return 0;
    }
    return d->base + off;
}

int cpu16_str(struct cpu16 *c, uint16_t sel, uint16_t off, char *out, unsigned cap) {
    const struct cpu16_desc *d = desc_of(c, sel);
    if (!d || !d->base || (d->kind != D_DATA && d->kind != D_CODE)) return -1;
    unsigned n = 0;
    for (uint32_t o = off; o <= d->limit; o++) {
        char ch = (char)d->base[o];
        if (!ch) { out[n] = 0; return (int)n; }
        if (n + 1 < cap) out[n++] = ch;
    }
    return -1;    // runs off the end of its segment
}

static uint8_t rd8(struct cpu16 *c, int s, uint32_t off) { return *mem(c, s, off, 1, 0); }
static uint16_t rd16(struct cpu16 *c, int s, uint32_t off) {
    uint8_t *p = mem(c, s, off, 2, 0);
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t rd32(struct cpu16 *c, int s, uint32_t off) {
    uint8_t *p = mem(c, s, off, 4, 0);
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr8(struct cpu16 *c, int s, uint32_t off, uint8_t v) { *mem(c, s, off, 1, 1) = v; }
static void wr16(struct cpu16 *c, int s, uint32_t off, uint16_t v) {
    uint8_t *p = mem(c, s, off, 2, 1);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(struct cpu16 *c, int s, uint32_t off, uint32_t v) {
    uint8_t *p = mem(c, s, off, 4, 1);
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i);
}
static uint32_t rdw(struct cpu16 *c, int s, uint32_t off, int w) {
    return w == 2 ? rd32(c, s, off) : w ? rd16(c, s, off) : rd8(c, s, off);
}
static void wrw(struct cpu16 *c, int s, uint32_t off, int w, uint32_t v) {
    if (w == 2) wr32(c, s, off, v);
    else if (w) wr16(c, s, off, (uint16_t)v);
    else wr8(c, s, off, (uint8_t)v);
}

void cpu16_push(struct cpu16 *c, uint16_t v) {
    c->r[R_SP] -= 2;
    wr16(c, S_SS, c->r[R_SP], v);
}

uint16_t cpu16_pop(struct cpu16 *c) {
    uint16_t v = rd16(c, S_SS, c->r[R_SP]);
    c->r[R_SP] += 2;
    return v;
}

static void push32(struct cpu16 *c, uint32_t v) {
    c->r[R_SP] -= 4;
    wr32(c, S_SS, c->r[R_SP], v);
}

static uint32_t pop32(struct cpu16 *c) {
    uint32_t v = rd32(c, S_SS, c->r[R_SP]);
    c->r[R_SP] += 4;
    return v;
}

// A push or pop of an operand's size: a word, or a dword under 66.
static void pushw(struct cpu16 *c, int w, uint32_t v) {
    if (w == 2) push32(c, v);
    else cpu16_push(c, (uint16_t)v);
}

// A segment register pushed under 66 takes four bytes of stack and writes
// two, the slot's high word left as it was -- what the SDM lets a CPU do
// and what recent ones do.
static void push_sreg(struct cpu16 *c, int w, uint16_t sel) {
    if (w == 2) {
        c->r[R_SP] -= 4;
        wr16(c, S_SS, c->r[R_SP], sel);
    } else {
        cpu16_push(c, sel);
    }
}
static uint32_t popw(struct cpu16 *c, int w) { return w == 2 ? pop32(c) : cpu16_pop(c); }

uint16_t cpu16_arg(struct cpu16 *c, unsigned byte_off) {
    return rd16(c, S_SS, (uint16_t)(c->r[R_SP] + 4 + byte_off));
}

static uint8_t fetch8(struct cpu16 *c) {
    uint8_t v = rd8(c, S_CS, c->ip);
    c->ip++;
    return v;
}

static uint16_t fetch16(struct cpu16 *c) {
    uint16_t v = rd16(c, S_CS, c->ip);
    c->ip += 2;
    return v;
}

static uint32_t fetch32(struct cpu16 *c) {
    uint32_t v = rd32(c, S_CS, c->ip);
    c->ip += 4;
    return v;
}

static uint32_t fetchw(struct cpu16 *c, int w) { return w == 2 ? fetch32(c) : w ? fetch16(c) : fetch8(c); }

int cpu16_load_sreg(struct cpu16 *c, int s, uint16_t sel) {
    if (s == S_CS) { cpu16_set_fault(c, FAULT_UD, "mov to CS"); return -1; }
    const struct cpu16_desc *d = desc_of(c, sel);
    if (cpu16_index(sel) == 0 && s != S_SS) {      // the null selector: legal until used
        c->sreg[s] = sel;
        return 0;
    }
    if (!d || (s == S_SS && d->kind != D_DATA)) {
        char why[64];
        snprintf(why, sizeof why, "loading selector %04x into a segment register", sel);
        cpu16_set_fault(c, s == S_SS ? FAULT_SS : FAULT_GP, why);
        return -1;
    }
    c->sreg[s] = sel;
    return 0;
}

// --- registers and flags ------------------------------------------------

static uint32_t wmask(int w) { return w == 2 ? 0xFFFFFFFFu : w ? 0xFFFFu : 0xFFu; }
static uint32_t wsign(int w) { return w == 2 ? 0x80000000u : w ? 0x8000u : 0x80u; }
static unsigned wbits(int w) { return w == 2 ? 32 : w ? 16 : 8; }
// An instruction's operand size from its W bit: a byte, else a word or (66) a dword.
static int osz(const struct insn *in, int wbit) { return wbit ? 1 + in->o32 : 0; }

static uint8_t get8(struct cpu16 *c, int r) {
    return r < 4 ? (uint8_t)c->r[r] : (uint8_t)(c->r[r - 4] >> 8);
}

static void set8(struct cpu16 *c, int r, uint8_t v) {
    if (r < 4) c->r[r] = (uint16_t)((c->r[r] & 0xFF00) | v);
    else c->r[r - 4] = (uint16_t)((c->r[r - 4] & 0x00FF) | v << 8);
}

static uint32_t get32(struct cpu16 *c, int r) { return (uint32_t)c->rh[r] << 16 | c->r[r]; }
static void set32(struct cpu16 *c, int r, uint32_t v) {
    c->r[r] = (uint16_t)v;
    c->rh[r] = (uint16_t)(v >> 16);
}

static uint32_t reg_get(struct cpu16 *c, int r, int w) { return w == 2 ? get32(c, r) : w ? c->r[r] : get8(c, r); }
static void reg_set(struct cpu16 *c, int r, int w, uint32_t v) {
    if (w == 2) set32(c, r, v);
    else if (w) c->r[r] = (uint16_t)v;
    else set8(c, r, (uint8_t)v);
}

static int parity(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return !(v & 1);
}

static void setf(struct cpu16 *c, uint16_t f, int on) {
    if (on) c->flags |= f;
    else c->flags &= (uint16_t)~f;
}

static void szp(struct cpu16 *c, uint32_t v, int w) {
    setf(c, F_ZF, (v & wmask(w)) == 0);
    setf(c, F_SF, (v & wsign(w)) != 0);
    setf(c, F_PF, parity((uint8_t)v));
}

// The eight ALU ops of 00-3F and group 1: add or adc sbb and sub xor cmp.
static uint32_t alu(struct cpu16 *c, int op, uint32_t a, uint32_t b, int w) {
    uint32_t mask = wmask(w), sign = wsign(w);
    uint64_t r;
    uint32_t cf = c->flags & F_CF ? 1 : 0;
    a &= mask;
    b &= mask;
    switch (op) {
    case 0: case 2:                                       // add, adc
        if (op == 0) cf = 0;
        r = (uint64_t)a + b + cf;
        setf(c, F_CF, r > mask);
        setf(c, F_OF, ((a ^ (uint32_t)r) & (b ^ (uint32_t)r) & sign) != 0);
        setf(c, F_AF, ((a ^ b ^ (uint32_t)r) & 0x10) != 0);
        break;
    case 3: case 5: case 7:                               // sbb, sub, cmp
        if (op != 3) cf = 0;
        r = (uint64_t)a - b - cf;
        setf(c, F_CF, (uint64_t)a < (uint64_t)b + cf);
        setf(c, F_OF, ((a ^ b) & (a ^ (uint32_t)r) & sign) != 0);
        setf(c, F_AF, ((a ^ b ^ (uint32_t)r) & 0x10) != 0);
        break;
    default:                                              // or, and, xor
        r = op == 1 ? (a | b) : op == 4 ? (a & b) : (a ^ b);
        setf(c, F_CF | F_OF | F_AF, 0);
        break;
    }
    uint32_t v = (uint32_t)r & mask;
    szp(c, v, w);
    return v;
}

static uint32_t incdec(struct cpu16 *c, uint32_t a, int dec, int w) {
    uint16_t keep = c->flags & F_CF;
    uint32_t r = alu(c, dec ? 5 : 0, a, 1, w);
    c->flags = (uint16_t)((c->flags & ~F_CF) | keep);
    return r;
}

// Group 2: rol ror rcl rcr shl shr sal sar, by `count` (already masked).
static uint32_t shift(struct cpu16 *c, int op, uint32_t a, unsigned count, int w) {
    unsigned bits = wbits(w);
    uint64_t mask = wmask(w), sign = wsign(w);
    uint64_t v = a & mask;
    if (count == 0) return (uint32_t)v;
    int cf = c->flags & F_CF ? 1 : 0;
    switch (op) {
    case 0:                                               // rol
        for (unsigned i = 0; i < count; i++) { cf = (v & sign) != 0; v = ((v << 1) | (uint64_t)cf) & mask; }
        setf(c, F_CF, cf);
        setf(c, F_OF, ((v & sign) != 0) ^ cf);
        return (uint32_t)v;
    case 1:                                               // ror
        for (unsigned i = 0; i < count; i++) { cf = v & 1; v = (v >> 1) | (cf ? sign : 0); }
        setf(c, F_CF, cf);
        setf(c, F_OF, ((v ^ (v << 1)) & sign) != 0);
        return (uint32_t)v;
    case 2:                                               // rcl
        for (unsigned i = 0; i < count % (bits + 1); i++) {
            int out = (v & sign) != 0;
            v = ((v << 1) | (uint64_t)cf) & mask;
            cf = out;
        }
        setf(c, F_CF, cf);
        setf(c, F_OF, ((v & sign) != 0) ^ cf);
        return (uint32_t)v;
    case 3:                                               // rcr
        setf(c, F_OF, ((v & sign) != 0) ^ cf);
        for (unsigned i = 0; i < count % (bits + 1); i++) {
            int out = v & 1;
            v = (v >> 1) | (cf ? sign : 0);
            cf = out;
        }
        setf(c, F_CF, cf);
        return (uint32_t)v;
    case 4: case 6:                                       // shl, sal
        cf = count <= bits ? (v >> (bits - count)) & 1 : 0;
        v = count < 64 ? (v << count) & mask : 0;
        setf(c, F_CF, cf);
        setf(c, F_OF, ((v & sign) != 0) ^ cf);
        break;
    case 5:                                               // shr
        cf = count <= bits ? (v >> (count - 1)) & 1 : 0;
        setf(c, F_OF, (a & sign) != 0);
        v = count < 64 ? v >> count : 0;
        setf(c, F_CF, cf);
        break;
    default: {                                            // sar
        int64_t sv = w == 2 ? (int32_t)v : w ? (int16_t)v : (int8_t)v;
        unsigned n = count > bits ? bits : count;
        cf = (sv >> (n - 1)) & 1;
        sv >>= n;
        v = (uint64_t)sv & mask;
        setf(c, F_CF, cf);
        setf(c, F_OF, 0);
        break;
    }
    }
    setf(c, F_AF, 0);
    szp(c, (uint32_t)v, w);
    return (uint32_t)v;
}

// SHLD and SHRD: `dst` shifted by `count` (masked to 5 bits), the bits
// coming in from `src`. A word shifted past 16 is undefined on a 386.
static uint32_t dshift(struct cpu16 *c, int right, uint32_t dst, uint32_t src, unsigned count, int w) {
    unsigned bits = wbits(w);
    uint32_t mask = wmask(w);
    dst &= mask;
    src &= mask;
    if (count == 0) return dst;
    uint64_t r;
    int cf;
    if (right) {
        uint64_t cat = (uint64_t)src << bits | dst;
        r = (cat >> count) & mask;
        cf = (int)((cat >> (count - 1)) & 1);
    } else {
        uint64_t cat = (uint64_t)dst << bits | src;
        r = ((cat << count) >> bits) & mask;
        cf = (int)((cat >> (2 * bits - count)) & 1);
    }
    setf(c, F_CF, cf);
    setf(c, F_OF, ((r ^ dst) & wsign(w)) != 0);
    setf(c, F_AF, 0);
    szp(c, (uint32_t)r, w);
    return (uint32_t)r;
}

// --- operands -----------------------------------------------------------

// The 386's 32-bit addressing: [reg], [disp32], or a SIB byte's base +
// index << scale; EBP or ESP as the base means SS.
static uint32_t modrm32(struct cpu16 *c, struct modrm *m, int *bp) {
    uint32_t off;
    *bp = 0;
    if (m->rm == 4) {
        uint8_t sib = fetch8(c);
        unsigned scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
        off = index != 4 ? get32(c, (int)index) << scale : 0;
        if (base == 5 && m->mod == 0) off += fetch32(c);
        else {
            off += get32(c, (int)base);
            *bp = base == R_SP || base == R_BP;
        }
    } else if (m->rm == 5 && m->mod == 0) {
        off = fetch32(c);
    } else {
        off = get32(c, m->rm);
        *bp = m->rm == R_BP;
    }
    if (m->mod == 1) off += (uint32_t)(int32_t)(int8_t)fetch8(c);
    else if (m->mod == 2) off += fetch32(c);
    return off;
}

static void decode_modrm(struct cpu16 *c, struct insn *in, struct modrm *m) {
    uint8_t b = fetch8(c);
    m->mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    if (m->mod == 3) return;
    int bp;
    if (in->a32) {
        m->off = modrm32(c, m, &bp);
        m->seg = in->seg >= 0 ? in->seg : bp ? S_SS : S_DS;
        return;
    }
    static const int base_bp[8] = { 0, 0, 1, 1, 0, 0, 1, 0 };
    uint16_t off = 0;
    switch (m->rm) {
    case 0: off = (uint16_t)(c->r[R_BX] + c->r[R_SI]); break;
    case 1: off = (uint16_t)(c->r[R_BX] + c->r[R_DI]); break;
    case 2: off = (uint16_t)(c->r[R_BP] + c->r[R_SI]); break;
    case 3: off = (uint16_t)(c->r[R_BP] + c->r[R_DI]); break;
    case 4: off = c->r[R_SI]; break;
    case 5: off = c->r[R_DI]; break;
    case 6: off = m->mod == 0 ? 0 : c->r[R_BP]; break;
    case 7: off = c->r[R_BX]; break;
    }
    bp = base_bp[m->rm] && !(m->rm == 6 && m->mod == 0);
    if (m->mod == 0 && m->rm == 6) off = fetch16(c);
    else if (m->mod == 1) off = (uint16_t)(off + (int8_t)fetch8(c));
    else if (m->mod == 2) off = (uint16_t)(off + fetch16(c));
    m->off = off;
    m->seg = in->seg >= 0 ? in->seg : bp ? S_SS : S_DS;
}

// The operand `delta` bytes past a memory operand, wrapping as its address size does.
static uint32_t off_plus(const struct insn *in, const struct modrm *m, int32_t delta) {
    uint32_t o = m->off + (uint32_t)delta;
    return in->a32 ? o : (uint16_t)o;
}

static uint32_t rm_get(struct cpu16 *c, struct modrm *m, int w) {
    if (m->mod == 3) return reg_get(c, m->rm, w);
    return rdw(c, m->seg, m->off, w);
}

static void rm_set(struct cpu16 *c, struct modrm *m, int w, uint32_t v) {
    if (m->mod == 3) reg_set(c, m->rm, w, v);
    else wrw(c, m->seg, m->off, w, v);
}

// An index register (SI, DI, CX, BX) as the address size reads it.
static uint32_t idx_get(struct cpu16 *c, const struct insn *in, int r) { return in->a32 ? get32(c, r) : c->r[r]; }
static void idx_add(struct cpu16 *c, const struct insn *in, int r, int32_t d) {
    if (in->a32) set32(c, r, get32(c, r) + (uint32_t)d);
    else c->r[r] = (uint16_t)(c->r[r] + d);
}

// --- control transfer -----------------------------------------------------

static void far_return(struct cpu16 *c, unsigned pop);

// Transfer to SEL:OFF. A thunk runs here: its C function, then the
// Pascal return. A sentinel ends the cpu16_call_far() that pushed it.
static void far_jump(struct cpu16 *c, uint16_t sel, uint16_t off) {
    const struct cpu16_desc *d = desc_of(c, sel);
    if (!d) {
        char why[64];
        snprintf(why, sizeof why, "far transfer to bad selector %04x", sel);
        cpu16_set_fault(c, FAULT_GP, why);
        return;
    }
    if (d->kind == D_CODE) {
        if (off > d->limit) { cpu16_set_fault(c, FAULT_GP, "far transfer past the code segment"); return; }
        c->sreg[S_CS] = sel;
        c->ip = off;
        return;
    }
    if (d->kind == D_THUNK) {
        c->sreg[S_CS] = sel;
        c->ip = off;
        int n = c->thunk ? c->thunk(c, sel, off) : -1;
        if (n < 0) {
            cpu16_set_fault(c, FAULT_API, "an imported function failed");
            return;
        }
        if (!c->fault && !c->exited) far_return(c, (unsigned)n);
        return;
    }
    if (d->kind == D_SENTINEL) {
        if (off != c->depth) { cpu16_set_fault(c, FAULT_GP, "return to a stale callback frame"); return; }
        c->sreg[S_CS] = sel;
        c->ip = off;
        return;
    }
    cpu16_set_fault(c, FAULT_GP, "far transfer to a data segment");
}

// A 66-prefixed far transfer: a 32-bit offset, which in a 16-bit code
// segment must still fit in 16 bits. A Windows call is entered through a
// 16-bit frame only (its Pascal return pops one).
static void far_jump32(struct cpu16 *c, uint16_t sel, uint32_t off) {
    const struct cpu16_desc *d = desc_of(c, sel);
    if (off > 0xFFFF || (d && d->kind != D_CODE)) {
        cpu16_set_fault(c, FAULT_GP, d && d->kind == D_THUNK ? "a 32-bit far transfer into Windows"
                                                             : "32-bit far transfer past the code segment");
        return;
    }
    far_jump(c, sel, (uint16_t)off);
}

static void far_return(struct cpu16 *c, unsigned pop) {
    uint16_t ip = cpu16_pop(c);
    uint16_t cs = cpu16_pop(c);
    c->r[R_SP] = (uint16_t)(c->r[R_SP] + pop);
    if (!c->fault) far_jump(c, cs, ip);
}

static void far_return32(struct cpu16 *c, unsigned pop) {
    uint32_t ip = pop32(c);
    uint16_t cs = (uint16_t)pop32(c);
    c->r[R_SP] = (uint16_t)(c->r[R_SP] + pop);
    if (!c->fault) far_jump32(c, cs, ip);
}

static void far_call(struct cpu16 *c, struct insn *in, uint16_t sel, uint32_t off) {
    if (in->o32) {
        push32(c, c->sreg[S_CS]);
        push32(c, c->ip);
        if (!c->fault) far_jump32(c, sel, off);
        return;
    }
    cpu16_push(c, c->sreg[S_CS]);
    cpu16_push(c, c->ip);
    if (!c->fault) far_jump(c, sel, (uint16_t)off);
}

// A near transfer: a 16-bit target wraps; a 66-prefixed one past the
// segment's 64 KiB is a fault, as in a 16-bit code segment on a 386.
static void jump_near(struct cpu16 *c, const struct insn *in, uint32_t target) {
    if (in->o32 && target > 0xFFFF) { cpu16_set_fault(c, FAULT_GP, "near transfer past the code segment"); return; }
    c->ip = (uint16_t)target;
}

static void interrupt(struct cpu16 *c, uint8_t n) {
    if (!c->intr || c->intr(c, n) < 0) {
        char why[48];
        // AH names the function for the DOS and BIOS services, so a
        // missing one says which.
        snprintf(why, sizeof why, "INT %02Xh function %02Xh is not implemented", n, c->r[R_AX] >> 8);
        cpu16_set_fault(c, FAULT_INT, why);
    }
}

static int cond(struct cpu16 *c, int cc) {
    uint16_t f = c->flags;
    int r;
    switch (cc >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = (f & F_ZF) || (((f & F_SF) != 0) != ((f & F_OF) != 0)); break;
    }
    return cc & 1 ? !r : r;
}

// --- string instructions -----------------------------------------------

// SI, DI and CX are the address size's (ESI, EDI, ECX under 67).
static void string_op(struct cpu16 *c, struct insn *in, uint8_t op) {
    int w = osz(in, op & 1);
    int src = in->seg >= 0 ? in->seg : S_DS;
    int32_t step = (c->flags & F_DF ? -1 : 1) * (int32_t)(wbits(w) / 8);
    for (;;) {
        if (in->rep && idx_get(c, in, R_CX) == 0) break;
        uint32_t si = idx_get(c, in, R_SI), di = idx_get(c, in, R_DI);
        switch (op & 0xFE) {
        case 0xA4:                                        // movs
            wrw(c, S_ES, di, w, rdw(c, src, si, w));
            idx_add(c, in, R_SI, step);
            idx_add(c, in, R_DI, step);
            break;
        case 0xA6:                                        // cmps
            alu(c, 7, rdw(c, src, si, w), rdw(c, S_ES, di, w), w);
            idx_add(c, in, R_SI, step);
            idx_add(c, in, R_DI, step);
            break;
        case 0xAA:                                        // stos
            wrw(c, S_ES, di, w, reg_get(c, R_AX, w));
            idx_add(c, in, R_DI, step);
            break;
        case 0xAC:                                        // lods
            reg_set(c, R_AX, w, rdw(c, src, si, w));
            idx_add(c, in, R_SI, step);
            break;
        case 0xAE:                                        // scas
            alu(c, 7, reg_get(c, R_AX, w), rdw(c, S_ES, di, w), w);
            idx_add(c, in, R_DI, step);
            break;
        }
        if (c->fault || !in->rep) break;
        idx_add(c, in, R_CX, -1);
        if ((op & 0xFE) == 0xA6 || (op & 0xFE) == 0xAE) {
            int zf = (c->flags & F_ZF) != 0;
            if ((in->rep == 0xF3 && !zf) || (in->rep == 0xF2 && zf)) break;
        }
    }
}

// --- group 3: test not neg mul imul div idiv ------------------------------

static void group3(struct cpu16 *c, struct modrm *m, int w) {
    uint32_t v = rm_get(c, m, w), mask = wmask(w);
    switch (m->reg) {
    case 0: case 1:
        alu(c, 4, v, fetchw(c, w), w);
        break;
    case 2:
        rm_set(c, m, w, ~v & mask);
        break;
    case 3: {
        uint32_t r = alu(c, 5, 0, v, w);
        setf(c, F_CF, (v & mask) != 0);
        rm_set(c, m, w, r);
        break;
    }
    case 4:                                               // mul
        if (w == 2) {
            uint64_t r = (uint64_t)get32(c, R_AX) * v;
            set32(c, R_AX, (uint32_t)r);
            set32(c, R_DX, (uint32_t)(r >> 32));
            setf(c, F_CF | F_OF, (r >> 32) != 0);
        } else if (w) {
            uint32_t r = (uint32_t)c->r[R_AX] * (uint16_t)v;
            c->r[R_AX] = (uint16_t)r;
            c->r[R_DX] = (uint16_t)(r >> 16);
            setf(c, F_CF | F_OF, c->r[R_DX] != 0);
        } else {
            c->r[R_AX] = (uint16_t)((uint8_t)c->r[R_AX] * (uint8_t)v);
            setf(c, F_CF | F_OF, (c->r[R_AX] >> 8) != 0);
        }
        szp(c, reg_get(c, R_AX, w), w);
        break;
    case 5:                                               // imul
        if (w == 2) {
            int64_t r = (int64_t)(int32_t)get32(c, R_AX) * (int32_t)v;
            set32(c, R_AX, (uint32_t)r);
            set32(c, R_DX, (uint32_t)((uint64_t)r >> 32));
            setf(c, F_CF | F_OF, r != (int32_t)r);
        } else if (w) {
            int32_t r = (int32_t)(int16_t)c->r[R_AX] * (int16_t)v;
            c->r[R_AX] = (uint16_t)r;
            c->r[R_DX] = (uint16_t)((uint32_t)r >> 16);
            setf(c, F_CF | F_OF, r != (int16_t)r);
        } else {
            int16_t r = (int16_t)((int8_t)c->r[R_AX] * (int8_t)v);
            c->r[R_AX] = (uint16_t)r;
            setf(c, F_CF | F_OF, r != (int8_t)r);
        }
        szp(c, reg_get(c, R_AX, w), w);
        break;
    case 6:                                               // div
        if (w == 2) {
            uint64_t n = (uint64_t)get32(c, R_DX) << 32 | get32(c, R_AX);
            if (v == 0 || n / v > 0xFFFFFFFFu) { cpu16_set_fault(c, FAULT_DE, "divide error (div)"); return; }
            set32(c, R_AX, (uint32_t)(n / v));
            set32(c, R_DX, (uint32_t)(n % v));
        } else if (w) {
            uint32_t n = (uint32_t)c->r[R_DX] << 16 | c->r[R_AX];
            if (v == 0 || n / v > 0xFFFF) { cpu16_set_fault(c, FAULT_DE, "divide error (div)"); return; }
            c->r[R_AX] = (uint16_t)(n / v);
            c->r[R_DX] = (uint16_t)(n % v);
        } else {
            uint16_t n = c->r[R_AX];
            uint8_t d = (uint8_t)v;
            if (d == 0 || n / d > 0xFF) { cpu16_set_fault(c, FAULT_DE, "divide error (div)"); return; }
            c->r[R_AX] = (uint16_t)((n % d) << 8 | (n / d));
        }
        break;
    default:                                              // idiv
        if (w == 2) {
            int64_t n = (int64_t)((uint64_t)get32(c, R_DX) << 32 | get32(c, R_AX));
            int64_t d = (int32_t)v;
            if (d == 0 || (n == INT64_MIN && d == -1)) { cpu16_set_fault(c, FAULT_DE, "divide error (idiv)"); return; }
            int64_t q = n / d;
            if (q > INT32_MAX || q < INT32_MIN) { cpu16_set_fault(c, FAULT_DE, "divide error (idiv)"); return; }
            set32(c, R_AX, (uint32_t)q);
            set32(c, R_DX, (uint32_t)(n % d));
        } else if (w) {
            int32_t n = (int32_t)((uint32_t)c->r[R_DX] << 16 | c->r[R_AX]);
            int32_t d = (int16_t)v;
            if (d == 0 || (n == INT32_MIN && d == -1)) { cpu16_set_fault(c, FAULT_DE, "divide error (idiv)"); return; }
            int32_t q = n / d;
            if (q > 32767 || q < -32768) { cpu16_set_fault(c, FAULT_DE, "divide error (idiv)"); return; }
            c->r[R_AX] = (uint16_t)q;
            c->r[R_DX] = (uint16_t)(n % d);
        } else {
            int16_t n = (int16_t)c->r[R_AX];
            int16_t d = (int8_t)v;
            if (d == 0) { cpu16_set_fault(c, FAULT_DE, "divide error (idiv)"); return; }
            int16_t q = (int16_t)(n / d);
            if (q > 127 || q < -128) { cpu16_set_fault(c, FAULT_DE, "divide error (idiv)"); return; }
            c->r[R_AX] = (uint16_t)((uint8_t)(n % d) << 8 | (uint8_t)q);
        }
        break;
    }
}

// imul r, rm (0F AF) and imul r, rm, imm (69, 6B): the product's low half,
// CF and OF when it did not fit.
static uint32_t imul2(struct cpu16 *c, uint32_t a, uint32_t b, int w) {
    int64_t r = w == 2 ? (int64_t)(int32_t)a * (int32_t)b : (int64_t)(int16_t)a * (int16_t)b;
    int fits = w == 2 ? r == (int32_t)r : r == (int16_t)r;
    setf(c, F_CF | F_OF, !fits);
    szp(c, (uint32_t)r, w);
    return (uint32_t)r & wmask(w);
}

// --- BCD -----------------------------------------------------------------

static void bcd(struct cpu16 *c, uint8_t op) {
    uint8_t al = (uint8_t)c->r[R_AX];
    int cf = (c->flags & F_CF) != 0, af = (c->flags & F_AF) != 0;
    switch (op) {
    case 0x27: case 0x2F: {                               // daa, das
        uint8_t old = al;
        int oldcf = cf;
        cf = 0;
        if ((al & 0x0F) > 9 || af) {
            if (op == 0x27) { cf = oldcf || al > 0xF9; al = (uint8_t)(al + 6); }
            else { cf = oldcf || al < 6; al = (uint8_t)(al - 6); }
            af = 1;
        } else af = 0;
        if (old > 0x99 || oldcf) {
            al = (uint8_t)(op == 0x27 ? al + 0x60 : al - 0x60);
            cf = 1;
        }
        set8(c, 0, al);
        szp(c, al, 0);
        setf(c, F_CF, cf);
        setf(c, F_AF, af);
        break;
    }
    case 0x37: case 0x3F:                                 // aaa, aas
        if ((al & 0x0F) > 9 || af) {
            if (op == 0x37) { c->r[R_AX] = (uint16_t)(c->r[R_AX] + 0x106); }
            else { c->r[R_AX] = (uint16_t)(c->r[R_AX] - 6); set8(c, 4, (uint8_t)(get8(c, 4) - 1)); }
            af = cf = 1;
        } else af = cf = 0;
        set8(c, 0, (uint8_t)(c->r[R_AX] & 0x0F));
        setf(c, F_CF, cf);
        setf(c, F_AF, af);
        break;
    }
}

// --- one instruction -------------------------------------------------------

static void undefined(struct cpu16 *c, unsigned op) {
    char why[64];
    snprintf(why, sizeof why, "opcode %02X is not implemented", op);
    cpu16_set_fault(c, FAULT_UD, why);
}

// LES LDS LSS LFS LGS: an offset of the operand size, then a selector.
static void load_far(struct cpu16 *c, struct insn *in, struct modrm *m, int s) {
    if (m->mod == 3) { undefined(c, 0xC4); return; }
    int w = osz(in, 1);
    uint32_t off = rdw(c, m->seg, m->off, w);
    uint16_t sel = rd16(c, m->seg, off_plus(in, m, w == 2 ? 4 : 2));
    if (!c->fault && cpu16_load_sreg(c, s, sel) == 0) reg_set(c, m->reg, w, off);
}

// BT BTS BTR BTC: bit `bit` of the operand into CF, then kept, set,
// cleared or flipped. A register's bit offset into MEMORY is signed and
// may reach past the operand, a word or dword at a time.
static void bit_op(struct cpu16 *c, struct insn *in, struct modrm *m, int w, int kind, uint32_t bit, int from_reg) {
    unsigned bits = wbits(w);
    if (m->mod != 3 && from_reg) {
        int32_t sb = w == 2 ? (int32_t)bit : (int16_t)bit;
        int32_t units = sb >= 0 ? sb / (int32_t)bits : -((-sb + (int32_t)bits - 1) / (int32_t)bits);
        m->off = off_plus(in, m, units * (int32_t)(bits / 8));
    }
    bit &= bits - 1;
    uint32_t v = rm_get(c, m, w);
    setf(c, F_CF, (v >> bit) & 1);
    if (kind == 0) return;
    uint32_t b = 1u << bit;
    rm_set(c, m, w, kind == 1 ? v | b : kind == 2 ? v & ~b : v ^ b);
}

static void op_0f(struct cpu16 *c, struct insn *in) {
    uint8_t op = fetch8(c);
    struct modrm m;
    int w = osz(in, 1);
    if (op >= 0x80 && op <= 0x8F) {                       // jcc near
        uint32_t d = in->o32 ? fetch32(c) : (uint32_t)(int32_t)(int16_t)fetch16(c);
        if (cond(c, op & 0x0F)) jump_near(c, in, c->ip + d);
        return;
    }
    if (op >= 0x90 && op <= 0x9F) {                       // setcc
        decode_modrm(c, in, &m);
        rm_set(c, &m, 0, (uint32_t)cond(c, op & 0x0F));
        return;
    }
    // dispatch-ok: one case per 0F opcode, a set the 386 fixed in 1985.
    switch (op) {
    case 0x00:                                            // sldt str lldt ltr verr verw
        decode_modrm(c, in, &m);
        if (m.reg == 4 || m.reg == 5) {
            const struct cpu16_desc *d = desc_of(c, (uint16_t)rm_get(c, &m, 1));
            int ok = d && (d->kind == D_DATA || (m.reg == 4 && d->kind == D_CODE));
            setf(c, F_ZF, ok);
            return;
        }
        cpu16_set_fault(c, FAULT_HLT, "system-table instruction (0F 00)");
        return;
    case 0x01:                                            // sgdt sidt lgdt lidt smsw lmsw
        decode_modrm(c, in, &m);
        if (m.reg == 4) {                                 // smsw: Open Watcom's C start-up asks
            rm_set(c, &m, 1, 0x0013);                     // PE MP ET, as 3.11 answers (reference VM)
            return;
        }
        cpu16_set_fault(c, FAULT_HLT, "system-table instruction (0F 01)");
        return;
    case 0x02: case 0x03: {                               // lar, lsl
        decode_modrm(c, in, &m);
        const struct cpu16_desc *d = desc_of(c, (uint16_t)rm_get(c, &m, 1));
        if (!d || (d->kind != D_DATA && d->kind != D_CODE)) { setf(c, F_ZF, 0); return; }
        setf(c, F_ZF, 1);
        if (op == 0x03) reg_set(c, m.reg, w, d->limit);
        else reg_set(c, m.reg, w, d->kind == D_CODE ? 0xFA00 : 0xF200);   // present, DPL 3, code/data
        return;
    }
    case 0x06: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x26:
        cpu16_set_fault(c, FAULT_HLT, "control or debug register access");
        return;
    case 0xA0: case 0xA8:                                 // push fs, push gs
        push_sreg(c, w, c->sreg[op == 0xA0 ? S_FS : S_GS]);
        return;
    case 0xA1: case 0xA9:                                 // pop fs, pop gs
        cpu16_load_sreg(c, op == 0xA1 ? S_FS : S_GS, (uint16_t)popw(c, w));
        return;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB:           // bt bts btr btc r/m, r
        decode_modrm(c, in, &m);
        bit_op(c, in, &m, w, (op >> 3) & 3, reg_get(c, m.reg, w), 1);
        return;
    case 0xBA:                                            // bt bts btr btc r/m, imm8
        decode_modrm(c, in, &m);
        if (m.reg < 4) { undefined(c, 0x0FBA); return; }
        bit_op(c, in, &m, w, m.reg - 4, fetch8(c), 0);
        return;
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {         // shld, shrd
        decode_modrm(c, in, &m);
        uint32_t v = rm_get(c, &m, w);
        unsigned n = (op & 1 ? (uint8_t)c->r[R_CX] : fetch8(c)) & 0x1F;
        rm_set(c, &m, w, dshift(c, op >= 0xAC, v, reg_get(c, m.reg, w), n, w));
        return;
    }
    case 0xAF:                                            // imul r, r/m
        decode_modrm(c, in, &m);
        reg_set(c, m.reg, w, imul2(c, reg_get(c, m.reg, w), rm_get(c, &m, w), w));
        return;
    case 0xB2: case 0xB4: case 0xB5:                      // lss, lfs, lgs
        decode_modrm(c, in, &m);
        load_far(c, in, &m, op == 0xB2 ? S_SS : op == 0xB4 ? S_FS : S_GS);
        return;
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {         // movzx, movsx
        decode_modrm(c, in, &m);
        int sw = op & 1;                                  // the source: a byte or a word
        uint32_t v = rm_get(c, &m, sw);
        if (op >= 0xBE) v = sw ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v;
        reg_set(c, m.reg, w, v);
        return;
    }
    case 0xBC: case 0xBD: {                               // bsf, bsr
        decode_modrm(c, in, &m);
        uint32_t v = rm_get(c, &m, w);
        setf(c, F_ZF, v == 0);
        if (!v) return;                                   // the destination is left as it was
        unsigned i = 0;
        if (op == 0xBC) while (!((v >> i) & 1)) i++;
        else { i = wbits(w) - 1; while (!((v >> i) & 1)) i--; }
        reg_set(c, m.reg, w, i);
        return;
    }
    default: {
        char why[64];
        snprintf(why, sizeof why, "opcode 0F %02X is not implemented", op);
        cpu16_set_fault(c, FAULT_UD, why);
        return;
    }
    }
}

static void step(struct cpu16 *c) {
    struct insn in = { .seg = -1, .cs = c->sreg[S_CS], .ip = c->ip };
    struct modrm m;
    uint8_t op;
    for (;;) {
        op = fetch8(c);
        if (c->fault) return;
        if (op == 0x26) in.seg = S_ES;
        else if (op == 0x2E) in.seg = S_CS;
        else if (op == 0x36) in.seg = S_SS;
        else if (op == 0x3E) in.seg = S_DS;
        else if (op == 0x64) in.seg = S_FS;
        else if (op == 0x65) in.seg = S_GS;
        else if (op == 0x66) in.o32 = 1;
        else if (op == 0x67) in.a32 = 1;
        else if (op == 0xF2 || op == 0xF3) in.rep = op;
        else if (op == 0xF0) continue;
        else break;
    }
    c->fault_cs = in.cs;
    c->fault_ip = in.ip;

    if (op < 0x40 && (op & 7) < 6) {                      // the ALU block
        int alu_op = op >> 3, w = osz(&in, op & 1);
        uint32_t r;
        switch (op & 7) {
        case 0: case 1:
            decode_modrm(c, &in, &m);
            r = alu(c, alu_op, rm_get(c, &m, w), reg_get(c, m.reg, w), w);
            if (alu_op != 7) rm_set(c, &m, w, r);
            return;
        case 2: case 3:
            decode_modrm(c, &in, &m);
            r = alu(c, alu_op, reg_get(c, m.reg, w), rm_get(c, &m, w), w);
            if (alu_op != 7) reg_set(c, m.reg, w, r);
            return;
        default: {
            uint32_t imm = fetchw(c, w);
            r = alu(c, alu_op, reg_get(c, R_AX, w), imm, w);
            if (alu_op != 7) reg_set(c, R_AX, w, r);
            return;
        }
        }
    }

    int wd = osz(&in, 1);                                 // a word or dword operand's size
    // dispatch-ok: one case per x86 opcode, a set the 386 fixed in 1985.
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        push_sreg(c, wd, c->sreg[op >> 3]);
        return;
    case 0x07: case 0x17: case 0x1F:
        cpu16_load_sreg(c, op >> 3, (uint16_t)popw(c, wd));
        return;
    case 0x0F: op_0f(c, &in); return;
    case 0x27: case 0x2F: case 0x37: case 0x3F: bcd(c, op); return;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        reg_set(c, op & 7, wd, incdec(c, reg_get(c, op & 7, wd), 0, wd));
        return;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        reg_set(c, op & 7, wd, incdec(c, reg_get(c, op & 7, wd), 1, wd));
        return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        uint32_t v = reg_get(c, op & 7, wd);              // push sp pushes the old value (286)
        pushw(c, wd, v);
        return;
    }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint32_t v = popw(c, wd);
        reg_set(c, op & 7, wd, v);
        return;
    }
    case 0x60: {                                          // pusha, pushad
        uint32_t sp = reg_get(c, R_SP, wd);
        for (int i = 0; i < 8; i++) pushw(c, wd, i == R_SP ? sp : reg_get(c, i, wd));
        return;
    }
    case 0x61:                                            // popa, popad
        for (int i = 7; i >= 0; i--) {
            uint32_t v = popw(c, wd);
            if (i != R_SP) reg_set(c, i, wd, v);
        }
        return;
    case 0x62: {                                          // bound
        decode_modrm(c, &in, &m);
        if (m.mod == 3) { undefined(c, op); return; }
        int32_t v, lo, hi;
        if (wd == 2) {
            v = (int32_t)get32(c, m.reg);
            lo = (int32_t)rd32(c, m.seg, m.off);
            hi = (int32_t)rd32(c, m.seg, off_plus(&in, &m, 4));
        } else {
            v = (int16_t)c->r[m.reg];
            lo = (int16_t)rd16(c, m.seg, m.off);
            hi = (int16_t)rd16(c, m.seg, off_plus(&in, &m, 2));
        }
        if (!c->fault && (v < lo || v > hi)) interrupt(c, 5);
        return;
    }
    case 0x63:                                            // arpl
        decode_modrm(c, &in, &m);
        setf(c, F_ZF, 0);
        return;
    case 0x68: pushw(c, wd, fetchw(c, wd)); return;
    case 0x6A: pushw(c, wd, (uint32_t)(int32_t)(int8_t)fetch8(c)); return;
    case 0x69: case 0x6B: {                               // imul r, rm, imm
        decode_modrm(c, &in, &m);
        uint32_t a = rm_get(c, &m, wd);
        uint32_t b = op == 0x69 ? fetchw(c, wd) : (uint32_t)(int32_t)(int8_t)fetch8(c);
        reg_set(c, m.reg, wd, imul2(c, a, b, wd));
        return;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF:
        cpu16_set_fault(c, FAULT_HLT, "port I/O");
        return;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)fetch8(c);
        if (cond(c, op & 0x0F)) jump_near(c, &in, (uint32_t)(c->ip + d));
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int w = osz(&in, op & 1);
        decode_modrm(c, &in, &m);
        uint32_t v = rm_get(c, &m, w);
        uint32_t imm = op == 0x81 ? fetchw(c, w) : op == 0x83 ? (uint32_t)(int32_t)(int8_t)fetch8(c) : fetch8(c);
        uint32_t r = alu(c, m.reg, v, imm, w);
        if (m.reg != 7) rm_set(c, &m, w, r);
        return;
    }
    case 0x84: case 0x85: {
        int w = osz(&in, op & 1);
        decode_modrm(c, &in, &m);
        alu(c, 4, rm_get(c, &m, w), reg_get(c, m.reg, w), w);
        return;
    }
    case 0x86: case 0x87: {
        int w = osz(&in, op & 1);
        decode_modrm(c, &in, &m);
        uint32_t a = rm_get(c, &m, w), b = reg_get(c, m.reg, w);
        rm_set(c, &m, w, b);
        reg_set(c, m.reg, w, a);
        return;
    }
    case 0x88: case 0x89:
        decode_modrm(c, &in, &m);
        rm_set(c, &m, osz(&in, op & 1), reg_get(c, m.reg, osz(&in, op & 1)));
        return;
    case 0x8A: case 0x8B:
        decode_modrm(c, &in, &m);
        reg_set(c, m.reg, osz(&in, op & 1), rm_get(c, &m, osz(&in, op & 1)));
        return;
    case 0x8C:                                            // a register takes it zero-extended
        decode_modrm(c, &in, &m);
        if (m.reg >= CPU16_SREGS) { undefined(c, op); return; }
        rm_set(c, &m, m.mod == 3 ? wd : 1, c->sreg[m.reg]);
        return;
    case 0x8D:
        decode_modrm(c, &in, &m);
        if (m.mod == 3) { undefined(c, op); return; }
        reg_set(c, m.reg, wd, m.off);
        return;
    case 0x8E:
        decode_modrm(c, &in, &m);
        if (m.reg >= CPU16_SREGS) { undefined(c, op); return; }
        cpu16_load_sreg(c, m.reg, (uint16_t)rm_get(c, &m, 1));
        return;
    case 0x8F: {
        decode_modrm(c, &in, &m);
        uint32_t v = popw(c, wd);
        rm_set(c, &m, wd, v);
        return;
    }
    case 0x90: return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint32_t t = reg_get(c, R_AX, wd);
        reg_set(c, R_AX, wd, reg_get(c, op & 7, wd));
        reg_set(c, op & 7, wd, t);
        return;
    }
    case 0x98:                                            // cbw, cwde
        if (wd == 2) set32(c, R_AX, (uint32_t)(int32_t)(int16_t)c->r[R_AX]);
        else c->r[R_AX] = (uint16_t)(int8_t)c->r[R_AX];
        return;
    case 0x99:                                            // cwd, cdq
        reg_set(c, R_DX, wd, reg_get(c, R_AX, wd) & wsign(wd) ? 0xFFFFFFFFu : 0);
        return;
    case 0x9A: {
        uint32_t off = fetchw(c, wd);
        uint16_t sel = fetch16(c);
        far_call(c, &in, sel, off);
        return;
    }
    case 0x9B:                                            // wait
        if (cpu16_x87_pending(c)) {
            c->ip = in.ip;
            interrupt(c, 0x75);
        }
        return;
    case 0x9C: pushw(c, wd, (uint16_t)((c->flags & 0x0FD5) | 0x0002)); return;
    case 0x9D: c->flags = (uint16_t)((popw(c, wd) & 0x0FD5) | 0x0002); return;
    case 0x9E: c->flags = (uint16_t)((c->flags & 0xFF00) | (get8(c, 4) & 0xD5) | 0x02); return;
    case 0x9F: set8(c, 4, (uint8_t)((c->flags & 0xD5) | 0x02)); return;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {         // mov to and from moffs
        uint32_t off = in.a32 ? fetch32(c) : fetch16(c);
        int s = in.seg >= 0 ? in.seg : S_DS, w = osz(&in, op & 1);
        if (op <= 0xA1) reg_set(c, R_AX, w, rdw(c, s, off, w));
        else wrw(c, s, off, w, reg_get(c, R_AX, w));
        return;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, &in, op);
        return;
    case 0xA8: alu(c, 4, c->r[R_AX], fetch8(c), 0); return;
    case 0xA9: alu(c, 4, reg_get(c, R_AX, wd), fetchw(c, wd), wd); return;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set8(c, op & 7, fetch8(c));
        return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        reg_set(c, op & 7, wd, fetchw(c, wd));
        return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int w = osz(&in, op & 1);
        decode_modrm(c, &in, &m);
        uint32_t v = rm_get(c, &m, w);
        unsigned n = op <= 0xC1 ? fetch8(c) : op <= 0xD1 ? 1 : (uint8_t)c->r[R_CX];
        n &= 0x1F;                                        // the 186's mask
        rm_set(c, &m, w, shift(c, m.reg, v, n, w));
        return;
    }
    case 0xC2: {
        uint16_t n = fetch16(c);
        jump_near(c, &in, popw(c, wd));
        c->r[R_SP] += n;
        return;
    }
    case 0xC3: jump_near(c, &in, popw(c, wd)); return;
    case 0xC4: case 0xC5:                                 // les, lds
        decode_modrm(c, &in, &m);
        load_far(c, &in, &m, op == 0xC4 ? S_ES : S_DS);
        return;
    case 0xC6: case 0xC7: {
        int w = osz(&in, op & 1);
        decode_modrm(c, &in, &m);
        rm_set(c, &m, w, fetchw(c, w));
        return;
    }
    case 0xC8: {                                          // enter
        uint16_t size = fetch16(c);
        unsigned level = fetch8(c) & 31, step_bytes = wd == 2 ? 4 : 2;
        pushw(c, wd, reg_get(c, R_BP, wd));
        uint16_t frame = c->r[R_SP];
        for (unsigned i = 1; i < level; i++) {
            c->r[R_BP] = (uint16_t)(c->r[R_BP] - step_bytes);
            pushw(c, wd, rdw(c, S_SS, c->r[R_BP], wd));
        }
        if (level) pushw(c, wd, frame);
        reg_set(c, R_BP, wd, frame);
        c->r[R_SP] -= size;
        return;
    }
    case 0xC9:                                            // leave
        c->r[R_SP] = c->r[R_BP];
        reg_set(c, R_BP, wd, popw(c, wd));
        return;
    case 0xCA:
        if (in.o32) far_return32(c, fetch16(c));
        else far_return(c, fetch16(c));
        return;
    case 0xCB:
        if (in.o32) far_return32(c, 0);
        else far_return(c, 0);
        return;
    case 0xCC: interrupt(c, 3); return;
    case 0xCD: interrupt(c, fetch8(c)); return;
    case 0xCE: if (c->flags & F_OF) interrupt(c, 4); return;
    case 0xCF: {                                          // iret, iretd
        uint32_t ip = popw(c, wd);
        uint16_t cs = (uint16_t)popw(c, wd);
        c->flags = (uint16_t)((popw(c, wd) & 0x0FD5) | 0x0002);
        if (c->fault) return;
        if (in.o32) far_jump32(c, cs, ip);
        else far_jump(c, cs, (uint16_t)ip);
        return;
    }
    case 0xD4: {                                          // aam
        uint8_t base = fetch8(c);
        if (!base) { cpu16_set_fault(c, FAULT_DE, "divide error (aam)"); return; }
        uint8_t al = (uint8_t)c->r[R_AX];
        c->r[R_AX] = (uint16_t)((al / base) << 8 | (al % base));
        szp(c, (uint8_t)c->r[R_AX], 0);
        return;
    }
    case 0xD5: {                                          // aad
        uint8_t base = fetch8(c);
        uint8_t al = (uint8_t)(get8(c, 0) + get8(c, 4) * base);
        c->r[R_AX] = al;
        szp(c, al, 0);
        return;
    }
    case 0xD6: set8(c, 0, c->flags & F_CF ? 0xFF : 0); return;     // salc
    case 0xD7: {                                          // xlat
        int s = in.seg >= 0 ? in.seg : S_DS;
        uint32_t at = idx_get(c, &in, R_BX) + (uint8_t)c->r[R_AX];
        set8(c, 0, rd8(c, s, in.a32 ? at : (uint16_t)at));
        return;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        decode_modrm(c, &in, &m);
        if (c->fault) return;
        if (m.mod != 3 && m.off > 0xFFFF) { cpu16_set_fault(c, FAULT_GP, "x87 operand past a 16-bit segment"); return; }
        uint8_t b = (uint8_t)(m.mod << 6 | m.reg << 3 | m.rm);
        if (cpu16_x87_waits(op, b) && cpu16_x87_pending(c)) {   // the coprocessor's IRQ 13, before it runs
            c->ip = in.ip;
            interrupt(c, 0x75);
            return;
        }
        cpu16_x87_op(c, op, b, m.mod == 3 ? -1 : m.seg, (uint16_t)m.off, in.cs, in.ip);
        return;
    }
    case 0xE0: case 0xE1: case 0xE2: {                    // loopnz loopz loop: CX, or ECX under 67
        int8_t d = (int8_t)fetch8(c);
        idx_add(c, &in, R_CX, -1);
        int zf = (c->flags & F_ZF) != 0;
        if (idx_get(c, &in, R_CX) && (op == 0xE2 || (op == 0xE1 ? zf : !zf)))
            jump_near(c, &in, (uint32_t)(c->ip + d));
        return;
    }
    case 0xE3: {                                          // jcxz, jecxz
        int8_t d = (int8_t)fetch8(c);
        if (!idx_get(c, &in, R_CX)) jump_near(c, &in, (uint32_t)(c->ip + d));
        return;
    }
    case 0xE8: {
        uint32_t d = wd == 2 ? fetch32(c) : (uint32_t)(int32_t)(int16_t)fetch16(c);
        pushw(c, wd, c->ip);
        jump_near(c, &in, c->ip + d);
        return;
    }
    case 0xE9: {
        uint32_t d = wd == 2 ? fetch32(c) : (uint32_t)(int32_t)(int16_t)fetch16(c);
        jump_near(c, &in, c->ip + d);
        return;
    }
    case 0xEA: {
        uint32_t off = fetchw(c, wd);
        uint16_t sel = fetch16(c);
        if (in.o32) far_jump32(c, sel, off);
        else far_jump(c, sel, (uint16_t)off);
        return;
    }
    case 0xEB: { int8_t d = (int8_t)fetch8(c); jump_near(c, &in, (uint32_t)(c->ip + d)); return; }
    case 0xF4: cpu16_set_fault(c, FAULT_HLT, "hlt"); return;
    case 0xF5: c->flags ^= F_CF; return;
    case 0xF6: case 0xF7:
        decode_modrm(c, &in, &m);
        group3(c, &m, osz(&in, op & 1));
        return;
    case 0xF8: setf(c, F_CF, 0); return;
    case 0xF9: setf(c, F_CF, 1); return;
    case 0xFA: setf(c, F_IF, 0); return;
    case 0xFB: setf(c, F_IF, 1); return;
    case 0xFC: setf(c, F_DF, 0); return;
    case 0xFD: setf(c, F_DF, 1); return;
    case 0xFE:
        decode_modrm(c, &in, &m);
        if (m.reg > 1) { undefined(c, op); return; }
        rm_set(c, &m, 0, incdec(c, rm_get(c, &m, 0), m.reg, 0));
        return;
    case 0xFF:
        decode_modrm(c, &in, &m);
        switch (m.reg) {
        case 0: case 1: rm_set(c, &m, wd, incdec(c, rm_get(c, &m, wd), m.reg, wd)); return;
        case 2: { uint32_t t = rm_get(c, &m, wd); pushw(c, wd, c->ip); jump_near(c, &in, t); return; }
        case 3: case 5: {
            if (m.mod == 3) { undefined(c, op); return; }
            uint32_t off = rdw(c, m.seg, m.off, wd);
            uint16_t sel = rd16(c, m.seg, off_plus(&in, &m, wd == 2 ? 4 : 2));
            if (c->fault) return;
            if (m.reg == 3) far_call(c, &in, sel, off);
            else if (in.o32) far_jump32(c, sel, off);
            else far_jump(c, sel, (uint16_t)off);
            return;
        }
        case 4: jump_near(c, &in, rm_get(c, &m, wd)); return;
        case 6: pushw(c, wd, rm_get(c, &m, wd)); return;
        default: undefined(c, op); return;
        }
    default:
        undefined(c, op);
        return;
    }
}

int cpu16_step(struct cpu16 *c) {
    if (c->fault) return -1;
    step(c);
    c->steps++;
    return c->fault ? -1 : 0;
}

int cpu16_run(struct cpu16 *c, uint64_t max_steps) {
    uint64_t until = max_steps ? c->steps + max_steps : 0;
    unsigned depth = c->depth;
    while (!c->fault && !c->exited) {
        const struct cpu16_desc *d = desc_of(c, c->sreg[S_CS]);
        if (d && d->kind == D_SENTINEL && c->ip == depth) return 0;
        if (until && c->steps >= until) { cpu16_set_fault(c, FAULT_LIMIT, "step budget"); break; }
        step(c);
        if (!(++c->steps & (CPU16_TICK_STEPS - 1)) && c->tick) c->tick(c);
    }
    return c->fault ? -1 : 0;
}

int cpu16_call_far(struct cpu16 *c, uint16_t sel, uint16_t off) {
    static uint16_t sentinel;
    if (!sentinel || desc_of(c, sentinel) == 0 || desc_of(c, sentinel)->kind != D_SENTINEL)
        sentinel = cpu16_alloc(c, D_SENTINEL, 0, 0xFFFF, 0);
    if (!sentinel) { cpu16_set_fault(c, FAULT_API, "no selector for a callback"); return -1; }
    uint16_t cs = c->sreg[S_CS], ip = c->ip;
    c->depth++;
    cpu16_push(c, sentinel);
    cpu16_push(c, (uint16_t)c->depth);
    far_jump(c, sel, off);
    int r = cpu16_run(c, 0);
    c->depth--;
    c->sreg[S_CS] = cs;
    c->ip = ip;
    return r;
}
