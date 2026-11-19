// /lib/libtlsalign.so -- a library whose thread-local block must be
// aligned past a page, which no per-thread block here can promise:
// tls_dyn_test checks a dlopen() of it is refused, not misaligned.
__thread char tls_align_buf[64] __attribute__((aligned(8192)));
char *tls_align_addr(void) { return tls_align_buf; }
