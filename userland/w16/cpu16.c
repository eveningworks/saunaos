// cpu16.c -- the 16-bit x86 interpreter (cpu16.h). One instruction per
// step(): prefixes, then the opcode switch. Flags are computed eagerly.
// Every memory access goes through mem(), which checks the selector and
// the limit; a failure sets the fault and hands back a scratch buffer,
// and the run loop stops before the next instruction, so nothing after
// a fault can reach host memory.
#include "cpu16.h"

#include <stdio.h>
#include <string.h>

// What one instruction is decoding: its prefixes and where it began.
struct insn {
    int seg;            // segment override, or -1
    int rep;            // 0, 0xF2 (REPNE) or 0xF3 (REP/REPE)
    uint16_t cs, ip;    // the instruction's own address, for faults
};

struct modrm {
    int mod, reg, rm;
    int seg;            // the effective segment of a memory operand
    uint16_t off;
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

static const struct cpu16_desc *desc_of(struct cpu16 *c, uint16_t sel) {
    unsigned i = cpu16_index(sel);
    if (i == 0 || i >= CPU16_MAX_DESC || (sel & 7) != 7 || c->desc[i].kind == D_FREE) return 0;
    return &c->desc[i];
}

// A checked pointer to `size` bytes at sreg:off. On failure: the fault
// is set and a scratch buffer comes back, so the caller need not check.
static uint8_t *mem(struct cpu16 *c, int s, uint16_t off, unsigned size, int write) {
    const struct cpu16_desc *d = desc_of(c, c->sreg[s]);
    int kind = s == S_SS ? FAULT_SS : FAULT_GP;
    if (!d || !d->base) {
        cpu16_set_fault(c, kind, "memory access through a null or unusable selector");
        return g_dummy;
    }
    if ((uint32_t)off + size - 1 > d->limit) {
        char why[96];
        snprintf(why, sizeof why, "offset %04x+%u past the segment limit %04x", off, size,
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
        || len == 0 || (uint32_t)off + len - 1 > d->limit) {
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

static uint8_t rd8(struct cpu16 *c, int s, uint16_t off) { return *mem(c, s, off, 1, 0); }
static uint16_t rd16(struct cpu16 *c, int s, uint16_t off) {
    uint8_t *p = mem(c, s, off, 2, 0);
    return (uint16_t)(p[0] | p[1] << 8);
}
static void wr8(struct cpu16 *c, int s, uint16_t off, uint8_t v) { *mem(c, s, off, 1, 1) = v; }
static void wr16(struct cpu16 *c, int s, uint16_t off, uint16_t v) {
    uint8_t *p = mem(c, s, off, 2, 1);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
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

static uint8_t get8(struct cpu16 *c, int r) {
    return r < 4 ? (uint8_t)c->r[r] : (uint8_t)(c->r[r - 4] >> 8);
}

static void set8(struct cpu16 *c, int r, uint8_t v) {
    if (r < 4) c->r[r] = (uint16_t)((c->r[r] & 0xFF00) | v);
    else c->r[r - 4] = (uint16_t)((c->r[r - 4] & 0x00FF) | v << 8);
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
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    setf(c, F_ZF, (v & mask) == 0);
    setf(c, F_SF, (v & sign) != 0);
    setf(c, F_PF, parity((uint8_t)v));
}

// The eight ALU ops of 00-3F and group 1: add or adc sbb and sub xor cmp.
static uint16_t alu(struct cpu16 *c, int op, uint16_t a, uint16_t b, int w) {
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80, r;
    uint32_t cf = c->flags & F_CF ? 1 : 0;
    switch (op) {
    case 0: case 2:                                       // add, adc
        if (op == 0) cf = 0;
        r = (uint32_t)a + b + cf;
        setf(c, F_CF, r > mask);
        setf(c, F_OF, ((a ^ r) & (b ^ r) & sign) != 0);
        setf(c, F_AF, ((a ^ b ^ r) & 0x10) != 0);
        break;
    case 3: case 5: case 7:                               // sbb, sub, cmp
        if (op != 3) cf = 0;
        r = (uint32_t)a - b - cf;
        setf(c, F_CF, (uint32_t)a < (uint32_t)b + cf);
        setf(c, F_OF, ((a ^ b) & (a ^ r) & sign) != 0);
        setf(c, F_AF, ((a ^ b ^ r) & 0x10) != 0);
        break;
    default:                                              // or, and, xor
        r = op == 1 ? (uint32_t)(a | b) : op == 4 ? (uint32_t)(a & b) : (uint32_t)(a ^ b);
        setf(c, F_CF | F_OF | F_AF, 0);
        break;
    }
    r &= mask;
    szp(c, r, w);
    return (uint16_t)r;
}

static uint16_t incdec(struct cpu16 *c, uint16_t a, int dec, int w) {
    uint16_t keep = c->flags & F_CF;
    uint16_t r = alu(c, dec ? 5 : 0, a, 1, w);
    c->flags = (uint16_t)((c->flags & ~F_CF) | keep);
    return r;
}

// Group 2: rol ror rcl rcr shl shr sal sar, by `count` (already masked).
static uint16_t shift(struct cpu16 *c, int op, uint16_t a, unsigned count, int w) {
    unsigned bits = w ? 16 : 8;
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    uint32_t v = a & mask;
    if (count == 0) return (uint16_t)v;
    int cf = c->flags & F_CF ? 1 : 0;
    switch (op) {
    case 0:                                               // rol
        for (unsigned i = 0; i < count; i++) { cf = (v & sign) != 0; v = ((v << 1) | (uint32_t)cf) & mask; }
        setf(c, F_CF, cf);
        setf(c, F_OF, ((v & sign) != 0) ^ cf);
        return (uint16_t)v;
    case 1:                                               // ror
        for (unsigned i = 0; i < count; i++) { cf = v & 1; v = (v >> 1) | (cf ? sign : 0); }
        setf(c, F_CF, cf);
        setf(c, F_OF, ((v ^ (v << 1)) & sign) != 0);
        return (uint16_t)v;
    case 2:                                               // rcl
        for (unsigned i = 0; i < count % (bits + 1); i++) {
            int out = (v & sign) != 0;
            v = ((v << 1) | (uint32_t)cf) & mask;
            cf = out;
        }
        setf(c, F_CF, cf);
        setf(c, F_OF, ((v & sign) != 0) ^ cf);
        return (uint16_t)v;
    case 3:                                               // rcr
        setf(c, F_OF, ((v & sign) != 0) ^ cf);
        for (unsigned i = 0; i < count % (bits + 1); i++) {
            int out = v & 1;
            v = (v >> 1) | (cf ? sign : 0);
            cf = out;
        }
        setf(c, F_CF, cf);
        return (uint16_t)v;
    case 4: case 6:                                       // shl, sal
        cf = count <= bits ? (v >> (bits - count)) & 1 : 0;
        v = count < 32 ? (v << count) & mask : 0;
        setf(c, F_CF, cf);
        setf(c, F_OF, ((v & sign) != 0) ^ cf);
        break;
    case 5:                                               // shr
        cf = count <= bits ? (v >> (count - 1)) & 1 : 0;
        setf(c, F_OF, (a & sign) != 0);
        v = count < 32 ? v >> count : 0;
        setf(c, F_CF, cf);
        break;
    default: {                                            // sar
        int32_t sv = w ? (int16_t)v : (int8_t)v;
        unsigned n = count > bits ? bits : count;
        cf = (sv >> (n - 1)) & 1;
        sv >>= n;
        v = (uint32_t)sv & mask;
        setf(c, F_CF, cf);
        setf(c, F_OF, 0);
        break;
    }
    }
    setf(c, F_AF, 0);
    szp(c, v, w);
    return (uint16_t)v;
}

// --- operands -----------------------------------------------------------

static void decode_modrm(struct cpu16 *c, struct insn *in, struct modrm *m) {
    uint8_t b = fetch8(c);
    m->mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    if (m->mod == 3) return;
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
    int bp = base_bp[m->rm] && !(m->rm == 6 && m->mod == 0);
    if (m->mod == 0 && m->rm == 6) off = fetch16(c);
    else if (m->mod == 1) off = (uint16_t)(off + (int8_t)fetch8(c));
    else if (m->mod == 2) off = (uint16_t)(off + fetch16(c));
    m->off = off;
    m->seg = in->seg >= 0 ? in->seg : bp ? S_SS : S_DS;
}

static uint16_t rm_get(struct cpu16 *c, struct modrm *m, int w) {
    if (m->mod == 3) return w ? c->r[m->rm] : get8(c, m->rm);
    return w ? rd16(c, m->seg, m->off) : rd8(c, m->seg, m->off);
}

static void rm_set(struct cpu16 *c, struct modrm *m, int w, uint16_t v) {
    if (m->mod == 3) {
        if (w) c->r[m->rm] = v;
        else set8(c, m->rm, (uint8_t)v);
        return;
    }
    if (w) wr16(c, m->seg, m->off, v);
    else wr8(c, m->seg, m->off, (uint8_t)v);
}

static uint16_t reg_get(struct cpu16 *c, int r, int w) { return w ? c->r[r] : get8(c, r); }
static void reg_set(struct cpu16 *c, int r, int w, uint16_t v) {
    if (w) c->r[r] = v;
    else set8(c, r, (uint8_t)v);
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

static void far_return(struct cpu16 *c, unsigned pop) {
    uint16_t ip = cpu16_pop(c);
    uint16_t cs = cpu16_pop(c);
    c->r[R_SP] = (uint16_t)(c->r[R_SP] + pop);
    if (!c->fault) far_jump(c, cs, ip);
}

static void far_call(struct cpu16 *c, uint16_t sel, uint16_t off) {
    cpu16_push(c, c->sreg[S_CS]);
    cpu16_push(c, c->ip);
    if (!c->fault) far_jump(c, sel, off);
}

static void interrupt(struct cpu16 *c, uint8_t n) {
    if (!c->intr || c->intr(c, n) < 0) {
        char why[48];
        snprintf(why, sizeof why, "INT %02Xh is not implemented", n);
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

static void string_op(struct cpu16 *c, struct insn *in, uint8_t op) {
    int w = op & 1;
    int src = in->seg >= 0 ? in->seg : S_DS;
    int16_t step = (int16_t)((c->flags & F_DF ? -1 : 1) * (w ? 2 : 1));
    for (;;) {
        if (in->rep && c->r[R_CX] == 0) break;
        switch (op & 0xFE) {
        case 0xA4:                                        // movs
            if (w) wr16(c, S_ES, c->r[R_DI], rd16(c, src, c->r[R_SI]));
            else wr8(c, S_ES, c->r[R_DI], rd8(c, src, c->r[R_SI]));
            c->r[R_SI] += step;
            c->r[R_DI] += step;
            break;
        case 0xA6:                                        // cmps
            alu(c, 7, w ? rd16(c, src, c->r[R_SI]) : rd8(c, src, c->r[R_SI]),
                w ? rd16(c, S_ES, c->r[R_DI]) : rd8(c, S_ES, c->r[R_DI]), w);
            c->r[R_SI] += step;
            c->r[R_DI] += step;
            break;
        case 0xAA:                                        // stos
            if (w) wr16(c, S_ES, c->r[R_DI], c->r[R_AX]);
            else wr8(c, S_ES, c->r[R_DI], (uint8_t)c->r[R_AX]);
            c->r[R_DI] += step;
            break;
        case 0xAC:                                        // lods
            if (w) c->r[R_AX] = rd16(c, src, c->r[R_SI]);
            else set8(c, 0, rd8(c, src, c->r[R_SI]));
            c->r[R_SI] += step;
            break;
        case 0xAE:                                        // scas
            alu(c, 7, w ? c->r[R_AX] : (uint8_t)c->r[R_AX],
                w ? rd16(c, S_ES, c->r[R_DI]) : rd8(c, S_ES, c->r[R_DI]), w);
            c->r[R_DI] += step;
            break;
        }
        if (c->fault || !in->rep) break;
        c->r[R_CX]--;
        if ((op & 0xFE) == 0xA6 || (op & 0xFE) == 0xAE) {
            int zf = (c->flags & F_ZF) != 0;
            if ((in->rep == 0xF3 && !zf) || (in->rep == 0xF2 && zf)) break;
        }
    }
}

// --- group 3: test not neg mul imul div idiv ------------------------------

static void group3(struct cpu16 *c, struct modrm *m, int w) {
    uint16_t v = rm_get(c, m, w);
    switch (m->reg) {
    case 0: case 1:
        alu(c, 4, v, w ? fetch16(c) : fetch8(c), w);
        break;
    case 2:
        rm_set(c, m, w, (uint16_t)~v);
        break;
    case 3: {
        uint16_t r = alu(c, 5, 0, v, w);
        setf(c, F_CF, (w ? v : (uint8_t)v) != 0);
        rm_set(c, m, w, r);
        break;
    }
    case 4:                                               // mul
        if (w) {
            uint32_t r = (uint32_t)c->r[R_AX] * v;
            c->r[R_AX] = (uint16_t)r;
            c->r[R_DX] = (uint16_t)(r >> 16);
            setf(c, F_CF | F_OF, c->r[R_DX] != 0);
        } else {
            c->r[R_AX] = (uint16_t)((uint8_t)c->r[R_AX] * (uint8_t)v);
            setf(c, F_CF | F_OF, (c->r[R_AX] >> 8) != 0);
        }
        szp(c, w ? c->r[R_AX] : (uint8_t)c->r[R_AX], w);
        break;
    case 5:                                               // imul
        if (w) {
            int32_t r = (int32_t)(int16_t)c->r[R_AX] * (int16_t)v;
            c->r[R_AX] = (uint16_t)r;
            c->r[R_DX] = (uint16_t)((uint32_t)r >> 16);
            setf(c, F_CF | F_OF, r != (int16_t)r);
        } else {
            int16_t r = (int16_t)((int8_t)c->r[R_AX] * (int8_t)v);
            c->r[R_AX] = (uint16_t)r;
            setf(c, F_CF | F_OF, r != (int8_t)r);
        }
        szp(c, w ? c->r[R_AX] : (uint8_t)c->r[R_AX], w);
        break;
    case 6:                                               // div
        if (w) {
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
        if (w) {
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

static void op_0f(struct cpu16 *c, struct insn *in) {
    uint8_t op = fetch8(c);
    struct modrm m;
    if (op == 0x00) {                                     // sldt str lldt ltr verr verw
        decode_modrm(c, in, &m);
        if (m.reg == 4 || m.reg == 5) {
            const struct cpu16_desc *d = desc_of(c, rm_get(c, &m, 1));
            int ok = d && (d->kind == D_DATA || (m.reg == 4 && d->kind == D_CODE));
            setf(c, F_ZF, ok);
            return;
        }
        cpu16_set_fault(c, FAULT_HLT, "system-table instruction (0F 00)");
        return;
    }
    if (op == 0x02 || op == 0x03) {                       // lar, lsl
        decode_modrm(c, in, &m);
        const struct cpu16_desc *d = desc_of(c, rm_get(c, &m, 1));
        if (!d || (d->kind != D_DATA && d->kind != D_CODE)) { setf(c, F_ZF, 0); return; }
        setf(c, F_ZF, 1);
        if (op == 0x03) c->r[m.reg] = (uint16_t)d->limit;
        else c->r[m.reg] = d->kind == D_CODE ? 0xFA00 : 0xF200;   // present, DPL 3, code/data
        return;
    }
    char why[64];
    snprintf(why, sizeof why, "opcode 0F %02X is not implemented", op);
    cpu16_set_fault(c, FAULT_UD, why);
}

static void step(struct cpu16 *c) {
    struct insn in = { -1, 0, c->sreg[S_CS], c->ip };
    struct modrm m;
    uint8_t op;
    for (;;) {
        op = fetch8(c);
        if (c->fault) return;
        if (op == 0x26) in.seg = S_ES;
        else if (op == 0x2E) in.seg = S_CS;
        else if (op == 0x36) in.seg = S_SS;
        else if (op == 0x3E) in.seg = S_DS;
        else if (op == 0xF2 || op == 0xF3) in.rep = op;
        else if (op == 0xF0) continue;
        else break;
    }
    c->fault_cs = in.cs;
    c->fault_ip = in.ip;

    if (op < 0x40 && (op & 7) < 6) {                      // the ALU block
        int alu_op = op >> 3, w = op & 1;
        uint16_t r;
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
            uint16_t imm = w ? fetch16(c) : fetch8(c);
            r = alu(c, alu_op, reg_get(c, R_AX, w), imm, w);
            if (alu_op != 7) reg_set(c, R_AX, w, r);
            return;
        }
        }
    }

    // dispatch-ok: one case per x86 opcode, a set the 286 fixed in 1982.
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        cpu16_push(c, c->sreg[op >> 3]);
        return;
    case 0x07: case 0x17: case 0x1F:
        cpu16_load_sreg(c, op >> 3, cpu16_pop(c));
        return;
    case 0x0F: op_0f(c, &in); return;
    case 0x27: case 0x2F: case 0x37: case 0x3F: bcd(c, op); return;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        c->r[op & 7] = incdec(c, c->r[op & 7], 0, 1);
        return;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        c->r[op & 7] = incdec(c, c->r[op & 7], 1, 1);
        return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        uint16_t v = c->r[op & 7];                        // push sp pushes the old value (286)
        cpu16_push(c, v);
        return;
    }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint16_t v = cpu16_pop(c);
        c->r[op & 7] = v;
        return;
    }
    case 0x60: {                                          // pusha
        uint16_t sp = c->r[R_SP];
        for (int i = 0; i < 8; i++) cpu16_push(c, i == R_SP ? sp : c->r[i]);
        return;
    }
    case 0x61:                                            // popa
        for (int i = 7; i >= 0; i--) {
            uint16_t v = cpu16_pop(c);
            if (i != R_SP) c->r[i] = v;
        }
        return;
    case 0x62: {                                          // bound
        decode_modrm(c, &in, &m);
        if (m.mod == 3) { undefined(c, op); return; }
        int16_t v = (int16_t)c->r[m.reg];
        int16_t lo = (int16_t)rd16(c, m.seg, m.off), hi = (int16_t)rd16(c, m.seg, (uint16_t)(m.off + 2));
        if (v < lo || v > hi) interrupt(c, 5);
        return;
    }
    case 0x63:                                            // arpl
        decode_modrm(c, &in, &m);
        setf(c, F_ZF, 0);
        return;
    case 0x68: cpu16_push(c, fetch16(c)); return;
    case 0x6A: cpu16_push(c, (uint16_t)(int8_t)fetch8(c)); return;
    case 0x69: case 0x6B: {                               // imul r, rm, imm
        decode_modrm(c, &in, &m);
        int16_t a = (int16_t)rm_get(c, &m, 1);
        int16_t b = op == 0x69 ? (int16_t)fetch16(c) : (int8_t)fetch8(c);
        int32_t r = (int32_t)a * b;
        c->r[m.reg] = (uint16_t)r;
        setf(c, F_CF | F_OF, r != (int16_t)r);
        szp(c, (uint16_t)r, 1);
        return;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF:
        cpu16_set_fault(c, FAULT_HLT, "port I/O");
        return;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)fetch8(c);
        if (cond(c, op & 0x0F)) c->ip = (uint16_t)(c->ip + d);
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int w = op & 1;
        decode_modrm(c, &in, &m);
        uint16_t v = rm_get(c, &m, w);
        uint16_t imm = op == 0x81 ? fetch16(c) : op == 0x83 ? (uint16_t)(int8_t)fetch8(c) : fetch8(c);
        uint16_t r = alu(c, m.reg, v, imm, w);
        if (m.reg != 7) rm_set(c, &m, w, r);
        return;
    }
    case 0x84: case 0x85:
        decode_modrm(c, &in, &m);
        alu(c, 4, rm_get(c, &m, op & 1), reg_get(c, m.reg, op & 1), op & 1);
        return;
    case 0x86: case 0x87: {
        int w = op & 1;
        decode_modrm(c, &in, &m);
        uint16_t a = rm_get(c, &m, w), b = reg_get(c, m.reg, w);
        rm_set(c, &m, w, b);
        reg_set(c, m.reg, w, a);
        return;
    }
    case 0x88: case 0x89:
        decode_modrm(c, &in, &m);
        rm_set(c, &m, op & 1, reg_get(c, m.reg, op & 1));
        return;
    case 0x8A: case 0x8B:
        decode_modrm(c, &in, &m);
        reg_set(c, m.reg, op & 1, rm_get(c, &m, op & 1));
        return;
    case 0x8C:
        decode_modrm(c, &in, &m);
        if (m.reg > 3) { undefined(c, op); return; }
        rm_set(c, &m, 1, c->sreg[m.reg]);
        return;
    case 0x8D:
        decode_modrm(c, &in, &m);
        if (m.mod == 3) { undefined(c, op); return; }
        c->r[m.reg] = m.off;
        return;
    case 0x8E:
        decode_modrm(c, &in, &m);
        if (m.reg > 3) { undefined(c, op); return; }
        cpu16_load_sreg(c, m.reg, rm_get(c, &m, 1));
        return;
    case 0x8F: {
        decode_modrm(c, &in, &m);
        uint16_t v = cpu16_pop(c);
        rm_set(c, &m, 1, v);
        return;
    }
    case 0x90: return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint16_t t = c->r[R_AX];
        c->r[R_AX] = c->r[op & 7];
        c->r[op & 7] = t;
        return;
    }
    case 0x98: c->r[R_AX] = (uint16_t)(int8_t)c->r[R_AX]; return;
    case 0x99: c->r[R_DX] = c->r[R_AX] & 0x8000 ? 0xFFFF : 0; return;
    case 0x9A: {
        uint16_t off = fetch16(c), sel = fetch16(c);
        far_call(c, sel, off);
        return;
    }
    case 0x9B: return;                                    // wait
    case 0x9C: cpu16_push(c, (uint16_t)((c->flags & 0x0FD5) | 0x0002)); return;
    case 0x9D: c->flags = (uint16_t)((cpu16_pop(c) & 0x0FD5) | 0x0002); return;
    case 0x9E: c->flags = (uint16_t)((c->flags & 0xFF00) | (get8(c, 4) & 0xD5) | 0x02); return;
    case 0x9F: set8(c, 4, (uint8_t)((c->flags & 0xD5) | 0x02)); return;
    case 0xA0: case 0xA1: {
        uint16_t off = fetch16(c);
        int s = in.seg >= 0 ? in.seg : S_DS;
        reg_set(c, R_AX, op & 1, op & 1 ? rd16(c, s, off) : rd8(c, s, off));
        return;
    }
    case 0xA2: case 0xA3: {
        uint16_t off = fetch16(c);
        int s = in.seg >= 0 ? in.seg : S_DS;
        if (op & 1) wr16(c, s, off, c->r[R_AX]);
        else wr8(c, s, off, (uint8_t)c->r[R_AX]);
        return;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, &in, op);
        return;
    case 0xA8: alu(c, 4, (uint8_t)c->r[R_AX], fetch8(c), 0); return;
    case 0xA9: alu(c, 4, c->r[R_AX], fetch16(c), 1); return;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set8(c, op & 7, fetch8(c));
        return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        c->r[op & 7] = fetch16(c);
        return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int w = op & 1;
        decode_modrm(c, &in, &m);
        uint16_t v = rm_get(c, &m, w);
        unsigned n = op <= 0xC1 ? fetch8(c) : op <= 0xD1 ? 1 : (uint8_t)c->r[R_CX];
        n &= 0x1F;                                        // the 186's mask
        rm_set(c, &m, w, shift(c, m.reg, v, n, w));
        return;
    }
    case 0xC2: { uint16_t n = fetch16(c); c->ip = cpu16_pop(c); c->r[R_SP] += n; return; }
    case 0xC3: c->ip = cpu16_pop(c); return;
    case 0xC4: case 0xC5: {                               // les, lds
        decode_modrm(c, &in, &m);
        if (m.mod == 3) { undefined(c, op); return; }
        uint16_t off = rd16(c, m.seg, m.off), sel = rd16(c, m.seg, (uint16_t)(m.off + 2));
        if (cpu16_load_sreg(c, op == 0xC4 ? S_ES : S_DS, sel) == 0) c->r[m.reg] = off;
        return;
    }
    case 0xC6: case 0xC7:
        decode_modrm(c, &in, &m);
        rm_set(c, &m, op & 1, op & 1 ? fetch16(c) : fetch8(c));
        return;
    case 0xC8: {                                          // enter
        uint16_t size = fetch16(c);
        unsigned level = fetch8(c) & 31;
        cpu16_push(c, c->r[R_BP]);
        uint16_t frame = c->r[R_SP];
        for (unsigned i = 1; i < level; i++) {
            c->r[R_BP] -= 2;
            cpu16_push(c, rd16(c, S_SS, c->r[R_BP]));
        }
        if (level) cpu16_push(c, frame);
        c->r[R_BP] = frame;
        c->r[R_SP] -= size;
        return;
    }
    case 0xC9: c->r[R_SP] = c->r[R_BP]; c->r[R_BP] = cpu16_pop(c); return;
    case 0xCA: far_return(c, fetch16(c)); return;
    case 0xCB: far_return(c, 0); return;
    case 0xCC: interrupt(c, 3); return;
    case 0xCD: interrupt(c, fetch8(c)); return;
    case 0xCE: if (c->flags & F_OF) interrupt(c, 4); return;
    case 0xCF: {                                          // iret
        uint16_t ip = cpu16_pop(c), cs = cpu16_pop(c);
        c->flags = (uint16_t)((cpu16_pop(c) & 0x0FD5) | 0x0002);
        if (!c->fault) far_jump(c, cs, ip);
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
        set8(c, 0, rd8(c, s, (uint16_t)(c->r[R_BX] + (uint8_t)c->r[R_AX])));
        return;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        cpu16_set_fault(c, FAULT_UD, "x87 floating point is not emulated yet");
        return;
    case 0xE0: case 0xE1: case 0xE2: {                    // loopnz loopz loop
        int8_t d = (int8_t)fetch8(c);
        c->r[R_CX]--;
        int zf = (c->flags & F_ZF) != 0;
        if (c->r[R_CX] && (op == 0xE2 || (op == 0xE1 ? zf : !zf))) c->ip = (uint16_t)(c->ip + d);
        return;
    }
    case 0xE3: {
        int8_t d = (int8_t)fetch8(c);
        if (!c->r[R_CX]) c->ip = (uint16_t)(c->ip + d);
        return;
    }
    case 0xE8: { uint16_t d = fetch16(c); cpu16_push(c, c->ip); c->ip = (uint16_t)(c->ip + d); return; }
    case 0xE9: { uint16_t d = fetch16(c); c->ip = (uint16_t)(c->ip + d); return; }
    case 0xEA: { uint16_t off = fetch16(c), sel = fetch16(c); far_jump(c, sel, off); return; }
    case 0xEB: { int8_t d = (int8_t)fetch8(c); c->ip = (uint16_t)(c->ip + d); return; }
    case 0xF4: cpu16_set_fault(c, FAULT_HLT, "hlt"); return;
    case 0xF5: c->flags ^= F_CF; return;
    case 0xF6: case 0xF7:
        decode_modrm(c, &in, &m);
        group3(c, &m, op & 1);
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
        case 0: case 1: rm_set(c, &m, 1, incdec(c, rm_get(c, &m, 1), m.reg, 1)); return;
        case 2: { uint16_t t = rm_get(c, &m, 1); cpu16_push(c, c->ip); c->ip = t; return; }
        case 3: case 5: {
            if (m.mod == 3) { undefined(c, op); return; }
            uint16_t off = rd16(c, m.seg, m.off), sel = rd16(c, m.seg, (uint16_t)(m.off + 2));
            if (c->fault) return;
            if (m.reg == 3) far_call(c, sel, off);
            else far_jump(c, sel, off);
            return;
        }
        case 4: c->ip = rm_get(c, &m, 1); return;
        case 6: cpu16_push(c, rm_get(c, &m, 1)); return;
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
        c->steps++;
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
