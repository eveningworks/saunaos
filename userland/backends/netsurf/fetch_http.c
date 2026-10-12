// NetSurf's http: and https: fetcher over libhttp -- see fetch_http.h.
//
// THE SHAPE IS UPSTREAM'S CURL FETCHER'S (content/fetchers/curl.c, not
// vendored): the same messages in the same order, the same headers read
// on the way past (Location, WWW-Authenticate, Set-Cookie), the same
// decisions about 304, 3xx, 401 and only_2xx. What differs is that the
// transfer runs in a thread rather than curl's multi handle.
//
// LIFETIME. A fetch is referenced by NetSurf (from setup until its free
// op) and by its thread (from start until the thread ends); whichever
// lets go last frees it, under g_mu. Abort and free only ever set
// `cancel` -- a thread is never waited for on the main thread, and one
// blocked reading a silent server finishes its read first.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "utils/useragent.h"
#include "utils/http/response-codes.h"
#include "netsurf/ssl_certs.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"
#include <nsutils/base64.h>

#include "uhttp.h"
#include "utls.h"
#include "fetch_http.h"

// What one fetch may hold queued before its thread waits for the main
// thread to catch up. A page's thread reads faster than layout consumes.
#define QUEUE_HIGH (512 * 1024)
// A handshake's bignum work and libhttp's frames, with room: the default
// pthread stack (64 KiB) is sized for workers that do not do TLS.
#define FETCH_STACK (256 * 1024)

enum { M_HEADER, M_DATA, M_CERTS, M_DONE };

struct msg {
    struct msg *next;
    int kind;
    // M_DONE: uhttp_fetch()'s result, the status and the cert fault.
    int rc, cert_err;
    struct utls_chain *chain;   // M_CERTS, a deep copy
    size_t len;
    char data[];                // M_HEADER (with its CRLF), M_DATA, M_DONE's err
};

struct hfetch {
    struct fetch *parent;
    nsurl *url;
    char *urlstr;
    bool only_2xx;
    // The request, read-only once the thread has it.
    const char *method;         // "GET" / "POST" (static strings)
    char *body;
    size_t body_len;
    char **headers;             // owned, NULL-terminated
    int nheaders, capheaders;
    int insecure;

    // Shared with the thread, under g_mu.
    int refs;
    volatile int cancel;
    struct msg *head, *tail;
    size_t queued;

    // The main thread's.
    struct hfetch *next;        // the active list
    bool started, linked, aborted, stopped, had_headers, terminal;
    long http_code;
    char *location, *realm;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_room = PTHREAD_COND_INITIALIZER;
static struct hfetch *g_active;
static bool g_in_poll;
static struct uhttp_pool *g_pool;
static int g_schemes;           // how many of http/https are initialised
static char *g_ua;
static void (*g_wake)(void);
static int g_weak;

void ns_fetch_http_set_user_agent(const char *ua) {
    free(g_ua);
    g_ua = ua ? strdup(ua) : NULL;
}

void ns_fetch_http_set_wake(void (*wake)(void)) { g_wake = wake; }
void ns_fetch_http_allow_weak_entropy(int on) { g_weak = on; }

// --- the request -------------------------------------------------------------

static bool add_header(struct hfetch *f, const char *fmt, const char *a, const char *b) {
    if (f->nheaders + 2 > f->capheaders) {
        int cap = f->capheaders ? f->capheaders * 2 : 16;
        char **n = realloc(f->headers, (size_t)cap * sizeof *n);
        if (!n) return false;
        f->headers = n;
        f->capheaders = cap;
    }
    int len = snprintf(NULL, 0, fmt, a, b);
    char *h = malloc((size_t)len + 1);
    if (!h) return false;
    snprintf(h, (size_t)len + 1, fmt, a, b);
    f->headers[f->nheaders++] = h;
    f->headers[f->nheaders] = NULL;
    return true;
}

// multipart/form-data, as a browser sends a form with a file input:
// each part named, a file's bytes read from its raw path here.
static char *multipart_body(const struct fetch_multipart_data *m, const char *boundary,
                            size_t *len_out) {
    size_t cap = 4096, n = 0;
    char *b = malloc(cap);
    if (!b) return NULL;
#define NEED(k) do { while (n + (k) + 1 > cap) { cap *= 2; char *nb = realloc(b, cap); \
                     if (!nb) { free(b); return NULL; } b = nb; } } while (0)
#define PUT(...) do { int k_ = snprintf(NULL, 0, __VA_ARGS__); NEED((size_t)k_); \
                      snprintf(b + n, cap - n, __VA_ARGS__); n += (size_t)k_; } while (0)
    for (; m; m = m->next) {
        if (m->file) {
            PUT("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\n", boundary, m->name, m->value);
            FILE *fp = m->rawfile ? fopen(m->rawfile, "rb") : NULL;
            if (fp) {
                char chunk[1024];
                size_t got;
                while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0) {
                    NEED(got);
                    memcpy(b + n, chunk, got);
                    n += got;
                }
                fclose(fp);
            }
            PUT("\r\n");
        } else {
            PUT("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", boundary,
                m->name, m->value);
        }
    }
    PUT("--%s--\r\n", boundary);
#undef PUT
#undef NEED
    *len_out = n;
    return b;
}

static void hfetch_destroy(struct hfetch *f) {
    for (struct msg *m = f->head, *nx; m; m = nx) {
        nx = m->next;
        if (m->chain) { utls_chain_free(m->chain); free(m->chain); }
        free(m);
    }
    for (int i = 0; i < f->nheaders; i++) free(f->headers[i]);
    free(f->headers);
    free(f->body);
    free(f->urlstr);
    free(f->location);
    free(f->realm);
    if (f->url) nsurl_unref(f->url);
    free(f);
}

// Drops one reference; the last frees. Called with g_mu NOT held.
static void hfetch_unref(struct hfetch *f) {
    pthread_mutex_lock(&g_mu);
    int last = --f->refs == 0;
    pthread_mutex_unlock(&g_mu);
    if (last) hfetch_destroy(f);
}

static void *fh_setup(struct fetch *parent, nsurl *url, bool only_2xx, bool downgrade_tls,
                      const char *post_urlenc,
                      const struct fetch_multipart_data *post_multipart,
                      const char **headers) {
    (void)downgrade_tls;   // mbedTLS offers 1.2 and 1.3 always; there is nothing lower
    struct hfetch *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->parent = parent;
    f->url = nsurl_ref(url);
    f->urlstr = strdup(nsurl_access(url));
    f->only_2xx = only_2xx;
    f->method = "GET";
    f->refs = 1;
    if (!f->urlstr) goto fail;

    for (int i = 0; headers && headers[i]; i++)
        if (!add_header(f, "%s%s", headers[i], "")) goto fail;
    if (post_urlenc) {
        f->method = "POST";
        f->body = strdup(post_urlenc);
        f->body_len = f->body ? strlen(f->body) : 0;
        if (!f->body ||
            !add_header(f, "%s%s", "Content-Type: application/x-www-form-urlencoded", ""))
            goto fail;
    } else if (post_multipart) {
        char boundary[48];
        snprintf(boundary, sizeof boundary, "----toyosFormBoundary%08lx%08lx",
                 (unsigned long)rand(), (unsigned long)rand());
        f->method = "POST";
        f->body = multipart_body(post_multipart, boundary, &f->body_len);
        if (!f->body ||
            !add_header(f, "Content-Type: multipart/form-data; boundary=%s%s", boundary, ""))
            goto fail;
    }
    const char *lang = nsoption_charp(accept_language);
    if (lang && lang[0] && !add_header(f, "Accept-Language: %s%s, *;q=0.1", lang, "")) goto fail;
    if (nsoption_bool(do_not_track) && !add_header(f, "%s%s", "DNT: 1", "")) goto fail;
    if (!add_header(f, "User-Agent: %s%s", g_ua ? g_ua : user_agent_string(), "")) goto fail;
    if (!add_header(f, "%s%s", "Accept: */*", "")) goto fail;
    return f;
fail:
    hfetch_destroy(f);
    return NULL;
}

// --- the thread --------------------------------------------------------------

// Queues one message, waiting while the fetch already holds QUEUE_HIGH.
// A cancelled fetch's messages are dropped: nobody will read them.
static int push(struct hfetch *f, struct msg *m) {
    pthread_mutex_lock(&g_mu);
    while (f->queued > QUEUE_HIGH && !f->cancel) pthread_cond_wait(&g_room, &g_mu);
    int dropped = f->cancel && m->kind != M_DONE;
    // One wake per batch: the main thread drains everything queued, so
    // only the message that finds the queue empty needs to wake it.
    int first = !f->head;
    if (!dropped) {
        if (f->tail) f->tail->next = m;
        else f->head = m;
        f->tail = m;
        f->queued += m->len;
    }
    pthread_mutex_unlock(&g_mu);
    if (dropped) {
        if (m->chain) { utls_chain_free(m->chain); free(m->chain); }
        free(m);
        return -1;
    }
    if (first && g_wake) g_wake();
    return 0;
}

static struct msg *msg_new(int kind, const void *data, size_t len, const char *tail) {
    size_t tl = tail ? strlen(tail) : 0;
    struct msg *m = calloc(1, sizeof *m + len + tl + 1);
    if (!m) return NULL;
    m->kind = kind;
    m->len = len + tl;
    if (len) memcpy(m->data, data, len);
    if (tl) memcpy(m->data + len, tail, tl);
    return m;
}

static void t_header(void *ctx, const char *line, size_t len) {
    // With its CRLF, as curl hands NetSurf a header line.
    struct msg *m = msg_new(M_HEADER, line, len, "\r\n");
    if (m) push(ctx, m);
}

static int t_data(void *ctx, const void *data, size_t len) {
    struct hfetch *f = ctx;
    struct msg *m = msg_new(M_DATA, data, len, NULL);
    if (!m) return 1;
    return push(f, m) == 0 ? 0 : 1;
}

static void t_certs(void *ctx, const struct utls_chain *chain) {
    struct msg *m = msg_new(M_CERTS, NULL, 0, NULL);
    if (!m) return;
    m->chain = calloc(1, sizeof *m->chain);
    if (m->chain) {
        *m->chain = *chain;
        for (int i = 0; i < chain->depth && i < UTLS_CHAIN_MAX; i++) {
            m->chain->certs[i].der = malloc(chain->certs[i].der_len ? chain->certs[i].der_len : 1);
            if (m->chain->certs[i].der && chain->certs[i].der_len)
                memcpy(m->chain->certs[i].der, chain->certs[i].der, chain->certs[i].der_len);
            else if (!m->chain->certs[i].der)
                m->chain->certs[i].der_len = 0;
        }
        for (int i = chain->depth; i < UTLS_CHAIN_MAX; i++) m->chain->certs[i].der = NULL;
    }
    push(ctx, m);
}

static void *worker(void *arg) {
    struct hfetch *f = arg;
    struct uhttp_request *req = calloc(1, sizeof *req);
    int rc = -1;
    if (req) {
        req->url = f->urlstr;
        req->method = f->method;
        req->body = f->body;
        req->body_len = f->body_len;
        req->headers = (const char *const *)f->headers;
        req->pool = g_pool;
        req->cancel = &f->cancel;
        req->insecure = f->insecure;
        req->allow_weak_entropy = g_weak;
        req->max_redirects = 0;          // NetSurf follows them itself
        req->sink = t_data;
        req->sink_ctx = f;
        req->on_header = t_header;
        req->on_certs = t_certs;
        rc = uhttp_fetch(req);
    }
    const char *err = req ? req->err : "out of memory";
    struct msg *m = msg_new(M_DONE, err, strlen(err), NULL);
    if (m) {
        m->rc = rc;
        m->cert_err = req ? req->cert_err : 0;
        push(f, m);
    }
    free(req);
    hfetch_unref(f);
    return NULL;
}

static bool fh_start(void *vf) {
    struct hfetch *f = vf;
    // On the main thread: urldb is NetSurf's and is not locked.
    char *cookie = urldb_get_cookie(f->url, true);
    if (cookie) {
        add_header(f, "Cookie: %s%s", cookie, "");
        free(cookie);
    }
    const char *auth = urldb_get_auth_details(f->url, NULL);
    if (auth) {
        uint8_t *b64 = NULL;
        size_t n = 0;
        if (nsu_base64_encode_alloc((const uint8_t *)auth, strlen(auth), &b64, &n) == NSUERROR_OK) {
            char *s = malloc(n + 1);
            if (s) {
                memcpy(s, b64, n);
                s[n] = 0;
                add_header(f, "Authorization: Basic %s%s", s, "");
                free(s);
            }
            free(b64);
        }
    }
    // "Proceed anyway" on the certificate page records this for the host.
    f->insecure = urldb_get_cert_permissions(f->url);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, FETCH_STACK);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    f->refs++;   // the thread's; no thread exists yet to race it
    pthread_t t;
    if (pthread_create(&t, &at, worker, f) != 0) {
        f->refs--;
        pthread_attr_destroy(&at);
        return false;
    }
    pthread_attr_destroy(&at);
    f->started = true;
    f->next = g_active;
    g_active = f;
    f->linked = true;
    return true;
}

static void unlink_active(struct hfetch *f) {
    if (!f->linked) return;
    for (struct hfetch **p = &g_active; *p; p = &(*p)->next)
        if (*p == f) { *p = f->next; break; }
    f->linked = false;
}

static void stop_thread(struct hfetch *f) {
    pthread_mutex_lock(&g_mu);
    f->cancel = 1;
    pthread_cond_broadcast(&g_room);
    pthread_mutex_unlock(&g_mu);
}

static void fh_free(void *vf) {
    struct hfetch *f = vf;
    unlink_active(f);
    stop_thread(f);
    hfetch_unref(f);
}

static void fh_abort(void *vf) {
    struct hfetch *f = vf;
    stop_thread(f);
    // Inside poll a callback may abort ANY fetch, and poll is walking the
    // list: the abort is noted and poll finishes it, as curl.c defers its
    // own while inside curl.
    if (g_in_poll && f->linked) {
        f->aborted = true;
        return;
    }
    fetch_remove_from_queues(f->parent);
    fetch_free(f->parent);
}

// --- the main thread's side --------------------------------------------------

static void send_simple(struct hfetch *f, fetch_msg_type type) {
    fetch_msg msg = { .type = type };
    fetch_send_callback(&msg, f->parent);
}

// curl.c's fetch_curl_process_headers: true when the response has been
// answered with a terminal message and its body is not wanted.
static bool process_headers(struct hfetch *f) {
    fetch_msg msg;
    f->had_headers = true;
    long code = f->http_code;
    if (code == HTTP_RESPONSE_NOT_MODIFIED && !strcmp(f->method, "GET")) {
        send_simple(f, FETCH_NOTMODIFIED);
        return true;
    }
    if (HTTP_RESPONSE_IS_3XX(code) && f->location) {
        msg.type = FETCH_REDIRECT;
        msg.data.redirect = f->location;
        fetch_send_callback(&msg, f->parent);
        return true;
    }
    if (code == HTTP_RESPONSE_UNAUTHORIZED) {
        msg.type = FETCH_AUTH;
        msg.data.auth.realm = f->realm;
        fetch_send_callback(&msg, f->parent);
        return true;
    }
    if (f->only_2xx && !HTTP_RESPONSE_IS_2XX(code)) {
        msg.type = FETCH_ERROR;
        msg.data.error = messages_get("Not2xx");
        fetch_send_callback(&msg, f->parent);
        return true;
    }
    return false;
}

static char *value_dup(const char *line, size_t len, size_t skip) {
    while (skip < len && (line[skip] == ' ' || line[skip] == '\t')) skip++;
    size_t end = len;
    while (end > skip && (line[end - 1] == '\r' || line[end - 1] == '\n' ||
                          line[end - 1] == ' ' || line[end - 1] == '\t'))
        end--;
    char *v = malloc(end - skip + 1);
    if (v) {
        memcpy(v, line + skip, end - skip);
        v[end - skip] = 0;
    }
    return v;
}

// strcasestr(), which tolibc does not have, over a counted line.
static const char *find_ci(const char *s, size_t len, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++)
        if (!strncasecmp(s + i, needle, n)) return s + i;
    return NULL;
}

static void on_header_line(struct hfetch *f, const char *line, size_t len) {
    if (len > 9 && !strncmp(line, "HTTP/1.", 7)) {
        f->http_code = strtol(line + 9, NULL, 10);
        fetch_set_http_code(f->parent, (http_response_code)f->http_code);
        free(f->location);
        f->location = NULL;
    }
    fetch_msg msg = { .type = FETCH_HEADER };
    msg.data.header_or_data.buf = (const uint8_t *)line;
    msg.data.header_or_data.len = len;
    fetch_send_callback(&msg, f->parent);

    if (len > 9 && !strncasecmp(line, "Location:", 9)) {
        free(f->location);
        f->location = value_dup(line, len, 9);
    } else if (len > 17 && !strncasecmp(line, "WWW-Authenticate:", 17)) {
        // The first realm="..." -- what NetSurf asks a login for.
        const char *r = find_ci(line, len, "realm=\"");
        if (r) {
            r += 7;
            const char *q = memchr(r, '"', (size_t)(line + len - r));
            if (q) {
                free(f->realm);
                f->realm = strndup(r, (size_t)(q - r));
            }
        }
    } else if (len > 11 && !strncasecmp(line, "Set-Cookie:", 11)) {
        char *v = value_dup(line, len, 11);
        if (v) {
            fetch_set_cookie(f->parent, v);
            free(v);
        }
    }
}

static ssl_cert_err cert_err_of(enum utls_cert_err e) {
    switch (e) {
    case UTLS_CERT_OK:            return SSL_CERT_ERR_OK;
    case UTLS_CERT_UNTRUSTED:     return SSL_CERT_ERR_BAD_ISSUER;
    case UTLS_CERT_BAD_SIG:       return SSL_CERT_ERR_BAD_SIG;
    case UTLS_CERT_NOT_YET_VALID: return SSL_CERT_ERR_TOO_YOUNG;
    case UTLS_CERT_EXPIRED:       return SSL_CERT_ERR_TOO_OLD;
    case UTLS_CERT_SELF_SIGNED:   return SSL_CERT_ERR_SELF_SIGNED;
    case UTLS_CERT_REVOKED:       return SSL_CERT_ERR_REVOKED;
    case UTLS_CERT_NAME_MISMATCH: return SSL_CERT_ERR_HOSTNAME_MISMATCH;
    case UTLS_CERT_UNKNOWN:       break;
    }
    return SSL_CERT_ERR_UNKNOWN;
}

static void send_certs(struct hfetch *f, const struct utls_chain *c) {
    struct cert_chain *chain;
    size_t depth = c->depth < MAX_CERT_DEPTH ? (size_t)c->depth : MAX_CERT_DEPTH;
    if (!depth || cert_chain_alloc(depth, &chain) != NSERROR_OK) return;
    for (size_t i = 0; i < depth; i++) {
        chain->certs[i].err = cert_err_of(c->certs[i].err);
        chain->certs[i].der = malloc(c->certs[i].der_len ? c->certs[i].der_len : 1);
        chain->certs[i].der_length = chain->certs[i].der ? c->certs[i].der_len : 0;
        if (chain->certs[i].der && c->certs[i].der_len)
            memcpy(chain->certs[i].der, c->certs[i].der, c->certs[i].der_len);
    }
    fetch_msg msg = { .type = FETCH_CERTS };
    msg.data.chain = chain;
    fetch_send_callback(&msg, f->parent);
    cert_chain_free(chain);
}

// One message. Sets f->terminal once a message that ends the fetch has
// been sent.
static void handle(struct hfetch *f, struct msg *m) {
    switch (m->kind) {
    case M_HEADER:
        on_header_line(f, m->data, m->len);
        break;
    case M_CERTS:
        if (m->chain) send_certs(f, m->chain);
        break;
    case M_DATA:
        if (!f->had_headers && process_headers(f)) {
            f->stopped = f->terminal = true;
            stop_thread(f);
            break;
        }
        {
            fetch_msg msg = { .type = FETCH_DATA };
            msg.data.header_or_data.buf = (const uint8_t *)m->data;
            msg.data.header_or_data.len = m->len;
            fetch_send_callback(&msg, f->parent);
        }
        break;
    case M_DONE:
        f->terminal = true;
        if (m->rc == 0) {
            if (!f->had_headers && process_headers(f)) break;   // a redirect with no body
            send_simple(f, FETCH_FINISHED);
        } else if (m->cert_err) {
            send_simple(f, FETCH_CERT_ERR);   // NetSurf asks; FETCH_CERTS went first
        } else {
            NSLOG(netsurf, INFO, "fetch %s: %s", f->urlstr, m->data);
            fetch_msg msg = { .type = FETCH_ERROR };
            msg.data.error = m->data;
            fetch_send_callback(&msg, f->parent);
        }
        break;
    }
}

static void fh_poll(lwc_string *scheme) {
    (void)scheme;
    if (g_in_poll) return;
    g_in_poll = true;
    for (struct hfetch *f = g_active, *next; f; f = next) {
        next = f->next;
        pthread_mutex_lock(&g_mu);
        struct msg *list = f->head;
        f->head = f->tail = NULL;
        f->queued = 0;
        pthread_cond_broadcast(&g_room);
        pthread_mutex_unlock(&g_mu);

        for (struct msg *m = list, *nx; m; m = nx) {
            nx = m->next;
            if (!f->aborted && !f->terminal) handle(f, m);
            if (m->chain) { utls_chain_free(m->chain); free(m->chain); }
            free(m);
        }
        if (f->aborted || f->terminal) {
            // Only THIS fetch leaves the list here, so `next` stays valid:
            // a callback's abort of another one was deferred above.
            unlink_active(f);
            fetch_remove_from_queues(f->parent);
            fetch_free(f->parent);
        }
    }
    g_in_poll = false;
}

static bool fh_initialise(lwc_string *scheme) {
    (void)scheme;
    if (!g_pool) {
        // Chrome's and Firefox's six per host; NetSurf's own fetch queue
        // (max_fetchers_per_host) keeps the active count inside it.
        struct uhttp_pool_limits lim = { .per_host = 6, .total = 16, .idle_ms = 30000 };
        g_pool = uhttp_pool_new(&lim);
        if (!g_pool) return false;
    }
    g_schemes++;
    return true;
}

static void fh_finalise(lwc_string *scheme) {
    (void)scheme;
    if (--g_schemes > 0) return;
    // A thread still running holds connections taken from the pool and
    // will hand them back to it; freeing it under that thread would be a
    // use after free. Every fetch has been freed by now, so each thread
    // is cancelled and ends at its next read -- the pool is left for the
    // exit to reclaim rather than waited for.
    if (!g_active) {
        uhttp_pool_free(g_pool);
        g_pool = NULL;
    }
}

static bool fh_acceptable(const nsurl *url) {
    (void)url;
    return true;
}

static const struct fetcher_operation_table g_ops = {
    .initialise = fh_initialise,
    .acceptable = fh_acceptable,
    .setup = fh_setup,
    .start = fh_start,
    .abort = fh_abort,
    .free = fh_free,
    .poll = fh_poll,
    .fdset = NULL,            // no select() here: the threads are the waiting
    .finalise = fh_finalise,
};

int ns_fetch_http_register(void) {
    static const char *const schemes[] = { "http", "https" };
    for (int i = 0; i < 2; i++) {
        lwc_string *s;
        if (lwc_intern_string(schemes[i], strlen(schemes[i]), &s) != lwc_error_ok) return -1;
        if (fetcher_add(s, &g_ops) != NSERROR_OK) return -1;
    }
    return 0;
}
