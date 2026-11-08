// /lib/libinit_b.so -- initfini_dyn_test's second library, which NEEDS
// libinit_a.so. The program names libinit_a.so first, so the loader maps
// A before B (breadth-first), and reversing the load order would run B's
// constructor before A's; the word this constructor writes says whether
// A's had already run.
#include <stddef.h>
#include "rt/sys.h"

int initfini_a_ready(void);

static void out(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

__attribute__((constructor)) static void b_ctor(void) {
    out(initfini_a_ready() ? "B-ctor(A-first) " : "B-ctor(A-NOT-YET) ");
}
__attribute__((destructor)) static void b_dtor(void) { out("B-dtor "); }

int initfini_b_marker(void) { return 0xB; }
