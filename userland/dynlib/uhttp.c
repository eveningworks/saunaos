// <uhttp.h> -- the HTTP/1.1 client, over a plain socket or a TLS session.
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rt/sys.h"
#include "uhttp.h"
#include "lib/uinflate.h"
#include "utls.h"
#include "lib/uresolv.h"

int uhttp_tls_available(void) { return utls_available(); }

static unsigned long long now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

// --- a connection ------------------------------------------------------------

// One transport, two implementations, chosen by the scheme, with a read
// buffer in front -- the response is parsed a LINE at a time, and a
// socket read returns at most SYS_NET_STREAM_READ_MAX bytes whatever it
// is handed, so a header line straddles reads routinely. Everything
// above this section is unaware of which transport it has.
struct conn {
    int          fd;
    struct utls *tls;                 // NULL for plain http
    // The pool's key: who this connection is to, and how it was checked.
    char         host[UHTTP_HOST_MAX];
    uint16_t     port;
    int          tls_on, insecure;
    unsigned long long idle_since;    // ms, while it sits in a pool
    size_t       rpos, rlen;
    char         rbuf[4096];
};

static void conn_close(struct conn *c) {
    if (!c) return;
    if (c->tls) utls_close(c->tls);
    if (c->fd >= 0) close(c->fd);
    free(c);
}

static long c_write(struct conn *c, const void *buf, size_t n) {
    return c->tls ? utls_write(c->tls, buf, n) : write(c->fd, buf, n);
}

// A stream write may be short -- the kernel's send buffer is 4 KiB and
// a TLS record is its own size -- so every write is a loop.
static int write_all(struct conn *c, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        long w = c_write(c, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

// Bytes buffered, or 0 at the peer's close, or negative on an error.
static long c_fill(struct conn *c) {
    if (c->rpos < c->rlen) return (long)(c->rlen - c->rpos);
    long got = c->tls ? utls_read(c->tls, c->rbuf, sizeof c->rbuf)
                      : read(c->fd, c->rbuf, sizeof c->rbuf);
    c->rpos = 0;
    c->rlen = got > 0 ? (size_t)got : 0;
    return got;
}

// --- the pool ----------------------------------------------------------------

#define POOL_CAP 64   // the most idle connections any limits may ask for

struct uhttp_pool {
    pthread_mutex_t mu;
    struct uhttp_pool_limits lim;
    struct conn *idle[POOL_CAP];
    int n;
    unsigned long reuses;
};

struct uhttp_pool *uhttp_pool_new(const struct uhttp_pool_limits *limits) {
    struct uhttp_pool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    pthread_mutex_init(&p->mu, NULL);
    if (limits) p->lim = *limits;
    if (p->lim.per_host <= 0) p->lim.per_host = 6;
    if (p->lim.total <= 0) p->lim.total = 16;
    if (p->lim.total > POOL_CAP) p->lim.total = POOL_CAP;
    if (p->lim.idle_ms <= 0) p->lim.idle_ms = 30000;
    return p;
}

void uhttp_pool_free(struct uhttp_pool *p) {
    if (!p) return;
    for (int i = 0; i < p->n; i++) conn_close(p->idle[i]);
    pthread_mutex_destroy(&p->mu);
    free(p);
}

int uhttp_pool_idle(struct uhttp_pool *p) {
    pthread_mutex_lock(&p->mu);
    int n = p->n;
    pthread_mutex_unlock(&p->mu);
    return n;
}

unsigned long uhttp_pool_reuses(struct uhttp_pool *p) {
    pthread_mutex_lock(&p->mu);
    unsigned long n = p->reuses;
    pthread_mutex_unlock(&p->mu);
    return n;
}

static int same_peer(const struct conn *c, const struct uhttp_url *u, int insecure) {
    return c->port == u->port && c->tls_on == u->tls && c->insecure == insecure &&
           !strcmp(c->host, u->host);
}

// The NEWEST idle connection to that peer -- the one least likely to have
// been closed by the server's own idle timer. Expired ones are closed on
// the way past, outside the lock: a TLS close is a write.
static struct conn *pool_take(struct uhttp_pool *p, const struct uhttp_url *u, int insecure) {
    struct conn *dead[POOL_CAP], *got = NULL;
    int ndead = 0;
    unsigned long long now = now_ms();
    pthread_mutex_lock(&p->mu);
    for (int i = 0; i < p->n;) {
        struct conn *c = p->idle[i];
        if (now - c->idle_since > (unsigned long long)p->lim.idle_ms) {
            dead[ndead++] = c;
            p->idle[i] = p->idle[--p->n];
            continue;
        }
        i++;
    }
    int best = -1;
    for (int i = 0; i < p->n; i++)
        if (same_peer(p->idle[i], u, insecure) &&
            (best < 0 || p->idle[i]->idle_since > p->idle[best]->idle_since))
            best = i;
    if (best >= 0) {
        got = p->idle[best];
        p->idle[best] = p->idle[--p->n];
        p->reuses++;
    }
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < ndead; i++) conn_close(dead[i]);
    return got;
}

// Keeps `c` idle, making room by closing the OLDEST idle connection to
// the same host, then the oldest of all.
static void pool_put(struct uhttp_pool *p, struct conn *c) {
    struct conn *evict = NULL;
    c->idle_since = now_ms();
    pthread_mutex_lock(&p->mu);
    int same = 0, old_same = -1, old_any = -1;
    for (int i = 0; i < p->n; i++) {
        struct conn *o = p->idle[i];
        if (!strcmp(o->host, c->host) && o->port == c->port) {
            same++;
            if (old_same < 0 || o->idle_since < p->idle[old_same]->idle_since) old_same = i;
        }
        if (old_any < 0 || o->idle_since < p->idle[old_any]->idle_since) old_any = i;
    }
    int victim = same >= p->lim.per_host ? old_same : p->n >= p->lim.total ? old_any : -1;
    if (victim >= 0) {
        evict = p->idle[victim];
        p->idle[victim] = p->idle[--p->n];
    }
    p->idle[p->n++] = c;
    pthread_mutex_unlock(&p->mu);
    conn_close(evict);
}

int uhttp_parse_url(const char *url, struct uhttp_url *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof *out);

    const char *p = url;
    if (!strncmp(p, "https://", 8)) {
        out->tls = 1;
        out->port = 443;
        out->scheme_given = 1;
        p += 8;
    } else if (!strncmp(p, "http://", 7)) {
        out->tls = 0;
        out->port = 80;
        out->scheme_given = 1;
        p += 7;
    } else if (strstr(p, "://")) {
        return -1; // a scheme this does not speak, not a bare host
    } else {
        // NO SCHEME: guess https, and let the fetch fall back. Browsers
        // upgrade a typed address this way; GNU wget and curl both
        // default to plaintext instead, which is the older answer to a
        // question the web has since settled.
        out->tls = 1;
        out->port = 443;
        out->scheme_given = 0;
    }

    // host[:port], up to the first '/'. An IPv6 literal in brackets is
    // not accepted, because nothing in this stack speaks IPv6 and
    // parsing an address the socket layer would refuse is worse than
    // refusing it here.
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));

    size_t hostlen = (size_t)((colon ? colon : hostend) - p);
    if (hostlen == 0 || hostlen >= sizeof out->host) return -1;
    memcpy(out->host, p, hostlen);
    out->host[hostlen] = '\0';

    if (colon) {
        unsigned port = 0;
        for (const char *q = colon + 1; q < hostend; q++) {
            if (*q < '0' || *q > '9') return -1;
            port = port * 10 + (unsigned)(*q - '0');
            if (port > 65535) return -1;
        }
        if (port == 0) return -1;
        out->port = (uint16_t)port;
        out->port_given = 1;

        // AN EXPLICIT PORT CANCELS THE https GUESS, unless it is 443.
        // Chrome upgrades a typed address only on the default port, and
        // the reason is the same here: `host:8080` is overwhelmingly a
        // plain server, and guessing https for it fails at the
        // handshake -- which must NOT fall back, since that is
        // indistinguishable from a server whose certificate is wrong.
        // Somebody who wants https on an odd port can say so.
        if (!out->scheme_given) out->tls = (out->port == 443);
    }

    const char *path = slash ? slash : "/";
    if (strlen(path) >= sizeof out->path) return -1; // refuse, never truncate
    strlcpy(out->path, path, sizeof out->path);
    return 0;
}


// --- one exchange ------------------------------------------------------------

static void fail(struct uhttp_request *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, sizeof r->err, fmt, ap);
    va_end(ap);
}

// Longer than any header line a real server sends; a longer one fails
// the request rather than being cut, since a cut Set-Cookie or Location
// is a wrong value, not a short one.
#define UHTTP_LINE_MAX (16 * 1024)
// Longer than any redirect a real server sends; a longer one is refused.
#define UHTTP_LOCATION_MAX 4096
// A ceiling on what a server may send us compressed. A decompression
// bomb is a small download that expands without limit; this bounds the
// COMPRESSED side, and the sink's own refusal bounds the other.
#define UHTTP_GZIP_MAX (64u * 1024u * 1024u)

static int ieq_prefix(const char *s, const char *name) {
    while (*name) {
        char a = *s++, b = *name++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

// The value of a "Name: value" line when its name is `name` (lower case,
// with the colon), past the blanks; else NULL.
static const char *hval(const char *line, const char *name) {
    if (!ieq_prefix(line, name)) return NULL;
    line += strlen(name);
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

// Does the comma-separated header value carry `token`?
static int has_token(const char *v, const char *token) {
    size_t n = strlen(token);
    while (*v) {
        while (*v == ' ' || *v == '\t' || *v == ',') v++;
        if (ieq_prefix(v, token) && (v[n] == 0 || v[n] == ',' || v[n] == ' ' || v[n] == ';'))
            return 1;
        while (*v && *v != ',') v++;
    }
    return 0;
}

struct exchange {
    struct uhttp_request *req;
    struct conn *c;
    char *line;          // UHTTP_LINE_MAX, the line being read
    int got_any;         // a byte of the response arrived
    // The response, as its headers describe it.
    int http11, close_after, chunked, gzip, no_body;
    long long content_length;   // -1 = not given
    char *loc;
    // The compressed body, when there is one.
    char *gz;
    size_t gz_len, gz_cap;
    int stopped;
};

static int cancelled(const struct uhttp_request *r) { return r->cancel && *r->cancel; }

// One line into ex->line without its CRLF (a bare LF is accepted too).
// 1, or 0 at a close before any byte of it, or -1.
static int read_line(struct exchange *ex, size_t *len_out) {
    size_t len = 0;
    for (;;) {
        long n = c_fill(ex->c);
        if (n < 0 || cancelled(ex->req)) return -1;
        if (n == 0) return len ? -1 : 0;
        ex->got_any = 1;
        struct conn *c = ex->c;
        char *p = c->rbuf + c->rpos;
        char *nl = memchr(p, '\n', c->rlen - c->rpos);
        size_t take = nl ? (size_t)(nl - p) : c->rlen - c->rpos;
        if (len + take >= UHTTP_LINE_MAX) return -1;
        memcpy(ex->line + len, p, take);
        len += take;
        c->rpos += take + (nl ? 1 : 0);
        if (nl) {
            if (len && ex->line[len - 1] == '\r') len--;
            ex->line[len] = 0;
            *len_out = len;
            return 1;
        }
    }
}

// Body bytes, inflated or not, on their way to the caller.
static int deliver(struct exchange *ex, const char *data, size_t n) {
    struct uhttp_request *req = ex->req;
    if (ex->gzip) {
        // A GZIPPED BODY IS BUFFERED, NOT STREAMED: lib/uinflate.h
        // consumes one contiguous input, so what is held is the
        // COMPRESSED size, which is what asking for gzip bought.
        if (ex->gz_len + n > ex->gz_cap) {
            size_t want = ex->gz_cap ? ex->gz_cap * 2 : 16384;
            while (want < ex->gz_len + n) want *= 2;
            if (want > UHTTP_GZIP_MAX) {
                fail(req, "the compressed body is larger than %u bytes", (unsigned)UHTTP_GZIP_MAX);
                return -1;
            }
            char *ng = realloc(ex->gz, want);
            if (!ng) { fail(req, "out of memory for the compressed body"); return -1; }
            ex->gz = ng;
            ex->gz_cap = want;
        }
        memcpy(ex->gz + ex->gz_len, data, n);
        ex->gz_len += n;
    } else if (req->sink && req->sink(req->sink_ctx, data, n) != 0) {
        ex->stopped = 1;
        fail(req, "the fetch was stopped by its caller");
        return -1;
    }
    req->body_bytes += (unsigned long)n;
    return 0;
}

// Up to `want` bytes of the body straight from the buffer; -1 to read
// until the close. 0, or -1 with `err` set. `discard` drains instead.
static int read_body_bytes(struct exchange *ex, long long want, int discard) {
    for (;;) {
        if (want == 0) return 0;
        long n = c_fill(ex->c);
        if (n < 0 || cancelled(ex->req)) {
            fail(ex->req, "the connection failed while reading the response");
            return -1;
        }
        if (n == 0) {
            if (want < 0) return 0;   // read-until-close: that WAS the end
            fail(ex->req, "the connection closed before the body ended");
            return -1;
        }
        size_t take = (size_t)n;
        if (want > 0 && (long long)take > want) take = (size_t)want;
        struct conn *c = ex->c;
        if (!discard && deliver(ex, c->rbuf + c->rpos, take) != 0) return -1;
        c->rpos += take;
        if (want > 0) want -= (long long)take;
    }
}

// Transfer-Encoding: chunked -- a hex size line, that many bytes, a CRLF,
// until a zero size and the trailer lines after it.
static int read_chunked(struct exchange *ex, int discard) {
    for (;;) {
        size_t len;
        if (read_line(ex, &len) != 1) {
            fail(ex->req, "the connection closed inside a chunked body");
            return -1;
        }
        unsigned long long size = 0;
        int digits = 0;
        for (const char *p = ex->line; *p; p++, digits++) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0'
                  : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                  : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
            if (d < 0) break;                 // ';' starts an extension
            if (size >> 60) { digits = 0; break; }
            size = size * 16 + (unsigned)d;
        }
        if (!digits) { fail(ex->req, "a chunked body with a malformed size line"); return -1; }
        if (size == 0) {
            // Trailers, up to the blank line. Not delivered: nothing here
            // reads one, and a trailer may not carry framing.
            do {
                if (read_line(ex, &len) != 1) return 0;   // a close here loses nothing
            } while (len);
            return 0;
        }
        if (read_body_bytes(ex, (long long)size, discard) != 0) return -1;
        if (read_line(ex, &len) != 1 || len) {
            fail(ex->req, "a chunk did not end where its size said");
            return -1;
        }
    }
}

static int read_body(struct exchange *ex, int discard) {
    if (ex->no_body) return 0;
    if (ex->chunked) return read_chunked(ex, discard);
    if (ex->content_length >= 0) return read_body_bytes(ex, ex->content_length, discard);
    ex->close_after = 1;   // only the close says where it ends
    return read_body_bytes(ex, -1, discard);
}

static int is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// One header line of the final response: told to the caller, and the
// few the transfer itself needs are read.
static void take_header(struct exchange *ex, const char *line, size_t len) {
    struct uhttp_request *req = ex->req;
    if (req->on_header) req->on_header(req->sink_ctx, line, len);
    const char *v;
    if ((v = hval(line, "content-length:"))) {
        long long n = 0;
        int any = 0;
        for (; *v >= '0' && *v <= '9' && n < (1ll << 50); v++) { n = n * 10 + (*v - '0'); any = 1; }
        if (any) ex->content_length = n;
    } else if ((v = hval(line, "transfer-encoding:"))) {
        ex->chunked = has_token(v, "chunked");
    } else if ((v = hval(line, "content-encoding:"))) {
        // `gzip` and the older `x-gzip` are the same thing. `deflate` is
        // NOT accepted: some servers send a zlib stream under that name
        // and some a bare one, and we never ask for it.
        ex->gzip = has_token(v, "gzip") || has_token(v, "x-gzip");
    } else if ((v = hval(line, "connection:"))) {
        if (has_token(v, "close")) ex->close_after = 1;
        if (has_token(v, "keep-alive") && !ex->http11) ex->close_after = 0;
    } else if (req->max_redirects > 0 && (v = hval(line, "location:"))) {
        free(ex->loc);
        ex->loc = strlen(v) < UHTTP_LOCATION_MAX ? strdup(v) : NULL;
    }
}

// The status line and headers. 0, or -1 with `err` set; a close before
// any byte returns -2, which is how a dead pooled connection looks.
static int read_head(struct exchange *ex, int head_request) {
    struct uhttp_request *req = ex->req;
    size_t len;
    for (;;) {
        int r = read_line(ex, &len);
        if (r == 0 && !ex->got_any) return -2;
        if (r != 1) {
            if (!ex->got_any) return -2;
            fail(req, "the connection closed before the response headers ended");
            return -1;
        }
        // "HTTP/1.x NNN reason"
        if (strncmp(ex->line, "HTTP/1.", 7) != 0 || len < 12 || ex->line[8] != ' ') {
            fail(req, "the server did not answer in HTTP");
            return -1;
        }
        ex->http11 = ex->line[7] >= '1';
        req->status = atoi(ex->line + 9);
        // 1.1 keeps the connection unless told; 1.0 closes unless told.
        ex->close_after = !ex->http11 || !req->pool;
        ex->chunked = ex->gzip = 0;
        ex->content_length = -1;
        // An interim response (100 Continue, 103 Early Hints) is followed
        // by the real one; its headers describe nothing here.
        int interim = req->status >= 100 && req->status < 200 && req->status != 101;
        if (!interim && req->on_header) req->on_header(req->sink_ctx, ex->line, len);
        for (;;) {
            if (read_line(ex, &len) != 1) {
                fail(req, "the connection closed inside the response headers");
                return -1;
            }
            if (!len) break;
            if (!interim) take_header(ex, ex->line, len);
        }
        if (interim) continue;
        ex->no_body = head_request || req->status == 204 || req->status == 304 ||
                      req->status < 200;
        if (!req->pool) ex->close_after = 1;
        return 0;
    }
}

// Does a caller header line collide with one this library owns?
static int owned_header(const char *h) {
    return ieq_prefix(h, "host:") || ieq_prefix(h, "content-length:") ||
           ieq_prefix(h, "connection:") || ieq_prefix(h, "transfer-encoding:");
}

static int caller_has(const struct uhttp_request *req, const char *name) {
    for (const char *const *h = req->headers; h && *h; h++)
        if (ieq_prefix(*h, name)) return 1;
    return 0;
}

// The request head, on the heap. NULL with `err` set.
static char *build_request(struct uhttp_request *req, const struct uhttp_url *u,
                           const char *method, size_t *len_out) {
    size_t cap = strlen(u->path) + strlen(u->host) + 256;
    for (const char *const *h = req->headers; h && *h; h++) {
        if (strpbrk(*h, "\r\n") || owned_header(*h) || !strchr(*h, ':')) {
            fail(req, "refused the request header '%.60s'", *h);
            return NULL;
        }
        cap += strlen(*h) + 2;
    }
    char *b = malloc(cap);
    if (!b) { fail(req, "out of memory for the request"); return NULL; }
    size_t n = 0;
#define ADD(...) n += (size_t)snprintf(b + n, cap - n, __VA_ARGS__)
    ADD("%s %s HTTP/1.1\r\n", method, u->path);
    // The port is in Host only when it is not the scheme's own (RFC 9110).
    if (u->port != (u->tls ? 443 : 80)) ADD("Host: %s:%u\r\n", u->host, (unsigned)u->port);
    else ADD("Host: %s\r\n", u->host);
    if (!caller_has(req, "user-agent:")) ADD("User-Agent: SaunaOS/uhttp\r\n");
    if (!caller_has(req, "accept-encoding:")) ADD("Accept-Encoding: gzip\r\n");
    if (req->body || !strcmp(method, "POST") || !strcmp(method, "PUT"))
        ADD("Content-Length: %lu\r\n", (unsigned long)(req->body ? req->body_len : 0));
    if (!req->pool) ADD("Connection: close\r\n");
    for (const char *const *h = req->headers; h && *h; h++) ADD("%s\r\n", *h);
    ADD("\r\n");
#undef ADD
    *len_out = n;
    return b;
}

// Inflates a buffered gzip body into the sink.
struct uhttp_gz {
    struct uhttp_request *req;
    int stopped;
};

static int gz_sink(void *vctx, const uint8_t *data, size_t n) {
    struct uhttp_gz *c = vctx;
    if (!c->req->sink) return 0;
    if (c->req->sink(c->req->sink_ctx, (const char *)data, n) != 0) {
        c->stopped = 1;
        return -1;
    }
    return 0;
}

static int inflate_body(struct exchange *ex) {
    struct uhttp_request *req = ex->req;
    if (!ex->gzip || !ex->gz_len) return 0;
    struct uhttp_gz gctx = { req, 0 };
    size_t out_len = 0;
    // The decoder's window is the caller's -- too big for a stack frame.
    struct uinflate_scratch *sc = malloc(sizeof *sc);
    if (!sc) { fail(req, "out of memory to decompress the body"); return -1; }
    int zrc = uinflate(ex->gz, ex->gz_len, UINFLATE_GZIP, sc, gz_sink, &gctx, &out_len);
    free(sc);
    if (gctx.stopped) { fail(req, "the fetch was stopped by its caller"); return -1; }
    if (zrc < 0) {
        fail(req, "the server's gzip body could not be read: %s", uinflate_error());
        return -1;
    }
    // What the CALLER moved is the decompressed length.
    req->body_bytes = (unsigned long)out_len;
    return 0;
}

// One attempt at one scheme. THREE-VALUED where it matters: only
// "nothing is listening on that port" may be retried over http, because
// falling back on a TLS or certificate failure would turn "this server's
// identity is wrong" into "let us use plaintext instead" -- the
// downgrade HSTS exists to stop. REDIRECT hands `*location` (heap) to
// the caller, which follows it. STALE is a pooled connection the server
// had closed, found out before any byte of the response.
enum attempt { ATTEMPT_OK, ATTEMPT_NO_CONNECT, ATTEMPT_FAILED, ATTEMPT_REDIRECT, ATTEMPT_STALE };

static enum attempt open_conn(struct uhttp_request *req, const struct uhttp_url *u, uint32_t ip,
                              struct conn **out) {
    struct conn *c = calloc(1, sizeof *c);
    if (!c) { fail(req, "out of memory for a connection"); return ATTEMPT_FAILED; }
    c->fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_STREAM, NET_ABI_IPPROTO_TCP);
    if (c->fd < 0) {
        free(c);
        fail(req, "cannot open a socket");
        return ATTEMPT_FAILED;
    }
    strlcpy(c->host, u->host, sizeof c->host);
    c->port = u->port;
    c->tls_on = u->tls;
    c->insecure = req->insecure;
    if (sys_connect(c->fd, ip, u->port, req->connect_timeout_ms) < 0) {
        // The REASON, as wget and curl print it: refused and timed out
        // are different faults with different fixes.
        fail(req, "cannot connect to %s port %u: %s", u->host, (unsigned)u->port, strerror(errno));
        conn_close(c);
        return ATTEMPT_NO_CONNECT;
    }
    if (u->tls) {
        struct utls_chain *chain = calloc(1, sizeof *chain);
        struct utls_config cfg = {
            .hostname = u->host,
            .ca_dir   = req->ca_dir,
            .insecure = req->insecure,
            .entropy  = req->allow_weak_entropy ? UTLS_ENTROPY_ALLOW_WEAK
                                                : UTLS_ENTROPY_REQUIRE_GOOD,
            .chain    = chain,
        };
        char terr[UHTTP_ERR_MAX];
        c->tls = utls_connect(c->fd, &cfg, terr, sizeof terr);
        if (chain && chain->depth && req->on_certs) req->on_certs(req->sink_ctx, chain);
        if (!c->tls) {
            // NOT ATTEMPT_NO_CONNECT: the port answered and TLS failed,
            // which is a reason to stop rather than to try plaintext.
            fail(req, "%s", terr);
            if (chain) req->cert_err = (int)chain->worst;
            utls_chain_free(chain);
            free(chain);
            conn_close(c);
            return ATTEMPT_FAILED;
        }
        utls_chain_free(chain);
        free(chain);
    }
    if (req->on_connect)
        req->on_connect(req->sink_ctx, ip, c->tls ? utls_version(c->tls) : NULL,
                        c->tls ? utls_ciphersuite(c->tls) : NULL);
    *out = c;
    return ATTEMPT_OK;
}

static int resolve_host(struct uhttp_request *req, const char *host, uint32_t *ip);

static int idempotent(const char *method) {
    return !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "OPTIONS");
}

// The request over `c`, and its response read to the end.
static enum attempt exchange_on(struct uhttp_request *req, const struct uhttp_url *u,
                                struct conn *c, int reused, char **location) {
    const char *method = req->method ? req->method : "GET";
    struct exchange ex = { .req = req, .c = c, .content_length = -1 };
    enum attempt r = ATTEMPT_FAILED;
    size_t hlen = 0;
    char *head = build_request(req, u, method, &hlen);
    if (!head) goto out;
    ex.line = malloc(UHTTP_LINE_MAX);
    if (!ex.line) { fail(req, "out of memory for the response"); goto out; }

    req->status = 0;
    req->body_bytes = 0;
    if (write_all(c, head, hlen) != 0 ||
        (req->body && req->body_len && write_all(c, req->body, req->body_len) != 0)) {
        if (reused) { r = ATTEMPT_STALE; goto out; }
        fail(req, "the connection closed while sending the request");
        goto out;
    }
    int hr = read_head(&ex, !strcmp(method, "HEAD"));
    if (hr == -2) {
        if (reused) { r = ATTEMPT_STALE; goto out; }
        fail(req, "the server closed the connection without answering");
        goto out;
    }
    if (hr != 0) goto out;

    // A redirect to follow: its body is a page nobody reads, drained
    // only so the connection can be kept.
    if (req->max_redirects > 0 && is_redirect(req->status) && ex.loc) {
        if (read_body(&ex, 1) != 0) ex.close_after = 1;
        *location = ex.loc;
        ex.loc = NULL;
        r = ATTEMPT_REDIRECT;
        goto out;
    }
    if (req->on_headers)
        req->on_headers(req->sink_ctx, req->status,
                        ex.gzip || ex.content_length < 0 ? 0 : (unsigned long)ex.content_length);
    if (read_body(&ex, 0) != 0 || inflate_body(&ex) != 0) { ex.close_after = 1; goto out; }
    r = ATTEMPT_OK;

out:
    // Kept only when the response ended where its framing said and
    // nothing is left over -- leftover bytes would be read as the NEXT
    // response's start.
    if (r == ATTEMPT_OK || r == ATTEMPT_REDIRECT) {
        if (req->pool && !ex.close_after && c->rpos == c->rlen) pool_put(req->pool, c);
        else conn_close(c);
    } else if (r != ATTEMPT_STALE) {
        conn_close(c);
    }
    free(ex.gz);
    free(ex.loc);
    free(ex.line);
    free(head);
    return r;
}

// One request to `u`, over a pooled connection or a new one.
// `*ip` is resolved on first need and kept for a second try.
static enum attempt attempt_fetch(struct uhttp_request *req, const struct uhttp_url *u,
                                  uint32_t *ip, char **location) {
    const char *method = req->method ? req->method : "GET";
    for (int tries = 0; tries < 2; tries++) {
        struct conn *c = NULL;
        // A POST never takes a pooled connection: sent twice is not the
        // same as sent once, and a pooled one may be dead.
        if (req->pool && idempotent(method)) c = pool_take(req->pool, u, req->insecure);
        int reused = c != NULL;
        if (!c) {
            if (!*ip && resolve_host(req, u->host, ip) != 0) return ATTEMPT_FAILED;
            enum attempt o = open_conn(req, u, *ip, &c);
            if (o != ATTEMPT_OK) return o;
        }
        req->used_tls = u->tls;
        req->reused = reused;
        enum attempt r = exchange_on(req, u, c, reused, location);
        if (r != ATTEMPT_STALE) return r;
        conn_close(c);   // the server had closed it; once more on a fresh one
    }
    fail(req, "the server closed the connection without answering");
    return ATTEMPT_FAILED;
}

// Where a redirect points, from where it came from: an absolute URL, a
// scheme-relative "//host/path", or a path on the same server. Anything
// else ("page.html" relative to a directory) is refused, not guessed.
static int next_url(const struct uhttp_url *cur, const char *loc, struct uhttp_url *out) {
    while (*loc == ' ' || *loc == '\t') loc++;
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8))
        return uhttp_parse_url(loc, out);
    if (loc[0] == '/' && loc[1] == '/') {
        size_t n = strlen(loc) + 8;
        char *full = malloc(n);
        if (!full) return -1;
        snprintf(full, n, "%s:%s", cur->tls ? "https" : "http", loc);
        int rc = uhttp_parse_url(full, out);
        free(full);
        return rc;
    }
    if (loc[0] == '/') {
        if (strlen(loc) >= sizeof out->path) return -1;
        *out = *cur;
        strlcpy(out->path, loc, sizeof out->path);
        return 0;
    }
    return -1;
}

// Resolved per hop: a redirect may name another host.
static int resolve_host(struct uhttp_request *req, const char *host, uint32_t *ip) {
    if (uresolv_parse_ip(host, ip)) return 0;   // 1 = it was a dotted quad already
    int rc = uresolv_lookup(host, 0, ip);
    if (rc == -ENODEV) {
        fail(req, "no nameserver configured -- run `netd`, or set one in /etc/resolv.conf");
        return -1;
    }
    if (rc != 0) {
        fail(req, "%s: not found", host);
        return -1;
    }
    return 0;
}


int uhttp_fetch(struct uhttp_request *req) {
    if (!req || !req->url) return -1;
    req->err[0] = '\0';
    req->status = 0;
    req->body_bytes = 0;
    req->used_tls = 0;
    req->reused = 0;
    req->cert_err = 0;

    // Two URLs, the current and the next hop, on the heap: each is over
    // 2 KiB (UHTTP_PATH_MAX).
    struct uhttp_url *u = malloc(2 * sizeof *u);
    if (!u) {
        fail(req, "out of memory for the URL");
        return -1;
    }
    struct uhttp_url *nx = u + 1;
    int rc = -1;
    // A 303 turns the next hop into a GET; the caller's request is put
    // back as it was given.
    const char *method0 = req->method;
    const void *body0 = req->body;
    size_t body_len0 = req->body_len;
    if (uhttp_parse_url(req->url, u) != 0) {
        fail(req, "cannot parse '%s' as a URL", req->url);
        goto out;
    }

    if (u->tls && !utls_available()) {
        if (u->scheme_given) {
            fail(req, "this build has no TLS, so https is not available");
            goto out;
        }
        u->tls = 0; // guessed https on a build without it; http is the answer
        if (!u->port_given) u->port = 80;
    }

    for (int hop = 0;; hop++) {
        // Resolved ONCE per host, on first need -- a pooled connection
        // needs no lookup at all. The fallback changes the scheme and the
        // port, never the host, so a second lookup would ask a question
        // already answered, and could answer it differently.
        uint32_t ip = 0;
        char *loc = NULL;
        enum attempt r = attempt_fetch(req, u, &ip, &loc);

        // THE FALLBACK, and its three conditions are all necessary. Only
        // a GUESSED scheme may be downgraded (an explicit https:// that
        // will not connect is an error, not an invitation); only from
        // https; and only when the port did not answer at all.
        if (r == ATTEMPT_NO_CONNECT && hop == 0 && !u->scheme_given && u->tls) {
            if (req->on_fallback) req->on_fallback(req->sink_ctx, u->host);
            u->tls = 0;
            if (!u->port_given) u->port = 80;
            r = attempt_fetch(req, u, &ip, &loc);
        }
        if (r != ATTEMPT_REDIRECT) {
            rc = r == ATTEMPT_OK ? 0 : -1;
            goto out;
        }
        int bad = next_url(u, loc, nx) != 0;
        free(loc);
        if (bad) {
            fail(req, "the server redirected to an address this cannot follow");
            goto out;
        }
        if (u->tls && !nx->tls) {
            fail(req, "refused a redirect from https to plain http");
            goto out;
        }
        if (hop + 1 > req->max_redirects) {
            fail(req, "more than %d redirects", req->max_redirects);
            goto out;
        }
        // 303, and a 301/302 after a POST as every browser does it: the
        // next hop is a GET with no body. 307 and 308 repeat the request.
        if (req->status == 303 || ((req->status == 301 || req->status == 302) && req->method &&
                                   !strcmp(req->method, "POST"))) {
            req->method = NULL;
            req->body = NULL;
            req->body_len = 0;
        }
        struct uhttp_url *t = u; u = nx; nx = t;
    }
out:
    req->method = method0;
    req->body = body0;
    req->body_len = body_len0;
    free(u < nx ? u : nx);   // the one allocation, whichever slot is first
    return rc;
}
