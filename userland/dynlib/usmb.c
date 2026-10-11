// <usmb.h> -- the SMB2/3 conversation: negotiate, session setup, tree
// connect, and the five read-side commands. MS-SMB2 2.2 has every
// structure; offsets below are from the start of a command's body,
// which follows the 64-byte header.
#include <stdlib.h>
#include <string.h>

#include "usmb_internal.h"

enum {
    CMD_NEGOTIATE = 0, CMD_SESSION_SETUP = 1, CMD_LOGOFF = 2, CMD_TREE_CONNECT = 3,
    CMD_CREATE = 5, CMD_CLOSE = 6, CMD_READ = 8, CMD_QUERY_DIRECTORY = 0x0e,
    CMD_QUERY_INFO = 0x10,
};

#define FLAG_SERVER_TO_REDIR 0x1u
#define FLAG_ASYNC           0x2u
#define FLAG_SIGNED          0x8u

#define ST_SUCCESS          0x00000000u
#define ST_PENDING          0x00000103u
#define ST_NO_MORE_FILES    0x80000006u
#define ST_MORE_PROCESSING  0xC0000016u
#define ST_END_OF_FILE      0xC0000011u

#define SECMODE_SIGNING_ENABLED  0x1
#define SECMODE_SIGNING_REQUIRED 0x2

#define SESSION_IS_GUEST     0x1
#define SESSION_IS_NULL      0x2
#define SESSION_ENCRYPT_DATA 0x4
#define SHARE_ENCRYPT_DATA   0x8000u

#define ATTR_HIDDEN    0x2u
#define ATTR_DIRECTORY 0x10u

// Seconds between 1601-01-01 (FILETIME's epoch) and 1970-01-01.
#define FILETIME_UNIX_DIFF 11644473600ull

static const uint16_t DIALECTS[] = { 0x0202, 0x0210, 0x0300, 0x0302, 0x0311 };

const char *usmb_strerror(int err) {
    switch (err) {
    case USMB_OK:         return "ok";
    case USMB_E_NET:      return "connection failed";
    case USMB_E_TIMEOUT:  return "no answer from the server";
    case USMB_E_PROTO:    return "the server's reply made no sense";
    case USMB_E_LOGON:    return "wrong user name or password";
    case USMB_E_ACCESS:   return "access denied";
    case USMB_E_NOENT:    return "no such file or folder";
    case USMB_E_NOSHARE:  return "no such share on that server";
    case USMB_E_NOTDIR:   return "not a folder";
    case USMB_E_ISDIR:    return "is a folder";
    case USMB_E_NOMEM:    return "out of memory";
    case USMB_E_SIGN:     return "a reply failed its signature check";
    case USMB_E_ENCRYPT:  return "the share requires encryption, which this client does not do";
    case USMB_E_NEEDSIGN: return "the server requires signing, which a guest login cannot do";
    case USMB_E_DIALECT:  return "no SMB2 or SMB3 dialect in common (an SMB1-only server?)";
    case USMB_E_RESOLVE:  return "the server's name did not resolve";
    case USMB_E_SHAREDENIED: return "access to the share denied (or it requires encryption)";
    default:              return "the server refused";
    }
}

// NTSTATUS -> USMB_E_*, for the codes a read-only client meets. Anything
// else is USMB_E_OTHER, with the code kept for usmb_last_status().
static const struct { uint32_t status; int err; } STATUS_MAP[] = {
    { 0xC000006D, USMB_E_LOGON },   // LOGON_FAILURE
    { 0xC000006E, USMB_E_LOGON },   // ACCOUNT_RESTRICTION
    { 0xC0000071, USMB_E_LOGON },   // PASSWORD_EXPIRED
    { 0xC0000072, USMB_E_LOGON },   // ACCOUNT_DISABLED
    { 0xC0000064, USMB_E_LOGON },   // NO_SUCH_USER
    { 0xC000015B, USMB_E_LOGON },   // LOGON_TYPE_NOT_GRANTED
    { 0xC0000193, USMB_E_LOGON },   // ACCOUNT_EXPIRED
    { 0xC0000234, USMB_E_LOGON },   // ACCOUNT_LOCKED_OUT
    { 0xC0000022, USMB_E_ACCESS },  // ACCESS_DENIED
    { 0xC0000034, USMB_E_NOENT },   // OBJECT_NAME_NOT_FOUND
    { 0xC000003A, USMB_E_NOENT },   // OBJECT_PATH_NOT_FOUND
    { 0xC000000F, USMB_E_NOENT },   // NO_SUCH_FILE
    { 0xC0000033, USMB_E_NOENT },   // OBJECT_NAME_INVALID
    { 0xC00000CC, USMB_E_NOSHARE }, // BAD_NETWORK_NAME
    { 0xC0000103, USMB_E_NOTDIR },  // NOT_A_DIRECTORY
    { 0xC00000BA, USMB_E_ISDIR },   // FILE_IS_A_DIRECTORY
    // The session or share is gone: the caller reconnects.
    { 0xC0000203, USMB_E_NET },     // USER_SESSION_DELETED
    { 0xC00000C9, USMB_E_NET },     // NETWORK_NAME_DELETED
    { 0xC000035C, USMB_E_NET },     // NETWORK_SESSION_EXPIRED
    { 0xC000020C, USMB_E_NET },     // CONNECTION_DISCONNECTED
};

static int map_status(struct usmb *s, uint32_t st) {
    s->last_status = st;
    if (st == ST_SUCCESS) return USMB_OK;
    for (size_t i = 0; i < sizeof STATUS_MAP / sizeof STATUS_MAP[0]; i++)
        if (STATUS_MAP[i].status == st) return STATUS_MAP[i].err;
    return USMB_E_OTHER;
}

// ---- toy-os text <-> UTF-16LE ------------------------------------------
//
// TOY-OS TEXT IS LATIN-1 (docs/decisions/drivers.md): a character is one
// byte, and the font, the keyboard and every file name agree. So a name
// comes back as Latin-1 when Latin-1 can spell it -- and as UTF-8 when it
// cannot, which displays as gibberish but still names the file, where a
// '?' in its place would make it unreachable. Going the other way, a
// string that is valid UTF-8 AND holds a code point above U+00FF is read
// as UTF-8 (one of those names, handed back); anything else is Latin-1.

// The next UTF-8 code point at *p, or -1 for a malformed sequence.
static int32_t utf8_next(const unsigned char **pp) {
    const unsigned char *p = *pp;
    int32_t c;
    int n;
    if (*p < 0x80) { c = *p; n = 0; }
    else if ((*p & 0xe0) == 0xc0) { c = *p & 0x1f; n = 1; }
    else if ((*p & 0xf0) == 0xe0) { c = *p & 0x0f; n = 2; }
    else if ((*p & 0xf8) == 0xf0) { c = *p & 0x07; n = 3; }
    else return -1;
    p++;
    for (int i = 0; i < n; i++, p++) {
        if ((*p & 0xc0) != 0x80) return -1;
        c = c << 6 | (*p & 0x3f);
    }
    *pp = p;
    return c;
}

// Is `s` a name only UTF-8 can spell? Valid throughout, and past Latin-1.
static int needs_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    int wide = 0;
    while (*p) {
        int32_t c = utf8_next(&p);
        if (c < 0) return 0;
        if (c > 0xff) wide = 1;
    }
    return wide;
}

int usmb_to_utf16(const char *s, uint8_t *out, size_t cap, int slashes) {
    size_t o = 0;
    int utf8 = needs_utf8(s);
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = utf8 ? (uint32_t)utf8_next(&p) : *p++;
        if (slashes && c == '/') c = '\\';
        if (c >= 0x10000) {
            if (o + 4 > cap) return -1;
            c -= 0x10000;
            put16(out + o, (uint16_t)(0xd800 | (c >> 10)));
            put16(out + o + 2, (uint16_t)(0xdc00 | (c & 0x3ff)));
            o += 4;
        } else {
            if (o + 2 > cap) return -1;
            put16(out + o, (uint16_t)c);
            o += 2;
        }
    }
    return (int)o;
}

// The code point at in[i] (a surrogate pair is one), advancing *i.
static uint32_t utf16_next(const uint8_t *in, size_t bytes, size_t *i) {
    uint32_t c = get16(in + *i);
    *i += 2;
    if (c >= 0xd800 && c < 0xdc00 && *i + 1 < bytes) {
        uint32_t lo = get16(in + *i);
        if (lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); *i += 2; }
    }
    return c;
}

int usmb_from_utf16(const uint8_t *in, size_t bytes, char *out, size_t cap) {
    int wide = 0;
    for (size_t i = 0; i + 1 < bytes;) if (utf16_next(in, bytes, &i) > 0xff) wide = 1;
    size_t o = 0;
    for (size_t i = 0; i + 1 < bytes;) {
        uint32_t c = utf16_next(in, bytes, &i);
        uint8_t b[4];
        int n;
        if (!wide || c < 0x80) { b[0] = (uint8_t)c; n = 1; }
        else if (c < 0x800) { b[0] = (uint8_t)(0xc0 | c >> 6); b[1] = (uint8_t)(0x80 | (c & 0x3f)); n = 2; }
        else if (c < 0x10000) { b[0] = (uint8_t)(0xe0 | c >> 12); b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3f)); b[2] = (uint8_t)(0x80 | (c & 0x3f)); n = 3; }
        else { b[0] = (uint8_t)(0xf0 | c >> 18); b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3f)); b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3f)); b[3] = (uint8_t)(0x80 | (c & 0x3f)); n = 4; }
        if (o + (size_t)n + 1 > cap) return -1;
        memcpy(out + o, b, (size_t)n);
        o += (size_t)n;
    }
    out[o] = '\0';
    return (int)o;
}

static uint64_t ft_to_unix(uint64_t ft) {
    uint64_t secs = ft / 10000000ull;
    return secs > FILETIME_UNIX_DIFF ? secs - FILETIME_UNIX_DIFF : 0;
}

// ---- the transport: NetBIOS session framing over TCP ----------------

static int recv_all(struct usmb *s, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = s->io.recv(s->io.ctx, buf + got, n - got, s->timeout_ms);
        if (r == 0) return USMB_E_TIMEOUT;
        if (r < 0) return USMB_E_NET;
        got += (size_t)r;
    }
    return 0;
}

// One SMB2 message into s->rx. Skips NetBIOS keep-alives.
static int recv_msg(struct usmb *s) {
    for (;;) {
        uint8_t nb[4];
        int rc = recv_all(s, nb, 4);
        if (rc) return rc;
        size_t len = (size_t)nb[1] << 16 | (size_t)nb[2] << 8 | nb[3];
        if (nb[0] == 0x85 && len == 0) continue;
        if (nb[0] != 0 || len < USMB_HDR || len > USMB_RX_CAP) return USMB_E_PROTO;
        rc = recv_all(s, s->rx, len);
        if (rc) return rc;
        if (memcmp(s->rx, "\xfeSMB", 4)) return USMB_E_PROTO;
        s->rx_len = len;
        return 0;
    }
}

// Fill in the header for `cmd` at s->tx; the body follows it.
static void header(struct usmb *s, uint16_t cmd) {
    uint8_t *h = s->tx;
    memset(h, 0, USMB_HDR);
    memcpy(h, "\xfeSMB", 4);
    put16(h + 4, USMB_HDR);
    put16(h + 6, s->dialect > 0x0202 ? 1 : 0);   // CreditCharge
    put16(h + 12, cmd);
    put16(h + 14, 64);                           // credits asked for
    put64(h + 24, s->msg_id);
    put32(h + 36, s->tree_id);
    put64(h + 40, s->session_id);
}

// Send s->tx (header + `body` bytes) and wait for its final response.
// Returns the NTSTATUS (>= 0 as an unsigned), or a negative USMB_E_*;
// the response is in s->rx, its body at s->rx + USMB_HDR.
static int64_t transact(struct usmb *s, size_t body, int sign) {
    size_t len = USMB_HDR + body;
    uint64_t id = s->msg_id++;
    if (sign) usmb_sign(s->dialect, s->sign_key, s->tx, len);
    uint8_t nb[4] = { 0, (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
    if (s->io.send(s->io.ctx, nb, 4) < 0 || s->io.send(s->io.ctx, s->tx, len) < 0)
        return USMB_E_NET;
    for (;;) {
        int rc = recv_msg(s);
        if (rc) return rc;
        const uint8_t *h = s->rx;
        uint32_t flags = get32(h + 16);
        if (!(flags & FLAG_SERVER_TO_REDIR) || get64(h + 24) != id) continue; // not ours (an oplock break)
        uint32_t st = get32(h + 8);
        if (st == ST_PENDING && (flags & FLAG_ASYNC)) continue;               // interim: the answer follows
        if (s->sign) {
            if (!(flags & FLAG_SIGNED)) return USMB_E_SIGN;
            if (!usmb_verify(s->dialect, s->sign_key, s->rx, s->rx_len)) return USMB_E_SIGN;
        }
        return st;
    }
}

// A response body of at least `min` bytes, or USMB_E_PROTO.
static const uint8_t *body_of(struct usmb *s, size_t min) {
    if (s->rx_len < USMB_HDR + min) return NULL;
    return s->rx + USMB_HDR;
}

// A (offset-from-header, length) buffer named by a response, checked
// against what actually arrived.
static const uint8_t *buffer_of(struct usmb *s, uint32_t off, uint32_t len) {
    if (off > s->rx_len || len > s->rx_len - off) return NULL;
    return s->rx + off;
}

// ---- negotiate and session setup ------------------------------------

static int negotiate(struct usmb *s) {
    header(s, CMD_NEGOTIATE);
    put16(s->tx + 6, 0);                    // CreditCharge is 0 before a dialect is chosen
    uint8_t *b = s->tx + USMB_HDR;
    int nd = (int)(sizeof DIALECTS / sizeof DIALECTS[0]);
    memset(b, 0, 36);
    put16(b, 36);
    put16(b + 2, (uint16_t)nd);
    put16(b + 4, SECMODE_SIGNING_ENABLED);
    memcpy(b + 12, s->client_guid, 16);
    for (int i = 0; i < nd; i++) put16(b + 36 + 2 * i, DIALECTS[i]);
    size_t len = 36 + 2 * (size_t)nd;
    // 3.1.1's one REQUIRED context: pre-authentication integrity, SHA-512
    // with a 32-byte salt. 8-aligned from the start of the header.
    while ((USMB_HDR + len) % 8) b[len++] = 0;
    put32(b + 28, (uint32_t)(USMB_HDR + len));  // NegotiateContextOffset
    put16(b + 32, 1);                           // NegotiateContextCount
    uint8_t *c = b + len;
    memset(c, 0, 8 + 38);
    put16(c, 1);                                // SMB2_PREAUTH_INTEGRITY_CAPABILITIES
    put16(c + 2, 38);
    put16(c + 8, 1);                            // one algorithm
    put16(c + 10, 32);                          // salt length
    put16(c + 12, 1);                           // SHA-512
    s->io.random(s->io.ctx, c + 14, 32);
    len += 8 + 38;

    memset(s->preauth, 0, sizeof s->preauth);
    usmb_preauth_update(s->preauth, s->tx, USMB_HDR + len);
    int64_t st = transact(s, len, 0);
    if (st < 0) return (int)st;
    if (st != ST_SUCCESS) return st == 0xC00000BB ? USMB_E_DIALECT : map_status(s, (uint32_t)st);
    const uint8_t *r = body_of(s, 64);
    if (!r) return USMB_E_PROTO;
    s->server_secmode = get16(r + 2);
    s->dialect = get16(r + 4);
    s->max_read = get32(r + 32);
    s->server_time = get64(r + 40);
    int known = 0;
    for (int i = 0; i < nd; i++) known |= s->dialect == DIALECTS[i];
    if (!known) return USMB_E_DIALECT;
    if (s->dialect == 0x0311) usmb_preauth_update(s->preauth, s->rx, s->rx_len);
    if (s->max_read > USMB_READ_MAX) s->max_read = USMB_READ_MAX;
    if (s->max_read < 4096) s->max_read = 4096;
    return 0;
}

// One SESSION_SETUP leg carrying `blob`. Returns the NTSTATUS or an
// error; the server's security blob is left at *out/*outn.
static int64_t session_leg(struct usmb *s, const uint8_t *blob, size_t n,
                           const uint8_t **out, size_t *outn) {
    if (24 + n > USMB_TX_CAP - USMB_HDR) return USMB_E_PROTO;
    header(s, CMD_SESSION_SETUP);
    uint8_t *b = s->tx + USMB_HDR;
    memset(b, 0, 24);
    put16(b, 25);
    b[3] = SECMODE_SIGNING_ENABLED;
    put16(b + 12, USMB_HDR + 24);
    put16(b + 14, (uint16_t)n);
    memcpy(b + 24, blob, n);
    if (s->dialect == 0x0311) usmb_preauth_update(s->preauth, s->tx, USMB_HDR + 24 + n);
    int64_t st = transact(s, 24 + n, 0);
    if (st < 0) return st;
    if (st != ST_SUCCESS && st != ST_MORE_PROCESSING) return st;
    const uint8_t *r = body_of(s, 8);
    if (!r) return USMB_E_PROTO;
    *out = buffer_of(s, get16(r + 4), get16(r + 6));
    *outn = get16(r + 6);
    if (!*out) return USMB_E_PROTO;
    s->session_id = get64(s->rx + 40);
    // Every response but the last (successful) one joins the hash.
    if (s->dialect == 0x0311 && st == ST_MORE_PROCESSING)
        usmb_preauth_update(s->preauth, s->rx, s->rx_len);
    return st;
}

static int session_setup(struct usmb *s, const struct usmb_auth *a) {
    uint8_t tok[64], blob[2048];
    int tn = usmb_ntlm_negotiate(tok, sizeof tok);
    int bn = usmb_spnego_init(tok, (size_t)tn, blob, sizeof blob);
    if (tn < 0 || bn < 0) return USMB_E_PROTO;
    const uint8_t *sb, *chal;
    size_t sbn, chn;
    int64_t st = session_leg(s, blob, (size_t)bn, &sb, &sbn);
    if (st < 0) return (int)st;
    if (st != ST_MORE_PROCESSING) return map_status(s, (uint32_t)st);
    if (usmb_spnego_token(sb, sbn, &chal, &chn)) return USMB_E_PROTO;

    // The challenge lives in s->rx, which the next leg overwrites.
    uint8_t *challenge = malloc(chn);
    uint8_t *auth = malloc(USMB_TX_CAP / 2);
    if (!challenge || !auth) { free(challenge); free(auth); return USMB_E_NOMEM; }
    memcpy(challenge, chal, chn);
    struct usmb_ntlm_in in = {
        .challenge = challenge, .challenge_len = chn,
        .user = a ? a->user : NULL, .domain = a ? a->domain : NULL,
        .nt_hash = a ? a->nt_hash : (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
        .fallback_time = s->server_time, .io = &s->io,
    };
    uint8_t session_key[16];
    int has_key = 0;
    int an = usmb_ntlm_authenticate(&in, auth, USMB_TX_CAP / 2, session_key, &has_key);
    free(challenge);
    if (an < 0) { free(auth); return USMB_E_PROTO; }
    uint8_t *blob2 = malloc(USMB_TX_CAP / 2 + 32);
    int bn2 = blob2 ? usmb_spnego_resp(auth, (size_t)an, blob2, USMB_TX_CAP / 2 + 32) : -1;
    free(auth);
    if (bn2 < 0) { free(blob2); return blob2 ? USMB_E_PROTO : USMB_E_NOMEM; }
    st = session_leg(s, blob2, (size_t)bn2, &sb, &sbn);
    free(blob2);
    if (st < 0) return (int)st;
    if (st != ST_SUCCESS) return map_status(s, (uint32_t)st);
    if (!sbn) { /* nothing more from SPNEGO -- fine */ }
    else if (usmb_spnego_token(sb, sbn, &chal, &chn) == -2) return USMB_E_LOGON;

    const uint8_t *r = body_of(s, 8);
    uint16_t sflags = r ? get16(r + 2) : 0;
    if (sflags & SESSION_ENCRYPT_DATA) return USMB_E_ENCRYPT;
    s->guest = !!(sflags & (SESSION_IS_GUEST | SESSION_IS_NULL));
    if (!has_key || s->guest) {
        // MS-SMB2 3.2.5.3.1: no key, no signing -- and a server that
        // insists on signing cannot be served.
        if (s->server_secmode & SECMODE_SIGNING_REQUIRED) return USMB_E_NEEDSIGN;
        return 0;
    }
    usmb_signing_key(s->dialect, session_key, s->preauth, s->sign_key);
    memset(session_key, 0, sizeof session_key);
    // 3.1.1 signs the final SESSION_SETUP response; check it now that
    // the key exists. Earlier dialects need not, and some do not.
    if (s->dialect == 0x0311 && (get32(s->rx + 16) & FLAG_SIGNED) &&
        !usmb_verify(s->dialect, s->sign_key, s->rx, s->rx_len))
        return USMB_E_SIGN;
    s->sign = 1;
    return 0;
}

static int tree_connect(struct usmb *s, const char *share) {
    header(s, CMD_TREE_CONNECT);
    uint8_t *b = s->tx + USMB_HDR;
    memset(b, 0, 8);
    put16(b, 9);
    int n = usmb_to_utf16("\\\\", b + 8, 8, 0);
    int m = usmb_to_utf16(s->server, b + 8 + n, 1024, 0);
    if (m < 0) return USMB_E_PROTO;
    n += m;
    m = usmb_to_utf16("\\", b + 8 + n, 4, 0);
    n += m;
    m = usmb_to_utf16(share, b + 8 + n, 1024, 0);
    if (m < 0) return USMB_E_PROTO;
    n += m;
    put16(b + 4, USMB_HDR + 8);
    put16(b + 6, (uint16_t)n);
    int64_t st = transact(s, 8 + (size_t)n, s->sign);
    if (st < 0) return (int)st;
    // MS-SMB2 3.3.5.7: a share that requires encryption answers a client
    // that cannot encrypt with ACCESS_DENIED -- indistinguishable from a
    // share this user may not open, so the error says both.
    if (st == 0xC0000022) { s->last_status = (uint32_t)st; return USMB_E_SHAREDENIED; }
    if (st != ST_SUCCESS) return map_status(s, (uint32_t)st);
    const uint8_t *r = body_of(s, 16);
    if (!r) return USMB_E_PROTO;
    if (get32(r + 4) & SHARE_ENCRYPT_DATA) return USMB_E_ENCRYPT;
    s->tree_id = get32(s->rx + 36);
    return 0;
}

int usmb_open(struct usmb **out, const struct usmb_io *io,
              const char *server, const char *share,
              const struct usmb_auth *auth, unsigned timeout_ms) {
    *out = NULL;
    struct usmb *s = calloc(1, sizeof *s);
    if (s) { s->tx = malloc(USMB_TX_CAP); s->rx = malloc(USMB_RX_CAP); }
    if (!s || !s->tx || !s->rx) {
        if (s) { free(s->tx); free(s->rx); free(s); }
        io->close(io->ctx);
        return USMB_E_NOMEM;
    }
    s->io = *io;
    s->timeout_ms = timeout_ms ? timeout_ms : 10000;
    size_t sl = strlen(server);
    if (sl >= sizeof s->server) sl = sizeof s->server - 1;
    memcpy(s->server, server, sl);
    s->io.random(s->io.ctx, s->client_guid, 16);
    int rc = negotiate(s);
    if (!rc) rc = session_setup(s, auth);
    if (!rc) rc = tree_connect(s, share);
    if (rc) { usmb_close(s); return rc; }
    *out = s;
    return 0;
}

void usmb_close(struct usmb *s) {
    if (!s) return;
    if (s->session_id) {
        // Best effort: a broken connection just closes.
        unsigned t = s->timeout_ms;
        s->timeout_ms = 1000;
        header(s, CMD_LOGOFF);
        put16(s->tx + USMB_HDR, 4);
        put16(s->tx + USMB_HDR + 2, 0);
        (void)transact(s, 4, s->sign);
        s->timeout_ms = t;
    }
    s->io.close(s->io.ctx);
    memset(s->sign_key, 0, sizeof s->sign_key);
    free(s->tx);
    free(s->rx);
    free(s);
}

void usmb_get_info(const struct usmb *s, struct usmb_info *out) {
    out->dialect = s->dialect;
    out->signing = s->sign;
    out->guest = s->guest;
    out->max_read = s->max_read;
}

uint32_t usmb_last_status(const struct usmb *s) { return s->last_status; }

// ---- files ----------------------------------------------------------

#define ACCESS_READ_ATTRS 0x00100080u   // FILE_READ_ATTRIBUTES | SYNCHRONIZE
#define ACCESS_READ       0x00120089u   // GENERIC_READ's parts
#define OPT_DIRECTORY     0x00000001u
#define OPT_NON_DIRECTORY 0x00000040u

static void stat_from(const uint8_t *created, const uint8_t *written, uint64_t size,
                      uint32_t attrs, struct usmb_stat *st) {
    st->created = ft_to_unix(get64(created));
    st->modified = ft_to_unix(get64(written));
    st->size = size;
    st->is_dir = !!(attrs & ATTR_DIRECTORY);
    st->hidden = !!(attrs & ATTR_HIDDEN);
}

// Strip leading slashes: the server wants a share-relative name.
static const char *rel(const char *path) {
    while (path && *path == '/') path++;
    return path ? path : "";
}

static int create(struct usmb *s, const char *path, uint32_t access, uint32_t options,
                  uint8_t id[16], struct usmb_stat *st) {
    header(s, CMD_CREATE);
    uint8_t *b = s->tx + USMB_HDR;
    memset(b, 0, 56);
    put16(b, 57);
    put32(b + 4, 2);                        // impersonation: Impersonation
    put32(b + 24, access);
    put32(b + 32, 7);                       // share read, write and delete
    put32(b + 36, 1);                       // FILE_OPEN: never create
    put32(b + 40, options);
    int n = usmb_to_utf16(rel(path), b + 56, USMB_TX_CAP - USMB_HDR - 56, 1);
    if (n < 0) return USMB_E_NOENT;
    // A trailing separator names the same thing; servers refuse it.
    while (n >= 2 && get16(b + 56 + n - 2) == '\\') n -= 2;
    put16(b + 44, USMB_HDR + 56);
    put16(b + 46, (uint16_t)n);
    size_t len = 56 + (n ? (size_t)n : 1);  // the buffer is never empty
    if (!n) b[56] = 0;
    int64_t rc = transact(s, len, s->sign);
    if (rc < 0) return (int)rc;
    if (rc != ST_SUCCESS) return map_status(s, (uint32_t)rc);
    const uint8_t *r = body_of(s, 88);
    if (!r) return USMB_E_PROTO;
    memcpy(id, r + 64, 16);
    if (st) stat_from(r + 8, r + 24, get64(r + 48), get32(r + 56), st);
    return 0;
}

static int close_id(struct usmb *s, const uint8_t id[16]) {
    header(s, CMD_CLOSE);
    uint8_t *b = s->tx + USMB_HDR;
    memset(b, 0, 24);
    put16(b, 24);
    memcpy(b + 8, id, 16);
    int64_t rc = transact(s, 24, s->sign);
    if (rc < 0) return (int)rc;
    return rc == ST_SUCCESS ? 0 : map_status(s, (uint32_t)rc);
}

int usmb_stat(struct usmb *s, const char *path, struct usmb_stat *out) {
    uint8_t id[16];
    int rc = create(s, path, ACCESS_READ_ATTRS, 0, id, out);
    if (rc) return rc;
    return close_id(s, id);
}

int usmb_list(struct usmb *s, const char *path, usmb_list_cb cb, void *ctx) {
    uint8_t id[16];
    int rc = create(s, path, ACCESS_READ, OPT_DIRECTORY, id, NULL);
    if (rc) return rc;
    char *name = malloc(1024);
    if (!name) { close_id(s, id); return USMB_E_NOMEM; }
    int stop = 0;
    for (int first = 1; !stop; first = 0) {
        header(s, CMD_QUERY_DIRECTORY);
        uint8_t *b = s->tx + USMB_HDR;
        memset(b, 0, 32);
        put16(b, 33);
        b[2] = 1;                           // FileDirectoryInformation
        b[3] = first ? 1 : 0;               // RESTART_SCANS
        memcpy(b + 8, id, 16);
        put16(b + 24, USMB_HDR + 32);
        put16(b + 26, 2);
        put32(b + 28, USMB_READ_MAX);
        put16(b + 32, '*');
        int64_t st = transact(s, 34, s->sign);
        if (st == ST_NO_MORE_FILES) break;
        if (st < 0) { rc = (int)st; break; }
        if (st != ST_SUCCESS) { rc = map_status(s, (uint32_t)st); break; }
        const uint8_t *r = body_of(s, 8);
        const uint8_t *buf = r ? buffer_of(s, get16(r + 2), get32(r + 4)) : NULL;
        if (!buf) { rc = USMB_E_PROTO; break; }
        uint32_t blen = get32(r + 4);
        for (uint32_t off = 0; off + 64 <= blen;) {
            const uint8_t *e = buf + off;
            uint32_t nl = get32(e + 60);
            if (64 + (uint64_t)nl > blen - off) { rc = USMB_E_PROTO; stop = 1; break; }
            if (usmb_from_utf16(e + 64, nl, name, 1024) >= 0 &&
                strcmp(name, ".") && strcmp(name, "..")) {
                struct usmb_stat es;
                stat_from(e + 8, e + 24, get64(e + 40), get32(e + 56), &es);
                if (cb(ctx, name, &es)) { stop = 1; break; }
            }
            uint32_t next = get32(e);
            if (!next) break;
            off += next;
        }
    }
    free(name);
    int crc = close_id(s, id);
    return rc ? rc : crc;
}

int usmb_open_file(struct usmb *s, const char *path, struct usmb_file *f) {
    struct usmb_stat st;
    int rc = create(s, path, ACCESS_READ, OPT_NON_DIRECTORY, f->id, &st);
    if (!rc) f->size = st.size;
    return rc;
}

int64_t usmb_read(struct usmb *s, const struct usmb_file *f,
                  uint64_t offset, void *buf, uint32_t len) {
    if (len > s->max_read) len = s->max_read;
    header(s, CMD_READ);
    uint8_t *b = s->tx + USMB_HDR;
    memset(b, 0, 49);
    put16(b, 49);
    b[2] = 0x50;                            // where the data lands: just past this body
    put32(b + 4, len);
    put64(b + 8, offset);
    memcpy(b + 16, f->id, 16);
    int64_t st = transact(s, 49, s->sign);
    if (st < 0) return st;
    if (st == ST_END_OF_FILE) return 0;
    if (st != ST_SUCCESS) return map_status(s, (uint32_t)st);
    const uint8_t *r = body_of(s, 16);
    const uint8_t *data = r ? buffer_of(s, r[2], get32(r + 4)) : NULL;
    if (!data) return USMB_E_PROTO;
    uint32_t n = get32(r + 4);
    if (n > len) return USMB_E_PROTO;
    memcpy(buf, data, n);
    return n;
}

int usmb_close_file(struct usmb *s, const struct usmb_file *f) { return close_id(s, f->id); }

int usmb_fs_size(struct usmb *s, uint64_t *total, uint64_t *avail) {
    uint8_t id[16];
    int rc = create(s, "", ACCESS_READ_ATTRS, 0, id, NULL);
    if (rc) return rc;
    header(s, CMD_QUERY_INFO);
    uint8_t *b = s->tx + USMB_HDR;
    memset(b, 0, 41);
    put16(b, 41);
    b[2] = 2;                               // SMB2_0_INFO_FILESYSTEM
    b[3] = 7;                               // FileFsFullSizeInformation
    put32(b + 4, 64);
    memcpy(b + 24, id, 16);
    int64_t st = transact(s, 41, s->sign);
    if (st < 0) rc = (int)st;
    else if (st != ST_SUCCESS) rc = map_status(s, (uint32_t)st);
    else {
        const uint8_t *r = body_of(s, 8);
        const uint8_t *d = r ? buffer_of(s, get16(r + 2), get32(r + 4)) : NULL;
        if (!d || get32(r + 4) < 32) rc = USMB_E_PROTO;
        else {
            uint64_t unit = (uint64_t)get32(d + 24) * get32(d + 28);
            *total = get64(d) * unit;
            *avail = get64(d + 8) * unit;
        }
    }
    int crc = close_id(s, id);
    return rc ? rc : crc;
}

// ---- addresses --------------------------------------------------------

static int copy_part(const char *from, size_t n, char *to, size_t cap) {
    if (n >= cap) return -1;
    memcpy(to, from, n);
    to[n] = '\0';
    return 0;
}

int usmb_parse_url(const char *url, char *server, size_t scap, uint16_t *port,
                   char *share, size_t shcap, char *path, size_t pcap) {
    const char *p = url;
    *port = 0;
    if (!strncmp(p, "smb://", 6)) p += 6;
    else if ((p[0] == '/' && p[1] == '/') || (p[0] == '\\' && p[1] == '\\')) p += 2;
    else return -1;
    size_t n = strcspn(p, "/\\:");
    if (!n || copy_part(p, n, server, scap)) return -1;
    p += n;
    if (*p == ':') {
        unsigned v = 0;
        p++;
        if (*p < '0' || *p > '9') return -1;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p++ - '0'); if (v > 65535) return -1; }
        *port = (uint16_t)v;
    }
    if (*p != '/' && *p != '\\') return -1;
    p++;
    n = strcspn(p, "/\\");
    if (!n || copy_part(p, n, share, shcap)) return -1;
    p += n;
    // The rest is the path inside the share, '/'-separated.
    size_t o = 0;
    if (!pcap) return -1;
    for (; *p; p++) {
        if (o + 2 > pcap) return -1;
        path[o++] = *p == '\\' ? '/' : *p;
    }
    path[o] = '\0';
    return 0;
}
