// /lib/libinit_a.so -- initfini_dyn_test's first library: a constructor
// and a destructor that each write one word, and a flag its dependent
// (libinit_b.so) reads to prove this one was initialised FIRST.
//
// sys_write() is the executable's, exported to every library (the Makefile's
// DYN_LINK), so this links nothing and has no DT_NEEDED of its own.
#include <stddef.h>
#include <stdint.h>

#include "rt/sys.h"

static void out(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

static int g_ready;

__attribute__((constructor)) static void a_ctor(void) { out("A-ctor "); g_ready = 1; }
__attribute__((destructor)) static void a_dtor(void) { out("A-dtor "); }

int initfini_a_ready(void) { return g_ready; }
