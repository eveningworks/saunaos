// See ubase64.h.
#include "lib/ubase64.h"

int ubase64_encode(const void *in, size_t n, char *out, size_t cap) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = UBASE64_LEN(n);
    if (n > ((size_t)-1 - 3) / 4 * 3 || len + 1 > cap) return -1;
    const unsigned char *p = in;
    char *o = out;
    for (; n >= 3; n -= 3, p += 3) {
        *o++ = A[p[0] >> 2];
        *o++ = A[((p[0] & 3) << 4) | (p[1] >> 4)];
        *o++ = A[((p[1] & 15) << 2) | (p[2] >> 6)];
        *o++ = A[p[2] & 63];
    }
    if (n) {
        *o++ = A[p[0] >> 2];
        *o++ = A[((p[0] & 3) << 4) | (n > 1 ? p[1] >> 4 : 0)];
        *o++ = n > 1 ? A[(p[1] & 15) << 2] : '=';
        *o++ = '=';
    }
    *o = '\0';
    return (int)len;
}
