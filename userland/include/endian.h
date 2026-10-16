#ifndef ULIB_ENDIAN_H
#define ULIB_ENDIAN_H

// Byte order, glibc's and the BSDs' spelling (POSIX 2024 adopted the
// conversions). x86-64 only: little-endian, so the "le" conversions are
// the value itself and the "be" ones a byte swap.

#include <stdint.h>

#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN    4321
#define PDP_ENDIAN    3412
#define BYTE_ORDER    LITTLE_ENDIAN

#define __LITTLE_ENDIAN LITTLE_ENDIAN
#define __BIG_ENDIAN    BIG_ENDIAN
#define __PDP_ENDIAN    PDP_ENDIAN
#define __BYTE_ORDER    BYTE_ORDER

#define htobe16(x) __builtin_bswap16((uint16_t)(x))
#define htole16(x) ((uint16_t)(x))
#define be16toh(x) __builtin_bswap16((uint16_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define htobe32(x) __builtin_bswap32((uint32_t)(x))
#define htole32(x) ((uint32_t)(x))
#define be32toh(x) __builtin_bswap32((uint32_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define htobe64(x) __builtin_bswap64((uint64_t)(x))
#define htole64(x) ((uint64_t)(x))
#define be64toh(x) __builtin_bswap64((uint64_t)(x))
#define le64toh(x) ((uint64_t)(x))

#endif
