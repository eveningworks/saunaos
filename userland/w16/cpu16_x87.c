// cpu16_x87.c -- w16's x87 (cpu16.h): the program's FPU state is an
// FNSAVE image, and each ESC instruction runs ON THE HOST'S OWN FPU --
// FRSTOR the image, run that one instruction from a stub, FNSAVE it back
// (DOSBox's x86 FPU core does the same). The results are a real x87's to
// the last bit, transcendentals and unmasked-exception responses included;
// tools/cpu16_hostcheck.py holds it to the host running the same 16-bit
// code natively.
//
// THE HOST NEVER TRAPS, and that rests on two facts. An x87 exception
// traps at the NEXT waiting instruction, never the one that raised it, and
// FNSAVE does not wait -- so the instruction runs with the program's real
// masks and FNSAVE takes the state, ES and all, before anything traps. And
// a waiting instruction never reaches the host while one is pending:
// cpu16.c raises INT 75h first (FRSTORing a state with an unmasked flag
// set and then running a waiting instruction DOES trap).
//
// Memory operands are copied through a host buffer, so what the host runs
// is always `ESC [rax]`; cpu16.c and cpu16_ptr() decoded and limit-checked
// the program's own addressing. The environment and FNSAVE formats differ
// (16-bit in the program, 32-bit on the host), so FLDENV, FNSTENV, FRSTOR
// and FNSAVE are done here in C, and the instruction and operand pointers
// a 387 keeps are kept beside the image.
#include "cpu16.h"

#include <string.h>

#define IMG_CW   0
#define IMG_SW   4
#define IMG_TW   8
#define IMG_REGS 28

#define SW_FLAGS 0x007F      // IE..PE and SF, the sticky ones
#define SW_ES    0x0080
#define SW_TOP   0x3800
#define SW_B     0x8000

#define CW_INIT  0x037F

// The stubs: `ESC modrm; ret` at a 4-byte pitch. 512 register forms
// (D8-DF x C0-FF), then 64 memory forms with mod=00 rm=000, which in
// 64-bit mode is [rax].
__asm__(".pushsection .text.cpu16_x87_stubs,\"ax\",@progbits\n"
        ".balign 16\n"
        ".globl cpu16_x87_stubs\n"
        ".hidden cpu16_x87_stubs\n"
        "cpu16_x87_stubs:\n"
        ".irp op,0xd8,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf\n"
        ".set m,0xc0\n"
        ".rept 64\n"
        ".byte \\op, m, 0xc3, 0xcc\n"
        ".set m,m+1\n"
        ".endr\n"
        ".endr\n"
        ".irp op,0xd8,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf\n"
        ".set r,0\n"
        ".rept 8\n"
        ".byte \\op, r << 3, 0xc3, 0xcc\n"
        ".set r,r+1\n"
        ".endr\n"
        ".endr\n"
        ".popsection\n");
extern const uint8_t cpu16_x87_stubs[] __attribute__((visibility("hidden")));

static uint16_t rd16(const uint8_t *p, unsigned at) { return (uint16_t)(p[at] | p[at + 1] << 8); }
static void wr16(uint8_t *p, unsigned at, uint16_t v) { p[at] = (uint8_t)v; p[at + 1] = (uint8_t)(v >> 8); }

static void host_run(uint8_t *img, const uint8_t *stub, uint8_t *buf) {
    __asm__ volatile("frstor %0\n\t"
                     "call *%1\n\t"
                     "fnsave %0"
                     : "+m"(*(uint8_t(*)[108])img)
                     : "r"(stub), "a"(buf)
                     : "memory", "cc", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
}

// The image through the host and back, after C wrote into it: the CPU's
// own reading of it -- tags from the registers, ES from the flags.
static void settle(uint8_t *img) {
    __asm__ volatile("frstor %0\n\tfnsave %0" : "+m"(*(uint8_t(*)[108])img) : : "memory",
                     "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
}

static void ready(struct cpu16_x87 *x) {
    if (x->ready) return;
    memset(x->img, 0, sizeof x->img);
    wr16(x->img, IMG_CW, CW_INIT);
    wr16(x->img, IMG_TW, 0xFFFF);
    x->fip = x->fcs = x->fop = x->fdp = x->fds = 0;
    x->ready = 1;
}

static const uint8_t *stub_of(uint8_t op, uint8_t modrm) {
    unsigned slot = modrm >= 0xC0 ? (op - 0xD8u) * 64 + (modrm - 0xC0u) : 512 + (op - 0xD8u) * 8 + ((modrm >> 3) & 7);
    return cpu16_x87_stubs + 4 * slot;
}

// A 387's register forms. Not here: the P6 ones (FCMOVcc, FCOMI, FUCOMI),
// which a 386's 387 did not have, and the encodings no x87 defines.
static int reg_form_ok(uint8_t op, uint8_t m) {
    switch (op) {
    case 0xD8: case 0xDC: return 1;
    case 0xD9: return m < 0xD1 || (m >= 0xD8 && m <= 0xE1) || m == 0xE4 || m == 0xE5 ||
                      (m >= 0xE8 && m <= 0xEE) || m >= 0xF0;
    case 0xDA: return m == 0xE9;                         // FUCOMPP
    case 0xDB: return m >= 0xE0 && m <= 0xE4;            // FENI FDISI FCLEX FINIT FSETPM
    case 0xDD: return m < 0xF0;
    case 0xDE: return m < 0xD8 || m == 0xD9 || m >= 0xE0;
    default:   return m < 0xE1;                          // DF: FFREEP, aliases, FNSTSW AX
    }
}

// A 387's memory forms: the bytes the operand is, and whether the host
// runs it (H) or C does (X: the environment and FNSAVE formats).
enum { H = 1, X = 2 };
static const struct { uint8_t len, how; } MEM[8][8] = {
    { {4, H}, {4, H}, {4, H}, {4, H}, {4, H}, {4, H}, {4, H}, {4, H} },            // D8 m32real
    { {4, H}, {0, 0}, {4, H}, {4, H}, {14, X}, {2, H}, {14, X}, {2, H} },         // D9
    { {4, H}, {4, H}, {4, H}, {4, H}, {4, H}, {4, H}, {4, H}, {4, H} },            // DA m32int
    { {4, H}, {0, 0}, {4, H}, {4, H}, {0, 0}, {10, H}, {0, 0}, {10, H} },         // DB
    { {8, H}, {8, H}, {8, H}, {8, H}, {8, H}, {8, H}, {8, H}, {8, H} },            // DC m64real
    { {8, H}, {0, 0}, {8, H}, {8, H}, {94, X}, {0, 0}, {94, X}, {2, H} },         // DD
    { {2, H}, {2, H}, {2, H}, {2, H}, {2, H}, {2, H}, {2, H}, {2, H} },            // DE m16int
    { {2, H}, {0, 0}, {2, H}, {2, H}, {10, H}, {8, H}, {10, H}, {8, H} },         // DF
};

// The memory forms that only read their operand: everything but the stores.
static int reads_only(uint8_t op, unsigned reg) {
    if (op == 0xD9) return reg == 0 || reg == 5;
    if (op == 0xDB || op == 0xDF) return reg == 0 || reg == 4 || reg == 5;
    if (op == 0xDD) return reg == 0;
    return 1;
}

// The control instructions, which leave a 387's instruction and operand
// pointers alone.
static int is_control(uint8_t op, uint8_t modrm) {
    if (modrm >= 0xC0) return op == 0xDB || (op == 0xDF && modrm == 0xE0);
    return (op == 0xD9 && ((modrm >> 3) & 7) >= 4) || (op == 0xDD && ((modrm >> 3) & 7) >= 4);
}

int cpu16_x87_waits(uint8_t op, uint8_t modrm) {
    if (modrm >= 0xC0) return !(op == 0xDB && modrm >= 0xE0) && !(op == 0xDF && modrm == 0xE0);
    unsigned reg = (modrm >> 3) & 7;
    return !(op == 0xD9 && reg >= 6) && !(op == 0xDD && reg >= 6);   // FNSTENV FNSTCW FNSAVE FNSTSW
}

unsigned cpu16_x87_pending(struct cpu16 *c) {
    struct cpu16_x87 *x = &c->x87;
    if (!x->ready) return 0;
    return rd16(x->img, IMG_SW) & ~rd16(x->img, IMG_CW) & 0x3F;
}

uint16_t cpu16_x87_status(struct cpu16 *c) {
    ready(&c->x87);
    return rd16(c->x87.img, IMG_SW);
}

void cpu16_x87_set_cw(struct cpu16 *c, uint16_t cw) {
    ready(&c->x87);
    wr16(c->x87.img, IMG_CW, cw);
    settle(c->x87.img);
}

void cpu16_x87_clear(struct cpu16 *c) {
    struct cpu16_x87 *x = &c->x87;
    ready(x);
    wr16(x->img, IMG_SW, rd16(x->img, IMG_SW) & (uint16_t)~(SW_FLAGS | SW_ES | SW_B));
}

// FNINIT. Not from a fresh state: the registers keep what they hold, and
// the host's FNINIT does exactly that.
void cpu16_x87_init(struct cpu16 *c) {
    struct cpu16_x87 *x = &c->x87;
    ready(x);
    host_run(x->img, stub_of(0xDB, 0xE3), 0);
    x->fip = x->fcs = x->fop = x->fdp = x->fds = 0;
}

int cpu16_x87_exec(struct cpu16 *c, uint8_t op, uint8_t modrm, uint8_t *mem) {
    struct cpu16_x87 *x = &c->x87;
    ready(x);
    if (op < 0xD8 || op > 0xDF) return -1;
    unsigned reg = (modrm >> 3) & 7;
    if (modrm >= 0xC0 ? !reg_form_ok(op, modrm) || is_control(op, modrm) : MEM[op - 0xD8][reg].how != H)
        return -1;
    if (cpu16_x87_waits(op, modrm) && cpu16_x87_pending(c)) return -1;   // the host would trap
    uint8_t buf[16] = { 0 };
    unsigned len = modrm >= 0xC0 ? 0 : MEM[op - 0xD8][reg].len;
    if (len) memcpy(buf, mem, len);
    host_run(x->img, stub_of(op, modrm), buf);
    if (len) memcpy(mem, buf, len);
    return (int)cpu16_x87_pending(c);
}

// --- the program's view ----------------------------------------------------

void cpu16_x87_save(struct cpu16 *c, uint8_t out[108]) {
    struct cpu16_x87 *x = &c->x87;
    ready(x);
    memcpy(out, x->img, 108);
    memset(out + 12, 0, IMG_REGS - 12);
    wr16(out, 12, x->fip);
    wr16(out, 16, x->fcs);
    wr16(out, 18, x->fop);
    wr16(out, 20, x->fdp);
    wr16(out, 24, x->fds);
}

void cpu16_x87_load(struct cpu16 *c, const uint8_t in[108]) {
    struct cpu16_x87 *x = &c->x87;
    ready(x);
    memcpy(x->img, in, 108);
    x->fip = rd16(in, 12);
    x->fcs = rd16(in, 16);
    x->fop = rd16(in, 18) & 0x7FF;
    x->fdp = rd16(in, 20);
    x->fds = rd16(in, 24);
    settle(x->img);
}

// The 16-bit protected-mode environment: CW SW TW IP CS DP DS, a word each.
static void env_out(struct cpu16_x87 *x, uint8_t *p) {
    wr16(p, 0, rd16(x->img, IMG_CW));
    wr16(p, 2, rd16(x->img, IMG_SW));
    wr16(p, 4, rd16(x->img, IMG_TW));
    wr16(p, 6, x->fip);
    wr16(p, 8, x->fcs);
    wr16(p, 10, x->fdp);
    wr16(p, 12, x->fds);
}

// The registers stay where they are physically, so a new TOP turns the
// image's ST(0)-first order: ST(i) is the old ST(i + new - old).
static void env_in(struct cpu16_x87 *x, const uint8_t *p) {
    unsigned top0 = (rd16(x->img, IMG_SW) & SW_TOP) >> 11, top = (rd16(p, 2) & SW_TOP) >> 11;
    uint8_t regs[80];
    for (unsigned i = 0; i < 8; i++) memcpy(regs + 10 * i, x->img + IMG_REGS + 10 * ((i + top - top0) & 7), 10);
    memcpy(x->img + IMG_REGS, regs, sizeof regs);
    wr16(x->img, IMG_CW, rd16(p, 0));
    wr16(x->img, IMG_SW, rd16(p, 2));
    wr16(x->img, IMG_TW, rd16(p, 4));
    x->fip = rd16(p, 6);
    x->fcs = rd16(p, 8);
    x->fdp = rd16(p, 10);
    x->fds = rd16(p, 12);
}

void cpu16_x87_op(struct cpu16 *c, uint8_t op, uint8_t modrm, int seg, uint16_t off, uint16_t cs, uint16_t ip) {
    struct cpu16_x87 *x = &c->x87;
    ready(x);
    unsigned reg = (modrm >> 3) & 7;
    if (modrm >= 0xC0 ? !reg_form_ok(op, modrm) : !MEM[op - 0xD8][reg].how) {
        cpu16_set_fault(c, FAULT_UD, "an x87 instruction a 387 does not have");
        return;
    }
    uint8_t buf[16] = { 0 }, *p = 0;
    unsigned len = 0;
    if (modrm < 0xC0) {
        len = MEM[op - 0xD8][reg].len;
        p = cpu16_ptr(c, c->sreg[seg], off, len, !reads_only(op, reg));
        if (!p) return;
    }
    if (op == 0xDF && modrm == 0xE0) {                   // FNSTSW AX: the stub's AX is the buffer's
        c->r[R_AX] = rd16(x->img, IMG_SW);
        return;
    }
    if (op == 0xDB && modrm == 0xE3) {
        cpu16_x87_init(c);
        return;
    }
    if (p && MEM[op - 0xD8][reg].how == X) {
        if (op == 0xD9 && reg == 4) env_in(x, p);       // FLDENV
        else if (op == 0xD9) {                           // FNSTENV, then every exception masked
            env_out(x, p);
            wr16(x->img, IMG_CW, rd16(x->img, IMG_CW) | 0x3F);
        } else if (reg == 4) {                           // FRSTOR
            env_in(x, p);
            memcpy(x->img + IMG_REGS, p + 14, 80);
        } else {                                         // FNSAVE, then FNINIT
            env_out(x, p);
            memcpy(p + 14, x->img + IMG_REGS, 80);
            cpu16_x87_init(c);
            return;
        }
        settle(x->img);
        return;
    }
    if (len) memcpy(buf, p, len);
    host_run(x->img, stub_of(op, modrm), buf);
    if (len && !reads_only(op, reg)) memcpy(p, buf, len);
    if (is_control(op, modrm)) return;
    x->fip = ip;
    x->fcs = cs;
    x->fop = (uint16_t)((op & 7) << 8 | modrm);
    if (p) {
        x->fdp = off;
        x->fds = c->sreg[seg];
    }
}
