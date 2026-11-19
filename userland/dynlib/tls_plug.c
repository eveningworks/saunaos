// /lib/libtlsplug.so -- tls_dyn_test's dlopen()ed library: its block has
// no static offset, so every thread's copy is mapped on its first
// __tls_get_addr(), from the template (the 42 and the 64 zero bytes).
__thread int tls_plug_value = 42;
__thread char tls_plug_zeros[64];

int *tls_plug_addr(void) { return &tls_plug_value; }
int tls_plug_zero_sum(void) {
    int s = 0;
    for (int i = 0; i < 64; i++) s += tls_plug_zeros[i];
    tls_plug_zeros[0] = 1;                 // dirty this thread's copy only
    return s;
}
