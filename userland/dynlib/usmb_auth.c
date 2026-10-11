// The SMB client's security half: NTLMv2 inside SPNEGO, the session's
// signing key, and the signatures themselves. MS-NLMP and MS-SMB2
// 3.1.4 are the references; the comments name the section where a
// choice is not obvious.
#include <stdlib.h>
#include <string.h>

#include "ucrypto.h"
#include "usmb_internal.h"

// ---- MD4 (RFC 1320) -------------------------------------------------
//
// HERE AND NOWHERE ELSE: the NT password hash is its only use, and
// mbedTLS 3 dropped it. Not exported -- nothing new should hash with it.

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define ROTL(v, s) (((v) << (s)) | ((v) >> (32 - (s))))

static void md4_block(uint32_t st[4], const uint8_t *p) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = get32(p + 4 * i);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    static const int r1[4] = { 3, 7, 11, 19 }, r2[4] = { 3, 5, 9, 13 }, r3[4] = { 3, 9, 11, 15 };
    static const int o3[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    for (int i = 0; i < 16; i++) {
        uint32_t t = a + F(b, c, d) + x[i];
        a = d; d = c; c = b; b = ROTL(t, r1[i % 4]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t t = a + G(b, c, d) + x[(i % 4) * 4 + i / 4] + 0x5a827999u;
        a = d; d = c; c = b; b = ROTL(t, r2[i % 4]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t t = a + H(b, c, d) + x[o3[i]] + 0x6ed9eba1u;
        a = d; d = c; c = b; b = ROTL(t, r3[i % 4]);
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void usmb_md4(const void *data, size_t len, uint8_t out[16]) {
    uint32_t st[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    const uint8_t *p = data;
    size_t left = len;
    for (; left >= 64; left -= 64, p += 64) md4_block(st, p);
    uint8_t tail[128];
    memset(tail, 0, sizeof tail);
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tl = left < 56 ? 64 : 128;
    put64(tail + tl - 8, (uint64_t)len * 8);
    md4_block(st, tail);
    if (tl == 128) md4_block(st, tail + 64);
    for (int i = 0; i < 4; i++) put32(out + 4 * i, st[i]);
}

// THE PASSWORD IS NOT KEPT. Up to 256 characters, which is past
// Windows' own limit; a longer one hashes its first 256.
void usmb_nt_hash(const char *password, uint8_t out[16]) {
    uint8_t u[512];
    int n = usmb_to_utf16(password ? password : "", u, sizeof u, 0);
    if (n < 0) n = 0;
    usmb_md4(u, (size_t)n, out);
    memset(u, 0, sizeof u);
}

// ---- RC4, for NTLM's KEY_EXCH only ----------------------------------

static void rc4(const uint8_t *key, size_t klen, const uint8_t *in, uint8_t *out, size_t n) {
    uint8_t s[256];
    for (int i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + s[i] + key[i % klen]) & 0xff;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
    }
    for (size_t k = 0, i = 0, j = 0; k < n; k++) {
        i = (i + 1) & 0xff;
        j = (j + s[i]) & 0xff;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
        out[k] = in[k] ^ s[(s[i] + s[j]) & 0xff];
    }
}

// ---- SPNEGO (RFC 4178), only as much DER as NTLM needs --------------

static const uint8_t OID_SPNEGO[] = { 0x06, 0x06, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x02 };
static const uint8_t OID_NTLMSSP[] = { 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01,
                                       0x82, 0x37, 0x02, 0x02, 0x0a };

static size_t der_len_size(size_t n) { return n < 0x80 ? 1 : n < 0x100 ? 2 : 3; }

// Writes a tag and length (contents to follow) at `p`; returns bytes.
static size_t der_hdr(uint8_t *p, uint8_t tag, size_t n) {
    p[0] = tag;
    if (n < 0x80) { p[1] = (uint8_t)n; return 2; }
    if (n < 0x100) { p[1] = 0x81; p[2] = (uint8_t)n; return 3; }
    p[1] = 0x82; p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n; return 4;
}

static size_t der_size(size_t n) { return 1 + der_len_size(n) + n; }

int usmb_spnego_init(const uint8_t *tok, size_t n, uint8_t *out, size_t cap) {
    size_t mechtypes = der_size(sizeof OID_NTLMSSP);            // SEQUENCE OF OID
    size_t a0 = der_size(mechtypes);
    size_t a2 = der_size(der_size(n));
    size_t seq = der_size(a0 + a2);
    size_t init = der_size(seq);                                // [0] NegTokenInit
    size_t app = der_size(sizeof OID_SPNEGO + init);            // [APPLICATION 0]
    if (app > cap || n > 0xfff0) return -1;
    uint8_t *p = out;
    p += der_hdr(p, 0x60, sizeof OID_SPNEGO + init);
    memcpy(p, OID_SPNEGO, sizeof OID_SPNEGO); p += sizeof OID_SPNEGO;
    p += der_hdr(p, 0xa0, seq);
    p += der_hdr(p, 0x30, a0 + a2);
    p += der_hdr(p, 0xa0, mechtypes);
    p += der_hdr(p, 0x30, sizeof OID_NTLMSSP);
    memcpy(p, OID_NTLMSSP, sizeof OID_NTLMSSP); p += sizeof OID_NTLMSSP;
    p += der_hdr(p, 0xa2, der_size(n));
    p += der_hdr(p, 0x04, n);
    memcpy(p, tok, n); p += n;
    return (int)(p - out);
}

int usmb_spnego_resp(const uint8_t *tok, size_t n, uint8_t *out, size_t cap) {
    size_t a2 = der_size(der_size(n));
    size_t seq = der_size(a2);
    size_t all = der_size(seq);
    if (all > cap || n > 0xfff0) return -1;
    uint8_t *p = out;
    p += der_hdr(p, 0xa1, seq);
    p += der_hdr(p, 0x30, a2);
    p += der_hdr(p, 0xa2, der_size(n));
    p += der_hdr(p, 0x04, n);
    memcpy(p, tok, n); p += n;
    return (int)(p - out);
}

// One TLV at *p (bounded by end): its tag and contents. Advances *p past
// it. 0, or -1 for anything malformed -- this parses the server's bytes.
static int der_next(const uint8_t **p, const uint8_t *end, uint8_t *tag,
                    const uint8_t **val, size_t *vlen) {
    const uint8_t *q = *p;
    if (end - q < 2) return -1;
    *tag = q[0];
    size_t n = q[1];
    q += 2;
    if (n & 0x80) {
        int k = (int)(n & 0x7f);
        if (k < 1 || k > 2 || end - q < k) return -1;
        n = 0;
        for (int i = 0; i < k; i++) n = n << 8 | *q++;
    }
    if ((size_t)(end - q) < n) return -1;
    *val = q;
    *vlen = n;
    *p = q + n;
    return 0;
}

int usmb_spnego_token(const uint8_t *blob, size_t n, const uint8_t **tok, size_t *tn) {
    if (n >= 8 && memcmp(blob, "NTLMSSP", 8) == 0) { *tok = blob; *tn = n; return 0; }
    const uint8_t *p = blob, *v;
    size_t vl;
    uint8_t tag;
    if (der_next(&p, blob + n, &tag, &v, &vl) || tag != 0xa1) return -1;
    p = v;
    const uint8_t *end = v + vl;
    if (der_next(&p, end, &tag, &v, &vl) || tag != 0x30) return -1;
    p = v;
    end = v + vl;
    int found = -1;
    while (p < end) {
        const uint8_t *fv;
        size_t fl;
        if (der_next(&p, end, &tag, &fv, &fl)) return -1;
        if (tag == 0xa0) {               // negState: ENUMERATED; 2 is reject
            const uint8_t *q = fv, *ev;
            size_t el;
            uint8_t et;
            if (!der_next(&q, fv + fl, &et, &ev, &el) && et == 0x0a && el == 1 && ev[0] == 2)
                return -2;
        } else if (tag == 0xa2) {        // responseToken: OCTET STRING
            const uint8_t *q = fv;
            uint8_t ot;
            if (der_next(&q, fv + fl, &ot, tok, tn) || ot != 0x04) return -1;
            found = 0;
        }
    }
    return found;
}

// ---- NTLMSSP (MS-NLMP 2.2.1) ----------------------------------------

#define NTLM_UNICODE        0x00000001u
#define NTLM_REQUEST_TARGET 0x00000004u
#define NTLM_SIGN           0x00000010u
#define NTLM_NTLM           0x00000200u
#define NTLM_ANONYMOUS      0x00000800u
#define NTLM_ALWAYS_SIGN    0x00008000u
#define NTLM_EXT_SESSSEC    0x00080000u
#define NTLM_TARGET_INFO    0x00800000u
#define NTLM_128            0x20000000u
#define NTLM_KEY_EXCH       0x40000000u
#define NTLM_56             0x80000000u

#define NTLM_WANT (NTLM_UNICODE | NTLM_REQUEST_TARGET | NTLM_SIGN | NTLM_NTLM | \
                   NTLM_ALWAYS_SIGN | NTLM_EXT_SESSSEC | NTLM_TARGET_INFO |   \
                   NTLM_128 | NTLM_KEY_EXCH | NTLM_56)

int usmb_ntlm_negotiate(uint8_t *out, size_t cap) {
    if (cap < 32) return -1;
    memset(out, 0, 32);
    memcpy(out, "NTLMSSP", 8);
    put32(out + 8, 1);
    put32(out + 12, NTLM_WANT);
    return 32;   // empty domain and workstation fields; no Version
}

// One (len, maxlen, offset) field pointing into the message's payload.
static void ntlm_field(uint8_t *msg, int at, size_t *off, const void *data, size_t n) {
    put16(msg + at, (uint16_t)n);
    put16(msg + at + 2, (uint16_t)n);
    put32(msg + at + 4, (uint32_t)*off);
    memcpy(msg + *off, data, n);
    *off += n;
}

// Off the stack: the ring-3 frame budget is 2 KiB and these are 3.
struct ntlm_scratch {
    uint8_t user16[512], dom16[512], upper16[512];
    uint8_t temp[28 + 1024 + 4];
    uint8_t nt_resp[16 + 28 + 1024 + 4];
};

static int authenticate(const struct usmb_ntlm_in *in, uint8_t *out, size_t cap,
                        uint8_t session_key[16], int *has_key, struct ntlm_scratch *sc);

int usmb_ntlm_authenticate(const struct usmb_ntlm_in *in, uint8_t *out, size_t cap,
                           uint8_t session_key[16], int *has_key) {
    struct ntlm_scratch *sc = malloc(sizeof *sc);
    memset(session_key, 0, 16);
    *has_key = 0;
    if (!sc) return -1;
    int rc = authenticate(in, out, cap, session_key, has_key, sc);
    memset(sc, 0, sizeof *sc);
    free(sc);
    return rc;
}

static int authenticate(const struct usmb_ntlm_in *in, uint8_t *out, size_t cap,
                        uint8_t session_key[16], int *has_key, struct ntlm_scratch *sc) {
    const uint8_t *c = in->challenge;
    size_t cl = in->challenge_len;
    if (cl < 48 || memcmp(c, "NTLMSSP", 8) || get32(c + 8) != 2) return -1;
    uint32_t flags = get32(c + 20) & NTLM_WANT;
    const uint8_t *server_chal = c + 24;
    uint16_t ti_len = get16(c + 40);
    uint32_t ti_off = get32(c + 44);
    if (ti_off > cl || ti_len > cl - ti_off) return -1;
    const uint8_t *ti = c + ti_off;

    // The target info's AV pairs: the server's NetBIOS domain (the
    // default domain to sign in to) and its timestamp, if it sent one.
    const uint8_t *nb_dom = NULL;
    size_t nb_dom_len = 0;
    uint64_t ts = in->fallback_time;
    int have_ts = 0;
    for (size_t p = 0; p + 4 <= ti_len;) {
        uint16_t id = get16(ti + p), len = get16(ti + p + 2);
        if (id == 0 || p + 4 + len > ti_len) break;
        if (id == 2) { nb_dom = ti + p + 4; nb_dom_len = len; }
        if (id == 7 && len == 8) { ts = get64(ti + p + 4); have_ts = 1; }
        p += 4 + (size_t)len;
    }

    uint8_t *user16 = sc->user16, *dom16 = sc->dom16, ws16[16];
    int anon = !in->user || !in->user[0];
    int ul = anon ? 0 : usmb_to_utf16(in->user, user16, sizeof sc->user16, 0);
    int dl;
    if (in->domain && in->domain[0]) dl = usmb_to_utf16(in->domain, dom16, sizeof sc->dom16, 0);
    else if (nb_dom_len <= sizeof sc->dom16) { memcpy(dom16, nb_dom, nb_dom_len); dl = (int)nb_dom_len; }
    else dl = -1;
    int wl = usmb_to_utf16("TOYOS", ws16, sizeof ws16, 0);
    if (ul < 0 || dl < 0 || wl < 0) return -1;

    // 64 bytes of fixed fields, then a payload of up to this much.
    size_t need = 64 + (size_t)dl + (size_t)ul + (size_t)wl + 24 + 48 + ti_len + 16;
    if (need > cap) return -1;
    memset(out, 0, 64);
    memcpy(out, "NTLMSSP", 8);
    put32(out + 8, 3);
    size_t off = 64;

    if (anon) {
        // MS-NLMP 3.1.5.1.2: an empty NT response and a one-byte LM one.
        static const uint8_t z = 0;
        flags |= NTLM_ANONYMOUS;
        flags &= ~NTLM_KEY_EXCH;
        ntlm_field(out, 28, &off, dom16, (size_t)dl);
        ntlm_field(out, 36, &off, user16, 0);
        ntlm_field(out, 44, &off, ws16, (size_t)wl);
        ntlm_field(out, 12, &off, &z, 1);
        ntlm_field(out, 20, &off, &z, 0);
        ntlm_field(out, 52, &off, &z, 0);
        put32(out + 60, flags);
        return (int)off;
    }

    // NTOWFv2 = HMAC-MD5(NT hash, UPPER(user) || domain), both UTF-16LE.
    // Upper-casing is ASCII-only: a non-ASCII user name signs in as typed.
    uint8_t *upper16 = sc->upper16;
    memcpy(upper16, user16, (size_t)ul);
    for (int i = 0; i + 1 < ul; i += 2)
        if (upper16[i + 1] == 0 && upper16[i] >= 'a' && upper16[i] <= 'z') upper16[i] -= 32;
    uint8_t owf[16];
    struct ucrypto_piece ow[2] = { { upper16, (size_t)ul }, { dom16, (size_t)dl } };
    ucrypto_hmac_md5(in->nt_hash, 16, ow, 2, owf);

    uint8_t cc[8];
    in->io->random(in->io->ctx, cc, sizeof cc);

    // temp = 1, 1, Z(6), time, client challenge, Z(4), target info, Z(4)
    uint8_t *temp = sc->temp;
    if (ti_len > 1024) return -1;
    memset(temp, 0, sizeof sc->temp);
    temp[0] = 1; temp[1] = 1;
    put64(temp + 8, ts);
    memcpy(temp + 16, cc, 8);
    memcpy(temp + 28, ti, ti_len);
    size_t temp_len = 28 + (size_t)ti_len + 4;

    uint8_t *nt_resp = sc->nt_resp;
    struct ucrypto_piece pr[2] = { { server_chal, 8 }, { temp, temp_len } };
    ucrypto_hmac_md5(owf, 16, pr, 2, nt_resp);              // NTProofStr
    memcpy(nt_resp + 16, temp, temp_len);

    uint8_t base_key[16];
    struct ucrypto_piece bk = { nt_resp, 16 };
    ucrypto_hmac_md5(owf, 16, &bk, 1, base_key);

    // LMv2 -- unless the server sent a timestamp, when MS-NLMP 3.1.5.1.2
    // says to send zeros instead.
    uint8_t lm[24];
    memset(lm, 0, sizeof lm);
    if (!have_ts) {
        struct ucrypto_piece lp[2] = { { server_chal, 8 }, { cc, 8 } };
        ucrypto_hmac_md5(owf, 16, lp, 2, lm);
        memcpy(lm + 16, cc, 8);
    }

    // KEY_EXCH: a fresh random session key, sent RC4'd under the base
    // key. Without it the base key IS the session key (NTLMv2's KXKEY).
    uint8_t enc_key[16];
    size_t enc_len = 0;
    if (flags & NTLM_KEY_EXCH) {
        in->io->random(in->io->ctx, session_key, 16);
        rc4(base_key, 16, session_key, enc_key, 16);
        enc_len = 16;
    } else {
        memcpy(session_key, base_key, 16);
    }
    *has_key = 1;

    ntlm_field(out, 28, &off, dom16, (size_t)dl);
    ntlm_field(out, 36, &off, user16, (size_t)ul);
    ntlm_field(out, 44, &off, ws16, (size_t)wl);
    ntlm_field(out, 12, &off, lm, 24);
    ntlm_field(out, 20, &off, nt_resp, 16 + temp_len);
    ntlm_field(out, 52, &off, enc_key, enc_len);
    put32(out + 60, flags);

    memset(owf, 0, sizeof owf);
    memset(base_key, 0, sizeof base_key);
    return (int)off;
}

// ---- keys and signatures (MS-SMB2 3.1.4.1, 3.2.5.3) -----------------

// SP800-108 in counter mode, HMAC-SHA256, one block, L = 128.
static void kdf(const uint8_t key[16], const void *label, size_t llen,
                const void *ctx, size_t clen, uint8_t out[16]) {
    static const uint8_t one[4] = { 0, 0, 0, 1 }, zero = 0, bits[4] = { 0, 0, 0, 128 };
    struct ucrypto_piece v[5] = { { one, 4 }, { label, llen }, { &zero, 1 },
                                  { ctx, clen }, { bits, 4 } };
    uint8_t full[32];
    ucrypto_hmac_sha256(key, 16, v, 5, full);
    memcpy(out, full, 16);
}

void usmb_signing_key(uint16_t dialect, const uint8_t session_key[16],
                      const uint8_t preauth[64], uint8_t out[16]) {
    if (dialect < 0x0300) memcpy(out, session_key, 16);
    else if (dialect < 0x0311) kdf(session_key, "SMB2AESCMAC", 12, "SmbSign", 8, out);
    else kdf(session_key, "SMBSigningKey", 14, preauth, 64, out);
}

static void mac(uint16_t dialect, const uint8_t key[16], const uint8_t *msg, size_t len,
                uint8_t out[16]) {
    static const uint8_t zero16[16];
    // The signature field (header bytes 48..63) counts as zero.
    struct ucrypto_piece v[3] = { { msg, 48 }, { zero16, 16 }, { msg + 64, len - 64 } };
    if (dialect < 0x0300) {
        uint8_t full[32];
        ucrypto_hmac_sha256(key, 16, v, 3, full);
        memcpy(out, full, 16);
    } else {
        ucrypto_aes128_cmac(key, v, 3, out);
    }
}

void usmb_sign(uint16_t dialect, const uint8_t key[16], uint8_t *msg, size_t len) {
    put32(msg + 16, get32(msg + 16) | 0x8);   // SMB2_FLAGS_SIGNED
    mac(dialect, key, msg, len, msg + 48);
}

int usmb_verify(uint16_t dialect, const uint8_t key[16], uint8_t *msg, size_t len) {
    uint8_t want[16];
    mac(dialect, key, msg, len, want);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ msg[48 + i]);
    return diff == 0;
}

void usmb_preauth_update(uint8_t h[64], const uint8_t *msg, size_t len) {
    struct ucrypto_piece v[2] = { { h, 64 }, { msg, len } };
    uint8_t next[64];
    ucrypto_sha512(v, 2, next);
    memcpy(h, next, 64);
}
