// /lib/libtlsdyn.so -- tls_dyn_test's startup library: thread-local data
// reached all three ways a shared object reaches it, so each relocation
// class ld-toy applies for TLS has a reader:
//   - global-dynamic (__tls_get_addr, DTPMOD64 + DTPOFF64) -- the default
//     for -fpic code, what a dlopen()ed library has to use
//   - initial-exec (TPOFF64 through the GOT) -- legal only in a library
//     loaded at startup, whose block has a static offset
//   - a variable the EXECUTABLE reaches with `extern __thread` (its own
//     TPOFF64, against this library's block)
#include <stdint.h>

__thread int tls_lib_gd = 7;
__thread int tls_lib_bss;                                      // .tbss: zero in every thread
__thread int tls_lib_ie __attribute__((tls_model("initial-exec"))) = 9;
__thread long tls_lib_shared = 1234;                           // the exe's extern __thread

int *tls_lib_gd_addr(void) { return &tls_lib_gd; }
int *tls_lib_bss_addr(void) { return &tls_lib_bss; }
int *tls_lib_ie_addr(void) { return &tls_lib_ie; }
long *tls_lib_shared_addr(void) { return &tls_lib_shared; }
int tls_lib_bump(void) { return ++tls_lib_gd + ++tls_lib_ie; }
