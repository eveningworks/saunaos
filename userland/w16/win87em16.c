// win87em16.c -- WIN87EM, Windows 3.1's floating-point support: _fpMath,
// the C runtime's one entry into it, and the coprocessor's exceptions.
//
// THE FPU IS cpu16's (cpu16_x87.c), a 387, so this is WIN87EM with a
// coprocessor present: it never emulates an instruction. What is left is
// what 3.11's WIN87EM.DLL does around one (read from its code): _fpMath's
// functions, BX naming each, and IRQ 13. An exception the program left
// unmasked raises INT 75h at the next waiting instruction (cpu16.c); the
// handler the runtime gave _fpMath (BX=3, INT 3Eh's vector in Windows) is
// far-called with AL saying which -- 81h invalid ... 86h inexact, 8Ah/8Bh
// the stack -- and returns to that instruction, which runs again.
#include "w16_internal.h"

#include <stdio.h>

#define CW_RUNTIME 0x1332    // what _fpMath's initialise sets: 64-bit precision, invalid,
                             // zero divide and overflow unmasked, as the C runtime expects

// WIN87EM's own state.
static struct {
    uint32_t handler;        // BX=3's DX:AX, the runtime's exception handler; 0 none
    uint16_t cw;             // the control word BX=4 set, which BX=5 answers
    uint16_t status;         // the exceptions so far (BX=8)
    uint16_t saved;          // BX=12's word
    int users;               // BX=0 minus BX=2
} g_em = { 0, CW_RUNTIME, 0, 0, 0 };

static void em_init(struct w16 *w) {
    cpu16_x87_init(&w->cpu);
    cpu16_x87_set_cw(&w->cpu, CW_RUNTIME);
    g_em.cw = CW_RUNTIME;
    g_em.status = 0;
}

// One instruction on ST(0) with the rounding of AX's bits 10-11, the
// control word as it was after.
static int rounded(struct w16 *w, uint8_t op, uint8_t modrm, uint8_t *mem) {
    struct cpu16 *c = &w->cpu;
    uint8_t img[108];
    cpu16_x87_save(c, img);
    uint16_t keep = (uint16_t)(img[0] | img[1] << 8);
    cpu16_x87_set_cw(c, (uint16_t)((keep & ~0x0C00) | (c->r[R_AX] & 0x0C00)));
    int r = cpu16_x87_exec(c, op, modrm, mem);
    cpu16_x87_set_cw(c, keep);
    return r;
}

// _fpMath() -> registers
// Windows: The C runtime's door into floating point, BX choosing: 0 start using it (CF set: it
// cannot), 1 initialise, 2 stop using it, 3 the handler for an exception (DX:AX), 4 set the control
// word (AX), 5 read it back, 6 round ST(0) to an integer by AX's rounding bits, 7 pop ST(0) as an
// integer into DX:AX, 8 the exceptions so far, 9 clear them, 10 how many registers are in use, 11
// whether a coprocessor is there, 12 keep a word. Anything else answers -1.
// w16: The same, on w16's 387: initialising sets the runtime's control word (1332h, invalid, zero
// divide and overflow unmasked), and a coprocessor is always there.
static int api__fpMath(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    uint8_t m[4] = { 0 }, img[108];
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  _fpMath BX=%u, x87 status %04x\n", c->r[R_BX], cpu16_x87_status(c));
        fflush(g_w16_trace);
    }
    switch (c->r[R_BX]) {
    case 0:
        g_em.users++;
        em_init(w);
        c->r[R_AX] = 0;
        c->flags &= (uint16_t)~F_CF;
        break;
    case 1: em_init(w); break;
    case 2:
        em_init(w);
        if (g_em.users) g_em.users--;
        break;
    case 3: g_em.handler = (uint32_t)c->r[R_DX] << 16 | c->r[R_AX]; break;
    case 4:
        g_em.cw = c->r[R_AX];
        cpu16_x87_set_cw(c, c->r[R_AX]);
        break;
    case 5: c->r[R_AX] = g_em.cw; break;
    case 6: rounded(w, 0xD9, 0xFC, 0); break;                 // FRNDINT
    case 7:                                                    // FISTP m32
        if (rounded(w, 0xDB, 0x18, m) < 0) return -1;
        c->r[R_AX] = (uint16_t)(m[0] | m[1] << 8);
        c->r[R_DX] = (uint16_t)(m[2] | m[3] << 8);
        break;
    case 8:
        g_em.status = (uint16_t)((g_em.status | (cpu16_x87_status(c) & 0x3F)) & 0x1FFF);
        c->r[R_AX] = g_em.status;
        break;
    case 9:
        cpu16_x87_clear(c);
        g_em.status = 0;
        break;
    case 10: {                                                 // the tags that are not empty
        cpu16_x87_save(c, img);
        unsigned tw = img[8] | img[9] << 8, n = 0;
        for (int i = 0; i < 8; i++) n += ((tw >> (2 * i)) & 3) != 3;
        c->r[R_AX] = (uint16_t)n;
        break;
    }
    case 11: c->r[R_AX] = 1; break;
    case 12: g_em.saved = c->r[R_AX]; break;
    default:
        c->r[R_AX] = c->r[R_DX] = 0xFFFF;
        break;
    }
    return 0;
}

const struct w16_api g_win87em_api[] = {
    { 1, api__fpMath },
};
const unsigned g_win87em_api_n = sizeof g_win87em_api / sizeof g_win87em_api[0];

// What WIN87EM tells the handler, in its own order of precedence: the
// stack first, then the rest as the flags rank.
static uint8_t exception_code(unsigned unmasked, uint16_t sw) {
    if ((unmasked & X87_IE) && (sw & X87_SF)) return sw & 0x0200 ? 0x8A : 0x8B;   // C1: overflow
    if (unmasked & X87_IE) return 0x81;
    if (unmasked & X87_DE) return 0x82;
    if (unmasked & X87_ZE) return 0x83;
    if (unmasked & X87_OE) return 0x84;
    if (unmasked & X87_UE) return 0x85;
    return 0x86;
}

static const char *exception_name(uint8_t code) {
    static const char *const n[] = { "an invalid operation", "a denormal operand", "a division by zero",
                                     "an overflow", "an underflow", "an inexact result", "", "",
                                     "", "a stack overflow", "a stack underflow" };
    return code >= 0x81 && code <= 0x8B ? n[code - 0x81] : "an exception";
}

// INT 75h: the exception pending, IP at the instruction that met it. The
// handler is entered as WIN87EM's IRQ 13 handler calls it, AL the code,
// and returns through a thunk that puts AX and the flags back.
int w16_fpu_irq(struct w16 *w) {
    struct cpu16 *c = &w->cpu;
    uint16_t sw = cpu16_x87_status(c);
    unsigned unmasked = cpu16_x87_pending(c);
    uint8_t code = exception_code(unmasked, sw);
    if (g_w16_trace) {
        fprintf(g_w16_trace, "INT 75h: x87 status %04x, code %02x to %04x:%04x\n", sw, code,
                (unsigned)(g_em.handler >> 16), (unsigned)(uint16_t)g_em.handler);
        fflush(g_w16_trace);
    }
    g_em.status = (uint16_t)((g_em.status | (sw & 0x3F)) & 0x1FFF);
    cpu16_x87_clear(c);
    if (!g_em.handler) {
        char why[96];
        snprintf(why, sizeof why, "the floating-point exception %s, and no handler for it", exception_name(code));
        cpu16_set_fault(c, FAULT_INT, why);
        return -1;
    }
    uint16_t ret = w16_thunk_sel(w, W16_FPE_MODULE);
    if (!ret) {
        cpu16_set_fault(c, FAULT_API, "no selector for a floating-point handler's return");
        return -1;
    }
    cpu16_push(c, c->flags);
    cpu16_push(c, c->r[R_AX]);
    cpu16_push(c, c->sreg[S_CS]);
    cpu16_push(c, c->ip);
    cpu16_push(c, ret);
    cpu16_push(c, 0);
    if (c->fault) return -1;
    c->r[R_AX] = code;
    c->sreg[S_CS] = (uint16_t)(g_em.handler >> 16);
    c->ip = (uint16_t)g_em.handler;
    return 0;
}

// The handler's far return lands here, SS:SP at the instruction to run
// again, AX and the flags above it.
int w16_fpu_irq_return(struct w16 *w) {
    struct cpu16 *c = &w->cpu;
    c->r[R_AX] = cpu16_arg(c, 0);
    c->flags = cpu16_arg(c, 2);
    return 4;
}
