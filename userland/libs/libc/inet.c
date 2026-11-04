// inet_pton() and inet_ntop(), POSIX's address-text conversions.
//
// A PARSER REJECTS RATHER THAN GUESSES: "1.2.3" is not 1.2.0.3 here (it
// is to the pre-POSIX inet_aton), "01.2.3.4" is not octal, and an IPv6
// text with two `::` or a group of five digits is not an address.
// mbedTLS reaches these too, through x509_crt.c's check that a
// certificate's IP address matches the one connected to.
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Four decimal parts of 0-255, no leading zero but a lone "0", and
// nothing after. `end` NULL means the text must stop there exactly.
static int parse_v4(const char *s, uint8_t out[4]) {
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        if (s[0] == '0' && s[1] >= '0' && s[1] <= '9') return 0;
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255) return 0;
        }
        out[part] = (uint8_t)v;
        if (part < 3 && *s++ != '.') return 0;
    }
    return *s == '\0';
}

static int parse_v6(const char *s, uint8_t out[16]) {
    uint16_t g[8];
    int n = 0, gap = -1;              // groups so far; where `::` sits
    if (s[0] == ':') {
        if (s[1] != ':') return 0;
        gap = 0;
        s += 2;
        if (*s == '\0') goto done;
    }
    for (;;) {
        // A dotted quad may end the address, worth two groups.
        const char *dot = s;
        while (*dot && *dot != ':' && *dot != '.') dot++;
        if (*dot == '.') {
            uint8_t v4[4];
            if (n > 6 || !parse_v4(s, v4)) return 0;
            g[n++] = (uint16_t)(v4[0] << 8 | v4[1]);
            g[n++] = (uint16_t)(v4[2] << 8 | v4[3]);
            break;
        }
        unsigned v = 0;
        int digits = 0;
        int h;
        while ((h = hexval(*s)) >= 0) {
            v = v << 4 | (unsigned)h;
            if (++digits > 4) return 0;
            s++;
        }
        if (!digits || n == 8) return 0;
        g[n++] = (uint16_t)v;
        if (*s == '\0') break;
        if (*s++ != ':') return 0;
        if (*s == ':') {
            if (gap >= 0) return 0;       // a second `::`
            gap = n;
            s++;
            if (*s == '\0') break;
        }
    }
done:
    if (gap < 0 ? n != 8 : n > 7) return 0;
    int fill = 8 - n;
    uint16_t full[8];
    for (int i = 0, j = 0; i < 8; i++) {
        if (gap >= 0 && i >= gap && i < gap + fill) full[i] = 0;
        else full[i] = g[j++];
    }
    for (int i = 0; i < 8; i++) {
        out[2 * i] = (uint8_t)(full[i] >> 8);
        out[2 * i + 1] = (uint8_t)full[i];
    }
    return 1;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af == AF_INET) {
        uint8_t b[4];
        if (!parse_v4(src, b)) return 0;
        memcpy(dst, b, 4);
        return 1;
    }
    if (af == AF_INET6) {
        uint8_t b[16];
        if (!parse_v6(src, b)) return 0;
        memcpy(dst, b, 16);
        return 1;
    }
    errno = EAFNOSUPPORT;
    return -1;
}

static char *put_dec(char *p, unsigned v) {
    if (v >= 100) *p++ = (char)('0' + v / 100);
    if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}

static const char *finish(const char *text, size_t len, char *dst, socklen_t size) {
    if (len + 1 > size) { errno = ENOSPC; return 0; }
    memcpy(dst, text, len);
    dst[len] = '\0';
    return dst;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    const uint8_t *b = src;
    char buf[INET6_ADDRSTRLEN];
    char *p = buf;
    if (af == AF_INET) {
        for (int i = 0; i < 4; i++) {
            if (i) *p++ = '.';
            p = put_dec(p, b[i]);
        }
        return finish(buf, (size_t)(p - buf), dst, size);
    }
    if (af != AF_INET6) { errno = EAFNOSUPPORT; return 0; }

    uint16_t g[8];
    for (int i = 0; i < 8; i++) g[i] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
    // RFC 5952: the LONGEST run of two or more zero groups becomes `::`,
    // the first one on a tie.
    int best = -1, best_len = 1;
    for (int i = 0; i < 8;) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !g[j]) j++;
        if (j - i > best_len) { best = i; best_len = j - i; }
        i = j;
    }
    // ::ffff:a.b.c.d and ::a.b.c.d keep the dotted tail RFC 5952 names.
    int mapped = best == 0 && (best_len == 6 || (best_len == 5 && g[5] == 0xffff));
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        if (i == best) {
            *p++ = ':';
            if (i == 0) *p++ = ':';
            i += best_len - 1;
            continue;
        }
        if (mapped && i == 6) {
            for (int k = 12; k < 16; k++) {
                if (k > 12) *p++ = '.';
                p = put_dec(p, b[k]);
            }
            break;
        }
        int started = 0;
        for (int shift = 12; shift >= 0; shift -= 4) {
            unsigned nib = (unsigned)(g[i] >> shift) & 0xf;
            if (nib || started || shift == 0) { *p++ = HEX[nib]; started = 1; }
        }
        if (i < 7) *p++ = ':';
    }
    return finish(buf, (size_t)(p - buf), dst, size);
}
