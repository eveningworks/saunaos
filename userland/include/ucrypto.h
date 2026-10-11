#ifndef ULIB_UCRYPTO_H
#define ULIB_UCRYPTO_H

// Message digests, MACs and the one block-cipher MAC a network protocol
// needs -- for a caller that is not TLS (an SMB client signing its
// requests, an NTLM login).
//
// **THIS HEADER NAMES NO ENGINE**, for <utls.h>'s reason: it is
// implemented by mbedTLS (`userland/backends/mbedtls/ucrypto_mbedtls.c`,
// in /lib/libssl.so) and a program opts in by linking that library.
// Nothing here is a checksum (<uhash.h> is) and nothing here is a
// password hash: MD5 and HMAC-MD5 are here because NTLMv2 is built on
// them, not because they are fit for anything new.
//
// Every function is one-shot over a list of pieces, because each
// protocol that uses these hashes a concatenation (a header and a body,
// a label and a context) and building the concatenation first costs a
// buffer the caller then has to size.
#include <stddef.h>
#include <stdint.h>

struct ucrypto_piece { const void *p; size_t n; };

// 0 on success; -1 only if the engine refuses (it does not, for these).
int ucrypto_md5(const struct ucrypto_piece *v, int n, uint8_t out[16]);
int ucrypto_sha512(const struct ucrypto_piece *v, int n, uint8_t out[64]);
int ucrypto_hmac_md5(const void *key, size_t klen,
                     const struct ucrypto_piece *v, int n, uint8_t out[16]);
int ucrypto_hmac_sha256(const void *key, size_t klen,
                        const struct ucrypto_piece *v, int n, uint8_t out[32]);
// AES-128-CMAC (RFC 4493): a 16-byte key, a 16-byte tag.
int ucrypto_aes128_cmac(const uint8_t key[16],
                        const struct ucrypto_piece *v, int n, uint8_t out[16]);

#endif
