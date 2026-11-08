// Constructors and destructors -- what crt0 calls between TLS and main().
//
// THE ORDER IS glibc's (measured on 2.44): the program's .preinit_array,
// then every shared library's initialisers (a library's dependencies
// before it), then the program's .init_array; at exit, after every
// atexit handler, the program's .fini_array last entry first, then the
// libraries' in the reverse of the order they were initialised. C++
// globals register their destructors with __cxa_atexit() as they are
// constructed, so those run among the handlers, newest first -- which is
// why the fini arrays are hooked onto the same list BEFORE any
// constructor can register.
//
// WHO RUNS WHAT is glibc's split too, with one move. libc runs the
// program's own arrays: they are its hidden symbols, which libc.so cannot
// see, so crt0 passes their bounds in. The loader runs the libraries'
// (DT_INIT_ARRAY) -- but when asked from here, not before it jumps to the
// entry as glibc's ld.so does, because a library's constructor may touch
// errno, and errno is __thread in a TLS block only crt0 installs. For the
// same reason the program's preinit array, which glibc's loader runs, is
// run here.
#include <stddef.h>
#include <stdlib.h>
#include "ldso_api.h"

typedef void (*rt_init_fn)(int argc, char **argv, char **envp);
typedef void (*rt_fini_fn)(void);

// crt0.asm's `__rt_initfini`, six addresses in this order.
struct rt_initfini {
    rt_init_fn *preinit, *preinit_end;
    rt_init_fn *init, *init_end;
    rt_fini_fn *fini, *fini_end;
};

extern struct ldso_api __ldso_api;   // dlfcn.c; filled by ld-toy.so

void __libc_start_init(int argc, char **argv, char **envp, const struct rt_initfini *t);

static const struct rt_initfini *g_prog;

static int loader_has(void) {
    return __ldso_api.magic == LDSO_API_MAGIC;
}

static void run_fini(void) {
    for (rt_fini_fn *p = g_prog->fini_end; p > g_prog->fini; ) (*--p)();
    if (loader_has() && __ldso_api.run_fini) __ldso_api.run_fini();
}

void __libc_start_init(int argc, char **argv, char **envp, const struct rt_initfini *t) {
    g_prog = t;
    // The first entry on the exit list, so it runs LAST. It cannot fail:
    // the first 32 entries need no allocation.
    atexit(run_fini);
    // An init function is passed (argc, argv, envp), as glibc passes
    // them; one declared with no parameters ignores them under SysV.
    for (rt_init_fn *p = t->preinit; p < t->preinit_end; p++) (*p)(argc, argv, envp);
    if (loader_has() && __ldso_api.run_init) __ldso_api.run_init(argc, argv, envp);
    for (rt_init_fn *p = t->init; p < t->init_end; p++) (*p)(argc, argv, envp);
}
