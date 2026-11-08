#ifndef ULIB_FENV_H
#define ULIB_FENV_H

// C99's floating-point environment: the exception flags and the
// rounding mode, for BOTH of x86-64's floating-point units -- SSE
// (float and double, the MXCSR register) and the x87 (long double, its
// control and status words). A function here acts on the two together,
// as glibc's and musl's do, so a mode set once applies to every type.
//
// The values and fenv_t's layout are glibc's for x86-64 (fnstenv's 28
// bytes then MXCSR), so a program's FE_* constants mean the same thing
// here as on Linux. Each process starts with the default environment:
// everything masked, round to nearest (kernel/arch/x86_64/fpu.c).
#ifdef __cplusplus
extern "C" {
#endif

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

#define FE_TONEAREST  0x000
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00

typedef unsigned short fexcept_t;

typedef struct {
    unsigned short __control_word;
    unsigned short __reserved1;
    unsigned short __status_word;
    unsigned short __reserved2;
    unsigned short __tags;
    unsigned short __reserved3;
    unsigned int __eip;
    unsigned short __cs_selector;
    unsigned int __opcode : 11;
    unsigned int __reserved4 : 5;
    unsigned int __data_offset;
    unsigned short __data_selector;
    unsigned short __reserved5;
    unsigned int __mxcsr;
} fenv_t;

// The environment a process starts with.
#define FE_DFL_ENV ((const fenv_t *)-1)

int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flagp, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flagp, int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int round);
int fegetenv(fenv_t *envp);
// Saves the environment, clears the flags and masks every trap.
int feholdexcept(fenv_t *envp);
int fesetenv(const fenv_t *envp);
// Installs *envp, then raises the exceptions that were pending before.
int feupdateenv(const fenv_t *envp);

#ifdef __cplusplus
}
#endif

#endif
