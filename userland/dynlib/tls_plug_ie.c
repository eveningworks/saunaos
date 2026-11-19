// /lib/libtlsie.so -- a library that reaches its TLS initial-exec, which
// no dlopen() can satisfy (ld-toy keeps no static surplus): tls_dyn_test
// checks the dlopen() is refused, by a reason that names the cause.
__thread int tls_ie_value __attribute__((tls_model("initial-exec"))) = 5;
int tls_ie_get(void) { return tls_ie_value; }
