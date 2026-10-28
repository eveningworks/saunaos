#ifndef USMB_INTERNAL_H
#define USMB_INTERNAL_H

// Shared between usmb.c (the SMB2 conversation) and usmb_auth.c (NTLM,
// SPNEGO, key derivation and signing). Split by concern, as
// userland/wm/ is: the protocol never computes a MAC, and the auth half
// never sends a byte.
#include <stddef.h>
#include <stdint.h>

#include "usmb.h"

#define USMB_HDR       64
#define USMB_TX_CAP    (16 * 1024)           // the largest request: a CREATE's path
#define USMB_CREDIT    (64 * 1024)           // what one credit pays for
#define USMB_READ_MAX  (1024 * 1024)         // one READ, 16 credits, when the server allows large MTU
#define USMB_LIST_MAX  USMB_CREDIT           // one QUERY_DIRECTORY's output
#define USMB_RX_CAP    (USMB_READ_MAX + 4096)

struct usmb {
    struct usmb_io io;
    unsigned timeout_ms;
    uint16_t dialect;
    uint16_t server_secmode;
    uint64_t msg_id;
    uint64_t session_id;
    uint32_t tree_id;
    uint32_t max_read;
    uint32_t credits;       // what the server has granted and we have not spent
    uint64_t server_time;   // the NEGOTIATE response's FILETIME, for NTLM's timestamp
    uint32_t last_status;
    int sign;               // sign requests, check responses
    int guest;
    uint8_t sign_key[16];
    uint8_t preauth[64];    // 3.1.1's running SHA-512: the connection's, then the session's
    uint8_t client_guid[16];
    char server[256];
    uint8_t *tx;            // USMB_TX_CAP
    uint8_t *rx;            // USMB_RX_CAP: the last response, header first
    size_t rx_len;
};

static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static inline void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static inline uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

// SaunaOS text (UTF-8; a stray byte is its Latin-1 letter) -> UTF-16LE,
// '/' optionally turned into '\'. Bytes written, or -1 if it does not fit.
int usmb_to_utf16(const char *s, uint8_t *out, size_t cap, int slashes);
// UTF-16LE -> UTF-8, NUL-terminated. -1 if it does not fit.
int usmb_from_utf16(const uint8_t *in, size_t bytes, char *out, size_t cap);

// ---- usmb_auth.c ----------------------------------------------------

void usmb_md4(const void *data, size_t len, uint8_t out[16]);

// SPNEGO around an NTLMSSP token: the first (NegTokenInit) and later
// (NegTokenResp) client tokens. Bytes written, or -1.
int usmb_spnego_init(const uint8_t *tok, size_t n, uint8_t *out, size_t cap);
int usmb_spnego_resp(const uint8_t *tok, size_t n, uint8_t *out, size_t cap);
// The NTLMSSP token inside a server's SPNEGO blob (or the blob itself,
// if it is raw NTLMSSP). 0 with *tok/*n set, or -1; -2 if the server's
// negState is reject.
int usmb_spnego_token(const uint8_t *blob, size_t n, const uint8_t **tok, size_t *tn);

int usmb_ntlm_negotiate(uint8_t *out, size_t cap);

// The AUTHENTICATE message for `challenge`, and the session key it
// establishes (zeroed, and *has_key 0, for an anonymous login).
struct usmb_ntlm_in {
    const uint8_t *challenge; size_t challenge_len;
    const char *user, *domain;          // user NULL/"" = anonymous
    const uint8_t *nt_hash;             // 16 bytes
    uint64_t fallback_time;             // FILETIME, when the server sends no timestamp
    const struct usmb_io *io;           // for the client challenge and session key
};
int usmb_ntlm_authenticate(const struct usmb_ntlm_in *in, uint8_t *out, size_t cap,
                           uint8_t session_key[16], int *has_key);

// The signing key for `dialect` from the session key (and, for 3.1.1,
// the pre-authentication hash).
void usmb_signing_key(uint16_t dialect, const uint8_t session_key[16],
                      const uint8_t preauth[64], uint8_t out[16]);
// Writes the signature into the message (header+body) in place.
void usmb_sign(uint16_t dialect, const uint8_t key[16], uint8_t *msg, size_t len);
// 1 if the message's signature is right. Leaves the message unchanged.
int usmb_verify(uint16_t dialect, const uint8_t key[16], uint8_t *msg, size_t len);
void usmb_preauth_update(uint8_t h[64], const uint8_t *msg, size_t len);

#endif
