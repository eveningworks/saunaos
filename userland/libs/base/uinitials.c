// See base/uinitials.h.
#include "base/uinitials.h"
#include <string.h>

void uinitials(const char *name, char *out, size_t cap) {
    if (!cap) return;
    out[0] = '\0';
    size_t n = 0;
    const char *p = name ? name : "";
    for (int word = 0; word < 2; word++) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        unsigned char c = (unsigned char)*p;
        if (word == 1 && !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) break;
        size_t len = 1;   // the whole character: a UTF-8 lead byte and its continuations
        if (c >= 0xC0)
            while (p[len] && ((unsigned char)p[len] & 0xC0) == 0x80) len++;
        if (n + len + 1 > cap) break;
        memcpy(out + n, p, len);
        if (c >= 'a' && c <= 'z') out[n] = (char)(c - 'a' + 'A');
        n += len;
        out[n] = '\0';
        while (*p && *p != ' ' && *p != '\t') p++;
    }
}
