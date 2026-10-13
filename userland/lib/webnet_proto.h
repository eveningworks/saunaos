#ifndef ULIB_WEBNET_PROTO_H
#define ULIB_WEBNET_PROTO_H

#include <stdint.h>

// The contract between /bin/webnet -- the network process, one per Web
// Browser (docs/browser-design.md, stage 5) -- and its clients: each
// content process, which fetches through it, and the browser app, which
// asks it about cookies and downloads. Records over lib/ubulk.h; webnet
// is the server, on WEBNET_SERVICE_FMT. A record's `id` is the fetch a
// content process numbered, or a download webnet numbered.

#define WEBNET_SERVICE_FMT "wn%d"    // the browser app's pid (no dot: lib/uchan.h)

// --- content process -> webnet ---------------------------------------

enum {
    WN_FETCH = 1,      // struct wn_fetch, then the URL, the headers, the body's first bytes
    WN_BODY,           // more of a request body, in order, until body_len is reached
    WN_ABORT,          // stop the fetch; nothing more is sent for it
    WN_CERT_ALLOW,     // string: a URL whose host's certificate was accepted anyway
    WN_DOWNLOAD,       // string: the file name to save the fetch's response as
};

#define WN_METHOD_GET  0
#define WN_METHOD_POST 1

// The headers are NUL-terminated lines ("Name: value") back to back,
// `headers_len` bytes in all. webnet adds Cookie and drops any the
// content process sent; it never trusts a content process's cookies.
struct wn_fetch {
    int32_t method;
    int32_t url_len, headers_len;
    int32_t body_len;          // the whole body; what does not fit follows as WN_BODY
};

// --- webnet -> content process ---------------------------------------

enum {
    WN_HEADER = 20,    // a response header line, with its CRLF
    WN_DATA,           // body bytes
    WN_TLS,            // "version\0cipher" -- an https handshake was made
    WN_CERTS,          // struct wn_certs then each certificate's DER
    WN_DONE,           // struct wn_done then the error text (empty on success)
};

#define WN_CERTS_MAX 8
struct wn_certs {
    int32_t depth;
    int32_t err[WN_CERTS_MAX];    // enum utls_cert_err, per certificate
    int32_t len[WN_CERTS_MAX];    // DER bytes, which follow in order
};

struct wn_done { int32_t rc, cert_err; };

// --- the browser app <-> webnet --------------------------------------

enum {
    WN_HELLO_APP = 40, // app -> webnet: this client is the browser; downloads report here
    WN_COOKIES_COUNT,  // app -> webnet, string: a host. Answered with WN_COOKIES.
    WN_COOKIES_CLEAR,  // app -> webnet, string: a host -- every cookie it would be sent
    WN_COOKIES,        // webnet -> app: struct wn_cookies
    WN_DL_CANCEL,      // app -> webnet: id = the download
    WN_DL,             // webnet -> app: struct wn_dl then the name, the path, the URL
};

struct wn_cookies { int32_t count, cleared; };

enum { WN_DL_RUNNING, WN_DL_DONE, WN_DL_FAILED, WN_DL_CANCELLED };
struct wn_dl {
    int32_t state;
    int64_t got, total;              // total -1 when the server did not say
    int32_t name_len, path_len, url_len;
};

#endif
