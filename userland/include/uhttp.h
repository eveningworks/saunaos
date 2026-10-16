#ifndef ULIB_UHTTP_H
#define ULIB_UHTTP_H

// An HTTP/HTTPS client, as a library.
//
// It exists because there were about to be three copies of it. /bin/wget
// had a URL parser, a header/blank-line scanner and a status scrape;
// /bin/httpd has its own request-line parser; and docs/update-design.md
// designs a /bin/update that fetches an HTTP manifest. The part that is
// easy to get wrong is invisible in all three: a socket read returns at
// most SYS_NET_STREAM_READ_MAX bytes whatever buffer it is handed, so the
// blank line ending the headers can straddle any two reads.
//
// **NOT IN userland/lib/.** That directory is globbed into libuapp,
// which every program links, and this reaches TLS -- which would put
// mbedTLS behind every binary in the system. It builds into
// /lib/libhttp.so and a program opts in with ULIB_SO_<name>.
//
// **HTTP/1.1, AND A CONNECTION MAY OUTLIVE ITS REQUEST.** Given a pool
// (`struct uhttp_pool`), a response that ends where its framing says --
// Content-Length or chunked -- leaves its connection in the pool for the
// next request to the same scheme, host and port. Without one, every
// request sends `Connection: close` and the server ends the body by
// closing, which is what wget and update want.
//
// **THREADS.** Separate requests may run in separate threads at once,
// sharing one pool; a single request belongs to one thread.
//
// **HTTP AND HTTPS DIFFER BY ONE FIELD.** The scheme decides whether a
// TLS session is wrapped around the socket; everything above that is
// the same code. That is the whole reason the transport is behind
// <utls.h> rather than open-coded in the fetch.
#include <stddef.h>
#include <stdint.h>

// A URL path is not sized against FS_PATH_MAX -- a URL is not a path,
// and sizing it like one ties two unrelated limits together (the shell
// conventions make the same point about a command line).
#define UHTTP_HOST_MAX 128
// 2 KiB: a release-asset redirect (GitHub's) carries a signed query of
// over 1 KiB. Keep struct uhttp_url off a 2 KiB ring-3 stack frame.
#define UHTTP_PATH_MAX 2048
#define UHTTP_ERR_MAX  192

struct uhttp_url {
    char     host[UHTTP_HOST_MAX];
    char     path[UHTTP_PATH_MAX];
    uint16_t port;
    int      tls;          // https

    // A URL with NO scheme ("example.com") parses with tls=1 and port
    // 443, and these two say so. `uhttp_fetch()` then tries https and
    // falls back to http -- but ONLY when 443 does not connect, which
    // is the whole reason the caller has to be able to tell the two
    // apart. See the fetch's comment.
    int      scheme_given;
    int      port_given;
};

// 0 on success, -1 on a URL this cannot represent. REJECTS rather than
// guessing about anything but the SCHEME: a host that does not fit is
// an error, never a prefix.
int uhttp_parse_url(const char *url, struct uhttp_url *out);

// Called with each chunk of the body as it arrives. Return 0 to carry
// on, non-zero to abort the fetch. Streaming rather than buffering
// because the body may be larger than the heap.
typedef int (*uhttp_sink)(void *ctx, const void *data, size_t len);

struct uhttp_pool;    // below
struct utls_chain;    // <utls.h>

struct uhttp_request {
    // --- in ---
    const char *url;
    uhttp_sink  sink;
    void       *sink_ctx;

    // "GET" when NULL. A request with a body says what it is: "POST".
    const char *method;
    // Sent with a Content-Length. NULL sends none.
    const void *body;
    size_t      body_len;
    // Extra request headers, each a whole "Name: value" line without the
    // CRLF, NULL-terminated. One naming User-Agent or Accept-Encoding
    // REPLACES the default; anything else is added. Host, Content-Length
    // and Connection are this library's and are refused here (-1).
    const char *const *headers;

    // Reuse a kept-alive connection, and keep this one. NULL = a fresh
    // connection, closed after.
    struct uhttp_pool *pool;

    // Non-zero, set from another thread, stops the request at its next
    // read -- a read already waiting on a silent server finishes first.
    const volatile int *cancel;

    // Skip certificate verification. Encrypted but UNAUTHENTICATED --
    // anyone able to answer in the server's place can read and rewrite
    // the exchange. A caller setting this should say so on screen.
    int insecure;

    // Proceed even when QUERY_RANDOM says this machine's entropy is
    // only TSC jitter. Off by default; see <utls.h>.
    int allow_weak_entropy;

    const char *ca_dir; // NULL = UTLS_DEFAULT_CA_DIR
    // How long a TCP connect may take before the fetch fails; 0 = the
    // kernel's own. A browser trying https on a host typed without a
    // scheme gives it 3 s and falls back to http (Chrome's HTTPS-Upgrades).
    unsigned connect_timeout_ms;

    // How many 301/302/303/307/308 hops to follow -- 0, the default,
    // follows none and hands the redirect to the caller as its status
    // (curl without -L). Never from https to http: a downgrade the
    // first URL did not ask for.
    int max_redirects;

    // Called once, after the connection is up and before the body, so a
    // caller can print what it connected to. `tls_version` is NULL on a
    // plain connection. Either may be NULL.
    void (*on_connect)(void *ctx, uint32_t ip, const char *tls_version,
                       const char *tls_cipher);

    // Called when a scheme-less URL's https attempt could not CONNECT
    // and http is about to be tried instead. A downgrade the user did
    // not ask for should not be silent, so this exists to be printed --
    // it is not a debug hook.
    void (*on_fallback)(void *ctx, const char *host);

    // Called once, after the response headers and before the first
    // sink() call, so a caller can size a progress meter or refuse a
    // body before reading it. `content_length` is 0 when the server did
    // not say -- which is not an error and not an empty body: a chunked
    // body has none, and a server answering a `Connection: close`
    // request may end the body by closing. Also 0 for a gzipped body,
    // whose header counts compressed bytes the sink never sees.
    // May be NULL.
    void (*on_headers)(void *ctx, int status, unsigned long content_length);

    // Called with EVERY response header line as it arrives, the status
    // line first ("HTTP/1.1 200 OK"), then one per header exactly as the
    // server sent it, without the CRLF; `len` excludes the NUL. A folded
    // continuation line arrives as its own call, as curl delivers it. For
    // a browser, which keeps cookies, redirects and authentication
    // itself and so needs what the server said rather than a summary.
    // Called for every hop of a followed redirect. May be NULL.
    void (*on_header)(void *ctx, const char *line, size_t len);

    // Called once per https connection made (not per pooled reuse), after
    // the handshake -- or after it FAILED on a certificate -- with the
    // chain the server presented, leaf first, and each certificate's
    // fault. The chain is only lent for the call. May be NULL.
    void (*on_certs)(void *ctx, const struct utls_chain *chain);

    // --- out ---
    int           status;      // the HTTP status, or 0 if none was read
    unsigned long body_bytes;  // what reached the sink
    int           used_tls;    // whether the fetch that SUCCEEDED was https
    int           reused;      // the connection came from the pool
    // An enum utls_cert_err: why the server's certificate was refused,
    // or 0 when the failure (if any) was not the certificate's -- the
    // difference between "this site's identity is wrong" and "the
    // network is down", which a browser shows differently.
    int           cert_err;
    char          err[UHTTP_ERR_MAX];
};

// 0 on success (a response was read, whatever its status), -1 otherwise
// with `err` set to a sentence. A non-2xx status is NOT a failure here:
// the body of an error page is usually the explanation, and deciding
// what to do about it is the caller's.
int uhttp_fetch(struct uhttp_request *req);

// THE CONNECTION POOL: kept-alive connections, IDLE ones only, keyed by
// scheme, host, port and whether the certificate was checked. curl's
// connection cache, owned by its caller the way a curl share handle is.
// A connection in use is the request's; it comes back when the response
// was read to its end and nobody said `Connection: close`.
//
// A pooled connection the server has since closed is found out by the
// request that takes it, which then retries ONCE on a fresh connection --
// for GET and HEAD only, the methods that are safe to send twice. A POST
// always opens a fresh connection for that reason.
struct uhttp_pool_limits {
    int per_host;     // idle connections kept per host; 0 = 6, Chrome's and Firefox's
    int total;        // idle connections kept in all;   0 = 16
    int idle_ms;      // an idle connection older than this is closed; 0 = 30000
};
// NULL limits = the defaults. NULL on out of memory.
struct uhttp_pool *uhttp_pool_new(const struct uhttp_pool_limits *limits);
// Closes every idle connection. Nothing may be fetching through it.
void uhttp_pool_free(struct uhttp_pool *p);
// How many idle connections it holds, and how many it has handed out
// again since it was made -- what a test asserts reuse with.
int uhttp_pool_idle(struct uhttp_pool *p);
unsigned long uhttp_pool_reuses(struct uhttp_pool *p);

// A URL into a FILE, checked -- curl -o, plus what a game-data or
// package fetch needs: the body lands in `<path>.part` and is renamed
// over `path` only when the status is 200 and, if `sha256` is given, the
// bytes hash to it. Anything else leaves nothing behind. The SHA-256 is
// what makes `insecure` reasonable for a public file: a substitute fails
// the hash. Links /lib/libhash.so.
struct uhttp_download {
    // --- in ---
    const char *url;
    const char *path;
    const char *sha256;            // 64 hex digits, or NULL for unchecked
    int insecure, allow_weak_entropy, max_redirects;   // as uhttp_request's
    // Bytes so far and the size the server gave (0 if none), as they
    // arrive. Called from the fetching thread.
    void (*progress)(void *ctx, unsigned long got, unsigned long total);
    int (*cancelled)(void *ctx);   // non-zero stops it
    void *ctx;
    // --- out ---
    int status;
    unsigned long bytes;
    char err[UHTTP_ERR_MAX];
};
// 0, or -1 with `err` a sentence.
int uhttp_download(struct uhttp_download *d);

// Is this build able to speak https at all? Lets a caller say "no TLS
// in this build" rather than failing at connect time.
int uhttp_tls_available(void);

#endif
