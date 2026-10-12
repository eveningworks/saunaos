#ifndef ULIB_ARPA_INET_H
#define ULIB_ARPA_INET_H

// Byte-order conversion, and POSIX's two address-text conversions,
// inet_pton() and inet_ntop() (userland/libc/inet.c). The network stack
// is the kernel's and takes addresses as integers; these exist for
// ported code that parses an address out of text -- NetSurf telling an
// IP-literal URL host from a name. The pre-POSIX inet_aton/inet_addr/
// inet_ntoa are not here: POSIX replaced them with these two.
//
// x86-64 is little-endian, so the host-to-network pair is a byte swap
// and the network-to-host pair is the same swap again. __builtin_bswap
// compiles to one BSWAP instruction.

#include <stdint.h>
#include <netinet/in.h>

static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }

// AF_INET: dotted decimal, exactly four parts of 0-255 -- no octal, no
// hex, no short forms (POSIX's rule, and stricter than inet_aton's).
// AF_INET6: RFC 4291's text forms, `::` and a trailing dotted quad
// included. 1 on success, 0 for text that is not an address, -1 with
// EAFNOSUPPORT for another family. `dst` is the address in NETWORK order.
int inet_pton(int af, const char *src, void *dst);
// The address as text into `dst` (`size` bytes), or NULL with ENOSPC
// when it does not fit -- written whole or not at all. IPv6 is written
// in RFC 5952's canonical form: lower case, the longest run of zero
// groups as `::`.
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#endif
