// <ucrypto.h> on mbedTLS. The legacy (non-PSA) APIs, because each of
// these is a pure function of its inputs and PSA's key slots would add
// an import and a destroy around every call for nothing.
#include <string.h>

#include "mbedtls/build_info.h"
#include "mbedtls/md.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha512.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"

#include "ucrypto.h"

int ucrypto_md5(const struct ucrypto_piece *v, int n, uint8_t out[16]) {
    mbedtls_md5_context c;
    mbedtls_md5_init(&c);
    int rc = mbedtls_md5_starts(&c);
    for (int i = 0; !rc && i < n; i++) rc = mbedtls_md5_update(&c, v[i].p, v[i].n);
    if (!rc) rc = mbedtls_md5_finish(&c, out);
    mbedtls_md5_free(&c);
    return rc ? -1 : 0;
}

int ucrypto_sha512(const struct ucrypto_piece *v, int n, uint8_t out[64]) {
    mbedtls_sha512_context c;
    mbedtls_sha512_init(&c);
    int rc = mbedtls_sha512_starts(&c, 0);
    for (int i = 0; !rc && i < n; i++) rc = mbedtls_sha512_update(&c, v[i].p, v[i].n);
    if (!rc) rc = mbedtls_sha512_finish(&c, out);
    mbedtls_sha512_free(&c);
    return rc ? -1 : 0;
}

static int hmac(mbedtls_md_type_t t, const void *key, size_t klen,
                const struct ucrypto_piece *v, int n, uint8_t *out) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(t);
    if (!info) return -1;
    mbedtls_md_context_t c;
    mbedtls_md_init(&c);
    int rc = mbedtls_md_setup(&c, info, 1);
    if (!rc) rc = mbedtls_md_hmac_starts(&c, key, klen);
    for (int i = 0; !rc && i < n; i++) rc = mbedtls_md_hmac_update(&c, v[i].p, v[i].n);
    if (!rc) rc = mbedtls_md_hmac_finish(&c, out);
    mbedtls_md_free(&c);
    return rc ? -1 : 0;
}

int ucrypto_hmac_md5(const void *key, size_t klen,
                     const struct ucrypto_piece *v, int n, uint8_t out[16]) {
    return hmac(MBEDTLS_MD_MD5, key, klen, v, n, out);
}

int ucrypto_hmac_sha256(const void *key, size_t klen,
                        const struct ucrypto_piece *v, int n, uint8_t out[32]) {
    return hmac(MBEDTLS_MD_SHA256, key, klen, v, n, out);
}

int ucrypto_aes128_cmac(const uint8_t key[16],
                        const struct ucrypto_piece *v, int n, uint8_t out[16]) {
    const mbedtls_cipher_info_t *info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (!info) return -1;
    mbedtls_cipher_context_t c;
    mbedtls_cipher_init(&c);
    int rc = mbedtls_cipher_setup(&c, info);
    if (!rc) rc = mbedtls_cipher_cmac_starts(&c, key, 128);
    for (int i = 0; !rc && i < n; i++) rc = mbedtls_cipher_cmac_update(&c, v[i].p, v[i].n);
    if (!rc) rc = mbedtls_cipher_cmac_finish(&c, out);
    mbedtls_cipher_free(&c);
    return rc ? -1 : 0;
}
