#ifndef STRVEC_DIGEST_H
#define STRVEC_DIGEST_H

// FNV-1a over a NULL-terminated string vector, each string's NUL
// included -- how a test compares an argv or environment too big to
// echo back with the one its child received. One definition, so the
// parent's expectation and the child's report cannot drift apart.
// `*bytes` (if non-NULL) gets the vector's size, every NUL included.
#include <stddef.h>

static inline unsigned long strvec_fnv(char *const *v, size_t *bytes) {
    unsigned long h = 1469598103934665603UL;
    size_t n = 0;
    for (; v && *v; v++)
        for (const char *c = *v; ; c++) {
            h = (h ^ (unsigned char)*c) * 1099511628211UL;
            n++;
            if (!*c) break;
        }
    if (bytes) *bytes = n;
    return h;
}

#endif
