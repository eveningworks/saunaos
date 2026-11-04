#ifndef ULIB_UBASE64_H
#define ULIB_UBASE64_H

// ubase64 -- RFC 4648 base64, the standard alphabet with '=' padding:
// what an HTTP Basic credential, a data: URL or a MIME part carries.
#include <stddef.h>

// The encoded length of `n` bytes, without the terminator.
#define UBASE64_LEN(n) ((((n) + 2) / 3) * 4)

// Encodes `n` bytes into `out` and terminates it. Returns the length, or
// -1 -- writing NOTHING -- when `cap` cannot hold it and the NUL.
int ubase64_encode(const void *in, size_t n, char *out, size_t cap);

#endif
