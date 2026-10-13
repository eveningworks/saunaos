#ifndef ULIB_UTLS_H
#define ULIB_UTLS_H

// A TLS session over a connected socket -- a client's, or (below) a
// remote desktop server's.
//
// **THIS HEADER NAMES NO ENGINE.** It is implemented today by mbedTLS
// (`userland/backends/mbedtls/utls_mbedtls.c`, built into
// /lib/libssl.so) and mentions none of its types, so replacing the
// engine is a backend change rather than a change to every caller. That
// is not hypothetical tidiness: BearSSL was measured against mbedTLS
// before this was written, and the losing candidate was losing on
// maintenance rather than on fit.
//
// **NOT IN userland/lib/.** That directory is globbed wholesale into
// libuapp.a and libuapp.so, which every program links -- so a TLS call
// there would put mbedTLS behind every binary in the system. The
// implementation lives in libssl.so and a program opts in.
#include <stddef.h>

struct utls; // opaque

// How much the caller is prepared to trust this machine's randomness.
// A TLS private key drawn from TSC jitter under an emulator is not
// secret from anyone who can model the emulator, so utls_connect()
// REFUSES by default and this is the override. See
// `userland/backends/mbedtls/toyos_platform.c`.
enum utls_entropy_policy {
    UTLS_ENTROPY_REQUIRE_GOOD = 0, // refuse on jitter-only -- the default
    UTLS_ENTROPY_ALLOW_WEAK,       // proceed, and say so
};

// What is wrong with one certificate of a chain -- the order is
// NetSurf's and OpenSSL's coarse one, so a browser maps it 1:1. A chain
// can carry several faults; the worst is reported by priority, highest
// first: NAME_MISMATCH, EXPIRED, NOT_YET_VALID, REVOKED, SELF_SIGNED,
// UNTRUSTED, BAD_SIG.
enum utls_cert_err {
    UTLS_CERT_OK = 0,
    UTLS_CERT_UNKNOWN,         // verification failed for a reason not below
    UTLS_CERT_UNTRUSTED,       // no anchor in the store vouches for its issuer
    UTLS_CERT_BAD_SIG,         // a signature or key the policy will not accept
    UTLS_CERT_NOT_YET_VALID,
    UTLS_CERT_EXPIRED,
    UTLS_CERT_SELF_SIGNED,     // the server's certificate is its own issuer
    UTLS_CERT_REVOKED,
    UTLS_CERT_NAME_MISMATCH,   // valid, but for a different host
};

// A sentence for one: "the certificate has expired".
const char *utls_cert_err_text(enum utls_cert_err e);

// The chain the server PRESENTED, leaf first, each certificate in DER
// with its own fault. Filled on success AND on a verification failure --
// a browser's "this connection is not private" page is built from the
// failing chain, which is exactly when a NULL session would lose it.
#define UTLS_CHAIN_MAX 10
struct utls_chain {
    int depth;                       // how many of certs[] are filled
    enum utls_cert_err worst;        // the chain's fault, OK when it verified
    struct {
        enum utls_cert_err err;
        unsigned char *der;          // heap, owned by the chain
        size_t der_len;
    } certs[UTLS_CHAIN_MAX];
};
// Frees the DER copies and zeroes the chain. Safe on an empty one.
void utls_chain_free(struct utls_chain *c);

// What one certificate of a chain says, for a person to read -- the
// names and dates a browser's connection card shows. Strings are
// empty when the certificate does not carry them.
struct utls_cert_info {
    char subject_cn[128], subject_org[128];
    char issuer_cn[128], issuer_org[128];
    struct { int year, mon, day; } not_before, not_after;
};
// From a certificate's DER. 0, or -1 when it does not parse.
int utls_cert_describe(const unsigned char *der, size_t len, struct utls_cert_info *out);

struct utls_config {
    // The name to verify the certificate against, and to send as SNI.
    // REQUIRED even when verification is off: a shared-hosting server
    // given no name answers with the wrong certificate, and that reads
    // as a verification bug rather than a missing extension.
    const char *hostname;

    // Directory of PEM trust anchors. NULL means UTLS_DEFAULT_CA_DIR.
    const char *ca_dir;

    // Proceed with an UNVERIFIED peer. The connection is still
    // encrypted, and it is no longer authenticated: anyone able to
    // answer in the server's place can read and rewrite it. A caller
    // setting this should say so where a person will see it.
    int insecure;

    enum utls_entropy_policy entropy;

    // OUT, optional: the peer's chain and each certificate's fault, on
    // success or on a certificate failure (a failure before the server
    // sent one leaves it empty). The caller frees it with
    // utls_chain_free(). An `insecure` connection still verifies, so its
    // chain says what was wrong with what it accepted.
    struct utls_chain *chain;
};

#define UTLS_DEFAULT_CA_DIR "/etc/ssl/certs"

// Is a TLS engine present in this build? Always 1 in libssl.so; exists
// so a caller linked against it can answer without a handshake.
int utls_available(void);

// THE TRUST STORE IS PARSED ONCE PER PROCESS AND DIRECTORY, not once
// per handshake: the Mozilla bundle is about 150 certificates, and a
// page of thirty resources over TLS paid for parsing it thirty times.
// A change to the directory is seen by the next process, as a change to
// /etc/ssl/certs is by OpenSSL's callers.
//
// Wraps an already-CONNECTED socket. The fd stays the caller's: this
// does not close it, on success or failure.
//
// Returns NULL on failure and writes a sentence into `err` -- which is
// the whole reason it is not an errno. A TLS failure has too many
// distinguishable causes to fold into one integer, and "certificate
// verification failed: not trusted by any anchor in /etc/ssl/certs" is
// what a caller needs to print.
struct utls *utls_connect(int fd, const struct utls_config *cfg,
                          char *err, size_t errcap);

// Both return the byte count, 0 at clean end of stream, or negative on
// error. A short return is normal and is NOT an error: one TLS record
// is what it is, and the kernel caps a stream read at
// SYS_NET_STREAM_READ_MAX regardless of the buffer handed down, so a
// 16 KiB record arrives over a dozen reads.
long utls_read(struct utls *t, void *buf, size_t n);
long utls_write(struct utls *t, const void *buf, size_t n);

// Sends close_notify, then frees. A TLS stream has an explicit end, and
// omitting it makes a truncation attack indistinguishable from a peer
// hanging up.
void utls_close(struct utls *t);

// --- the server side ---------------------------------------------------
//
// For a remote desktop server (/bin/remoted's VeNCrypt). Its identity is a
// key and a SELF-SIGNED certificate made on this machine once and kept, so
// a viewer that remembered the fingerprint sees the same one next time.
// Making them is refused on weak randomness, as utls_connect() refuses to
// key a connection: a server key from TSC jitter is not secret.

// Makes the key if it is missing or will not parse, rebuilds the
// certificate for `names` (IPv4 addresses and DNS names, the first is the
// CN; at most UTLS_NAMES_MAX) and writes it only when it changed -- the same
// key and names give the same bytes, so the fingerprint survives reboots.
// Writes its SHA-256 fingerprint ("AB:CD:...") into `fp`, what a viewer
// shows and a person compares. 0, or -1 with a sentence in `err`.
#define UTLS_NAMES_MAX 12
int utls_server_identity(const char *key_path, const char *crt_path,
                         const char *const *names, int nnames,
                         char *fp, size_t fpcap, char *err, size_t errcap);

// Wraps an ACCEPTED socket as the TLS server with that identity, and
// runs the handshake -- given up after UTLS_ACCEPT_TIMEOUT_MS without a
// byte from the peer. NULL on failure, with a sentence in `err`.
#define UTLS_ACCEPT_TIMEOUT_MS 20000
struct utls *utls_accept(int fd, const char *key_path, const char *crt_path,
                         char *err, size_t errcap);

// utls_read() that waits at most `timeout_ms` for a record: bytes, 0 at
// a clean end, UTLS_TIMEOUT when nothing came, negative on error.
#define UTLS_TIMEOUT (-0x7f02)
long utls_read_timeout(struct utls *t, void *buf, size_t n, int timeout_ms);

// "TLSv1.3" / "TLSv1.2", and the negotiated suite. Never NULL.
const char *utls_version(struct utls *t);
const char *utls_ciphersuite(struct utls *t);

#endif
