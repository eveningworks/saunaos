// <fenv.h>: the exception flags and rounding mode of SSE and the x87
// together. Both units keep the same five flags in the same bit order
// (x87 status word bits 0-5, MXCSR bits 0-5) and the same two-bit
// rounding field (x87 control word bits 10-11, MXCSR bits 13-14), which
// is why one FE_* value serves both and the shifts below are the only
// translation. musl's x86_64 fenv.s is the same shape.
#include <fenv.h>
#include <stdint.h>

static uint32_t get_mxcsr(void) { uint32_t c; __asm__ volatile("stmxcsr %0" : "=m"(c)); return c; }
static void set_mxcsr(uint32_t c) { __asm__ volatile("ldmxcsr %0" : : "m"(c)); }
static uint16_t get_x87_sw(void) { uint16_t s; __asm__ volatile("fnstsw %0" : "=m"(s)); return s; }
static uint16_t get_x87_cw(void) { uint16_t c; __asm__ volatile("fnstcw %0" : "=m"(c)); return c; }
static void set_x87_cw(uint16_t c) { __asm__ volatile("fldcw %0" : : "m"(c)); }

int feclearexcept(int excepts) {
    excepts &= FE_ALL_EXCEPT;
    // The x87 status word cannot be written alone: through its
    // environment, with the bits cleared.
    fenv_t env;
    __asm__ volatile("fnstenv %0" : "=m"(env));
    env.__status_word &= (unsigned short)~excepts;
    __asm__ volatile("fldenv %0" : : "m"(env));
    set_mxcsr(get_mxcsr() & ~(uint32_t)excepts);
    return 0;
}

// Set in MXCSR, where a masked exception does nothing but record
// itself; the next floating-point instruction sees it. Real arithmetic
// would raise underflow and overflow with inexact too, which C allows.
int feraiseexcept(int excepts) {
    excepts &= FE_ALL_EXCEPT;
    set_mxcsr(get_mxcsr() | (uint32_t)excepts);
    return 0;
}

int fetestexcept(int excepts) {
    return (int)((get_mxcsr() | get_x87_sw()) & (uint32_t)excepts & FE_ALL_EXCEPT);
}

int fegetexceptflag(fexcept_t *flagp, int excepts) {
    *flagp = (fexcept_t)fetestexcept(excepts);
    return 0;
}

int fesetexceptflag(const fexcept_t *flagp, int excepts) {
    excepts &= FE_ALL_EXCEPT;
    feclearexcept(excepts);
    set_mxcsr(get_mxcsr() | (*flagp & (uint32_t)excepts));
    return 0;
}

int fegetround(void) { return (int)(get_mxcsr() >> 3 & 0xc00); }

int fesetround(int round) {
    if (round & ~0xc00) return -1;
    set_x87_cw((uint16_t)((get_x87_cw() & ~0xc00) | round));
    set_mxcsr((get_mxcsr() & ~(0xc00u << 3)) | ((uint32_t)round << 3));
    return 0;
}

int fegetenv(fenv_t *envp) {
    // fnstenv masks every x87 exception as a side effect; fldenv puts
    // the control word back.
    __asm__ volatile("fnstenv %0\n\tfldenv %0" : "=m"(*envp));
    envp->__mxcsr = get_mxcsr();
    return 0;
}

int fesetenv(const fenv_t *envp) {
    if (envp == FE_DFL_ENV) {
        __asm__ volatile("fninit");
        set_mxcsr(0x1f80);
        return 0;
    }
    __asm__ volatile("fldenv %0" : : "m"(*envp));
    set_mxcsr(envp->__mxcsr);
    return 0;
}

int feholdexcept(fenv_t *envp) {
    fegetenv(envp);
    fenv_t held = *envp;
    held.__control_word |= FE_ALL_EXCEPT | 0x02;    // mask everything, denormal too
    held.__status_word &= (unsigned short)~0x3f;
    held.__mxcsr = (held.__mxcsr | (0x3fu << 7)) & ~0x3fu;
    fesetenv(&held);
    return 0;
}

int feupdateenv(const fenv_t *envp) {
    int pending = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(envp);
    feraiseexcept(pending);
    return 0;
}
