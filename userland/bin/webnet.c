// webnet -- the Web Browser's network process (docs/browser-design.md,
// stage 5): WebKit's NetworkProcess, Chrome's Network Service. One per
// browser, started by it with `--app <pid>`; it ends when the browser
// does.
//
// It owns what a content process must not: the sockets and TLS (through
// libhttp and its connection pool), the cookie jar (lib/ucookie.h), the
// certificates a person accepted anyway, and downloads -- so a download
// outlives the tab that started it. Content processes fetch through it
// with lib/webnet_proto.h's records; the browser app asks it about
// cookies and hears about downloads.
//
// EACH FETCH IS A THREAD, as in the in-process fetcher it replaces
// (backends/netsurf/fetch_http.c): a toy-os socket only blocks. A worker
// writes its own responses into its client's ring (ubulk_server_send()
// takes the lock); the main thread reads requests and nothing else.
//
// A CONTENT PROCESS DECIDES A RESPONSE IS A DOWNLOAD, from bytes it has
// already been sent -- NetSurf sniffs. So a response that might be one
// (a type NetSurf does not draw) is copied here as it goes, up to
// REPLAY_MAX: when the content process says WN_DOWNLOAD, the file starts
// from that copy and the rest of the response goes to the file. A
// response that outgrew the copy is fetched again from the start.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lib/ubulk.h"
#include "lib/ucookie.h"
#include "lib/uargs.h"
#include "lib/webnet_proto.h"
#include "rt/sys.h"
#include "uhttp.h"
#include "utls.h"
#include "nspsl.h"

#define STATE_DIR     "/var/lib/browser"
#define JAR_FILE      STATE_DIR "/cookie-jar"
#define NETSURF_JAR   STATE_DIR "/cookies"      // NetSurf's own, read once
#define DOWNLOAD_DIR  "/home/Downloads"
#define FETCH_STACK   (256 * 1024)
#define REPLAY_MAX    (1024 * 1024)
#define SEND_WAIT_MS  60000                      // a content process that stopped reading
#define SAVE_EVERY_MS 10000

struct nfetch {
    struct nfetch *next;
    int pid;                  // the content process
    uint32_t id;              // its number for the fetch
    char *url;
    int post;
    char **headers;           // NULL-terminated, ours
    int nheaders;
    char *body;
    size_t body_len, body_got;
    int started, finished;    // the worker ran / ended
    volatile int cancel;
    int freed;                // the content process let go (WN_ABORT)
    int refs;                 // the list's and the worker's

    // The response, as the worker sees it.
    int keep_replay;          // a type NetSurf might hand back as a download
    char *replay;
    size_t replay_len;
    int replay_over;          // the response outgrew REPLAY_MAX

    // A download: set by the main thread, acted on by the worker.
    char *dl_name;            // asked for; NULL when not
    int dl;                   // the download's number once it is one
    // Let go by the page but kept a moment: a whole small response may
    // still be asked for as a download (NetSurf decides on its NEXT poll).
    unsigned long long linger_until;
};

struct download {
    uint32_t id;
    int state;
    int64_t got, total;
    char name[128], path[256], part[264], url[1024];
    FILE *f;
    struct nfetch *fetch;     // the fetch writing it
    unsigned long long last_report_ns;
};

#define DL_MAX 32
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // everything below
static struct nfetch *g_fetches;
static struct download g_dl[DL_MAX];
static int g_ndl;
static uint32_t g_next_dl = 1;
static struct ucookie_jar g_jar;
static char *g_allow[64];     // hosts whose certificate was accepted anyway
static int g_nallow;

static struct ubulk_server g_srv;
static struct uhttp_pool *g_pool;
static int g_app_pid, g_app_client, g_weak;

static int64_t now_s(void) { return (int64_t)time(NULL); }

static int is_public_suffix(const char *domain) {
    const char *s = nspsl_getpublicsuffix(domain);
    return s && !strcmp(s, domain);
}

// --- URLs ----------------------------------------------------------------

// What the jar keys a URL by. On the heap: a path is 2 KiB, and ring 3's
// frames are budgeted at that.
struct urlbits {
    char host[UHTTP_HOST_MAX];
    char path[UHTTP_PATH_MAX];
    int secure;
};

// `url`'s host and path (no query), https in `secure`. NULL when it does
// not parse; free() the result.
static struct urlbits *url_bits(const char *url) {
    struct uhttp_url *u = malloc(sizeof *u);
    struct urlbits *b = malloc(sizeof *b);
    if (!u || !b || uhttp_parse_url(url, u) != 0) {
        free(u);
        free(b);
        return NULL;
    }
    snprintf(b->host, sizeof b->host, "%s", u->host);
    snprintf(b->path, sizeof b->path, "%s", u->path[0] ? u->path : "/");
    b->path[strcspn(b->path, "?#")] = '\0';
    if (!b->path[0]) snprintf(b->path, sizeof b->path, "/");
    b->secure = u->tls;
    free(u);
    return b;
}

static int host_allowed(const char *url) {
    struct urlbits *b = url_bits(url);
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; b && i < g_nallow; i++) ok |= !strcasecmp(g_allow[i], b->host);
    pthread_mutex_unlock(&g_mu);
    free(b);
    return ok;
}

// --- downloads -------------------------------------------------------------

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// The name made safe for one path component and unique in the folder,
// as Chrome does: "name (1).ext".
static void choose_path(const char *suggested, char *out, size_t cap) {
    char name[96];
    snprintf(name, sizeof name, "%s", suggested && suggested[0] ? suggested : "download");
    for (char *c = name; *c; c++)
        if (*c == '/' || (unsigned char)*c < 0x20) *c = '_';
    if (name[0] == '.') name[0] = '_';   // never hidden, never "." or ".."
    snprintf(out, cap, DOWNLOAD_DIR "/%s", name);
    char part[280];
    snprintf(part, sizeof part, "%s.part", out);
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for (int n = 1; (exists(out) || exists(part)) && n < 100; n++) {
        snprintf(out, cap, DOWNLOAD_DIR "/%.*s (%d)%s", stem, name, n, name + stem);
        snprintf(part, sizeof part, "%s.part", out);
    }
}

static struct download *dl_find(uint32_t id) {
    for (int i = 0; i < g_ndl; i++) if (g_dl[i].id == id) return &g_dl[i];
    return NULL;
}

// Tells the browser app where download `d` stands. Under g_mu.
static void dl_report(struct download *d, int force) {
    unsigned long long now = sys_monotonic_ns();
    if (!force && now - d->last_report_ns < 250000000ull) return;
    d->last_report_ns = now;
    if (!g_app_client) return;
    char buf[sizeof(struct wn_dl) + 128 + 256 + 1024];
    struct wn_dl h = { d->state, d->got, d->total, (int32_t)strlen(d->name),
                       (int32_t)strlen(d->path), (int32_t)strlen(d->url) };
    size_t n = 0;
    memcpy(buf, &h, sizeof h);
    n += sizeof h;
    memcpy(buf + n, d->name, (size_t)h.name_len), n += (size_t)h.name_len;
    memcpy(buf + n, d->path, (size_t)h.path_len), n += (size_t)h.path_len;
    memcpy(buf + n, d->url, (size_t)h.url_len), n += (size_t)h.url_len;
    ubulk_server_send(&g_srv, g_app_client, WN_DL, d->id, buf, (uint32_t)n, 1000);
}

// A new download for `url`, its file opened. Under g_mu. NULL when the
// list is full of running ones.
static struct download *dl_new(const char *name, const char *url) {
    if (g_ndl == DL_MAX) {
        int k = 0;
        while (k < g_ndl && g_dl[k].state == WN_DL_RUNNING) k++;
        if (k == g_ndl) return NULL;
        g_dl[k] = g_dl[--g_ndl];
    }
    struct download *d = &g_dl[g_ndl++];
    memset(d, 0, sizeof *d);
    d->id = g_next_dl++;
    d->total = -1;
    mkdir(DOWNLOAD_DIR, 0755);
    choose_path(name, d->path, sizeof d->path);
    snprintf(d->part, sizeof d->part, "%s.part", d->path);
    const char *base = strrchr(d->path, '/');
    snprintf(d->name, sizeof d->name, "%s", base ? base + 1 : d->path);
    snprintf(d->url, sizeof d->url, "%s", url);
    d->f = fopen(d->part, "wb");
    d->state = d->f ? WN_DL_RUNNING : WN_DL_FAILED;
    printf("webnet: download start %s\n", d->name);
    return d;
}

static void dl_end(struct download *d, int state) {
    if (d->f) fclose(d->f);
    d->f = NULL;
    if (state == WN_DL_DONE && rename(d->part, d->path) != 0) state = WN_DL_FAILED;
    if (state != WN_DL_DONE) unlink(d->part);
    d->state = state;
    d->fetch = NULL;
    printf("webnet: download %s %s %lld\n",
           state == WN_DL_DONE ? "done" : state == WN_DL_CANCELLED ? "cancelled" : "failed", d->name,
           (long long)d->got);
    dl_report(d, 1);
}

// --- one fetch ---------------------------------------------------------------

static void fetch_free(struct nfetch *f) {
    for (int i = 0; i < f->nheaders; i++) free(f->headers[i]);
    free(f->headers);
    free(f->body);
    free(f->url);
    free(f->replay);
    free(f->dl_name);
    free(f);
}

// Drops a reference; the last unlinks and frees. Under g_mu.
static void fetch_unref(struct nfetch *f) {
    if (--f->refs > 0) return;
    for (struct nfetch **p = &g_fetches; *p; p = &(*p)->next)
        if (*p == f) { *p = f->next; break; }
    fetch_free(f);
}

// To the content process, unless the fetch became a download or was let
// go. Called WITHOUT g_mu: a full ring waits here, and the main thread
// must stay free to read.
static int to_client(struct nfetch *f, uint32_t type, const void *data, uint32_t len) {
    if (f->cancel || f->dl || f->freed) return 0;
    if (ubulk_server_send(&g_srv, f->pid, type, f->id, data, len, SEND_WAIT_MS) != 0) {
        f->cancel = 1;   // the content process is gone or wedged
        return -1;
    }
    return 0;
}

// Becomes a download if the content process asked: the copy so far goes
// to the file first. Under g_mu. 0 to carry on, -1 when it cannot (the
// copy outgrew REPLAY_MAX: the main thread fetches it again).
static int maybe_become_download(struct nfetch *f) {
    if (!f->dl_name || f->dl) return 0;
    if (f->replay_over || !f->keep_replay) return -1;
    struct download *d = dl_new(f->dl_name, f->url);
    if (!d) return -1;
    f->dl = (int)d->id;
    d->fetch = f;
    if (d->f && f->replay_len && fwrite(f->replay, 1, f->replay_len, d->f) != f->replay_len)
        dl_end(d, WN_DL_FAILED);
    d->got = (int64_t)f->replay_len;
    free(f->replay);
    f->replay = NULL;
    dl_report(d, 1);
    return 0;
}

// The types NetSurf draws in a page: never a download, never copied.
static int drawn_type(const char *v) {
    static const char *const T[] = { "text/html", "text/css", "text/plain", "image/",
                                     "application/xhtml", "text/javascript",
                                     "application/javascript", "font/" };
    while (*v == ' ') v++;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strncasecmp(v, T[i], strlen(T[i]))) return 1;
    return 0;
}

static void w_header(void *ctx, const char *line, size_t len) {
    struct nfetch *f = ctx;
    if (len > 9 && !strncmp(line, "HTTP/1.", 7)) {
        // A new response (a 100 Continue's successor): start the copy over.
        pthread_mutex_lock(&g_mu);
        f->keep_replay = 1;
        f->replay_len = 0;
        f->replay_over = 0;
        pthread_mutex_unlock(&g_mu);
    } else if (len > 13 && !strncasecmp(line, "Content-Type:", 13)) {
        char v[96];
        snprintf(v, sizeof v, "%.*s", (int)(len - 13), line + 13);
        if (drawn_type(v)) f->keep_replay = 0;
    } else if (len > 11 && !strncasecmp(line, "Set-Cookie:", 11)) {
        char *v = strndup(line + 11, len - 11);
        struct urlbits *b = v ? url_bits(f->url) : NULL;
        if (b) {
            pthread_mutex_lock(&g_mu);
            ucookie_set(&g_jar, b->host, b->path, b->secure, v, now_s());
            pthread_mutex_unlock(&g_mu);
        }
        free(b);
        free(v);
    }
    char *buf = malloc(len + 2);   // with its CRLF, as curl hands NetSurf a header
    if (!buf) return;
    memcpy(buf, line, len);
    memcpy(buf + len, "\r\n", 2);
    to_client(f, WN_HEADER, buf, (uint32_t)len + 2);
    free(buf);
}

static int w_data(void *ctx, const void *data, size_t len) {
    struct nfetch *f = ctx;
    pthread_mutex_lock(&g_mu);
    if (maybe_become_download(f) != 0) f->cancel = 1;
    struct download *d = f->dl ? dl_find((uint32_t)f->dl) : NULL;
    if (d) {
        if (d->state == WN_DL_RUNNING && d->f && fwrite(data, 1, len, d->f) != len) dl_end(d, WN_DL_FAILED);
        d->got += (int64_t)len;
        int stop = d->state != WN_DL_RUNNING;
        if (!stop) dl_report(d, 0);
        pthread_mutex_unlock(&g_mu);
        return stop;
    }
    if (f->keep_replay && !f->replay_over) {
        if (f->replay_len + len > REPLAY_MAX) {
            f->replay_over = 1;
            free(f->replay);
            f->replay = NULL;
        } else {
            char *g = realloc(f->replay, f->replay_len + len);
            if (g) {
                memcpy(g + f->replay_len, data, len);
                f->replay = g;
                f->replay_len += len;
            } else {
                f->replay_over = 1;
            }
        }
    }
    int cancel = f->cancel;
    pthread_mutex_unlock(&g_mu);
    if (cancel) return 1;
    // A body arrives in pieces no bigger than a socket read; a record
    // carries one.
    const char *p = data;
    for (size_t off = 0; off < len;) {
        uint32_t n = len - off > UBULK_REC_MAX ? UBULK_REC_MAX : (uint32_t)(len - off);
        if (to_client(f, WN_DATA, p + off, n) != 0) break;
        off += n;
    }
    return 0;   // a gone client stops being written to; a download may still follow
}

static void w_certs(void *ctx, const struct utls_chain *c) {
    struct nfetch *f = ctx;
    char *buf = malloc(UBULK_REC_MAX);
    if (!buf) return;
    struct wn_certs h = { 0 };
    size_t n = sizeof h;
    for (int i = 0; i < c->depth && i < WN_CERTS_MAX; i++) {
        if (n + c->certs[i].der_len > UBULK_REC_MAX) break;
        memcpy(buf + n, c->certs[i].der, c->certs[i].der_len);
        n += c->certs[i].der_len;
        h.err[i] = c->certs[i].err;
        h.len[i] = (int32_t)c->certs[i].der_len;
        h.depth = i + 1;
    }
    memcpy(buf, &h, sizeof h);
    to_client(f, WN_CERTS, buf, (uint32_t)n);
    free(buf);
}

static void w_connect(void *ctx, uint32_t ip, const char *version, const char *cipher) {
    (void)ip;
    if (!version) return;   // plain http
    char buf[128];
    int a = snprintf(buf, sizeof buf, "%s", version) + 1;
    int b = snprintf(buf + a, sizeof buf - (size_t)a, "%s", cipher ? cipher : "");
    to_client(ctx, WN_TLS, buf, (uint32_t)(a + b));
}

static void *worker(void *arg) {
    struct nfetch *f = arg;
    struct uhttp_request *req = calloc(1, sizeof *req);
    int rc = -1;
    if (req) {
        req->url = f->url;
        req->method = f->post ? "POST" : "GET";
        req->body = f->body;
        req->body_len = f->body_len;
        req->headers = (const char *const *)f->headers;
        req->pool = g_pool;
        req->cancel = &f->cancel;
        req->insecure = host_allowed(f->url);
        req->allow_weak_entropy = g_weak;
        req->max_redirects = 0;          // NetSurf follows them itself
        req->sink = w_data;
        req->sink_ctx = f;
        req->on_header = w_header;
        req->on_certs = w_certs;
        req->on_connect = w_connect;
        rc = uhttp_fetch(req);
    }
    // A request for a download that arrived after the body ended.
    pthread_mutex_lock(&g_mu);
    if (rc == 0 && maybe_become_download(f) != 0) f->cancel = 1;
    struct download *d = f->dl ? dl_find((uint32_t)f->dl) : NULL;
    if (d && d->state == WN_DL_RUNNING) {
        if (rc == 0) d->total = d->got;
        dl_end(d, rc == 0 ? WN_DL_DONE : f->cancel ? WN_DL_CANCELLED : WN_DL_FAILED);
    }
    f->finished = 1;
    pthread_mutex_unlock(&g_mu);

    const char *err = req ? req->err : "out of memory";
    char buf[sizeof(struct wn_done) + UHTTP_ERR_MAX];
    struct wn_done h = { rc, req ? req->cert_err : 0 };
    memcpy(buf, &h, sizeof h);
    size_t el = rc == 0 ? 0 : strlen(err);
    memcpy(buf + sizeof h, err, el);
    // The end goes to the page however the fetch ended -- one that THIS
    // side stopped too, or the page waits for it forever. Not to a page
    // that let it go, nor for a response that became a download.
    if (!f->freed && !f->dl)
        ubulk_server_send(&g_srv, f->pid, WN_DONE, f->id, buf, (uint32_t)(sizeof h + el), SEND_WAIT_MS);
    free(req);
    pthread_mutex_lock(&g_mu);
    fetch_unref(f);
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

// Starts the worker; the request is whole. Under g_mu.
static void fetch_start(struct nfetch *f) {
    // The Cookie header is the jar's: whatever the content process sent
    // is dropped, so a compromised page cannot read another site's
    // cookies out of a header it made up.
    static char cookie[8192];   // under g_mu
    struct urlbits *b = url_bits(f->url);
    if (b && ucookie_header(&g_jar, b->host, b->path, b->secure, now_s(), cookie, sizeof cookie)) {
        char *h = malloc(strlen(cookie) + 9);
        if (h) {
            sprintf(h, "Cookie: %s", cookie);
            f->headers[f->nheaders++] = h;
            f->headers[f->nheaders] = NULL;
        }
    }
    free(b);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, FETCH_STACK);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    f->refs++;
    f->started = 1;
    pthread_t t;
    if (pthread_create(&t, &at, worker, f) != 0) {
        f->refs--;
        f->started = 0;
        f->finished = 1;
        struct wn_done h = { -1, 0 };
        char buf[sizeof h + 32];
        memcpy(buf, &h, sizeof h);
        memcpy(buf + sizeof h, "no thread for the fetch", 23);
        pthread_mutex_unlock(&g_mu);
        ubulk_server_send(&g_srv, f->pid, WN_DONE, f->id, buf, sizeof h + 23, 1000);
        pthread_mutex_lock(&g_mu);
    }
    pthread_attr_destroy(&at);
}

static struct nfetch *fetch_find(int pid, uint32_t id) {
    for (struct nfetch *f = g_fetches; f; f = f->next)
        if (f->pid == pid && f->id == id && (!f->freed || f->linger_until)) return f;
    return NULL;
}

#define LINGER_NS 5000000000ull

// The lingering fetches whose moment has passed. Under g_mu.
static void linger_sweep(void) {
    unsigned long long now = sys_monotonic_ns();
    for (struct nfetch *f = g_fetches, *next; f; f = next) {
        next = f->next;
        if (f->linger_until && now >= f->linger_until) {
            f->linger_until = 0;
            fetch_unref(f);
        }
    }
}

// --- requests ------------------------------------------------------------------

static void on_fetch(const struct ubulk_msg *m) {
    struct wn_fetch h;
    if (m->len < sizeof h) return;
    memcpy(&h, m->data, sizeof h);
    const char *p = (const char *)m->data + sizeof h;
    size_t rest = m->len - sizeof h;
    if (h.url_len <= 0 || h.headers_len < 0 || h.body_len < 0 ||
        (size_t)h.url_len + (size_t)h.headers_len > rest)
        return;
    struct nfetch *f = calloc(1, sizeof *f);
    if (!f) return;
    f->pid = m->pid;
    f->id = m->id;
    f->post = h.method == WN_METHOD_POST;
    f->url = strndup(p, (size_t)h.url_len);
    f->refs = 1;
    int nh = 0;
    for (int i = 0; i < h.headers_len; i++) nh += p[h.url_len + i] == '\0';
    f->headers = calloc((size_t)nh + 2, sizeof *f->headers);   // + Cookie + NULL
    for (int i = 0, at = h.url_len; f->headers && i < nh; i++) {
        const char *line = p + at;
        at += (int)strlen(line) + 1;
        if (!strncasecmp(line, "Cookie:", 7)) continue;
        f->headers[f->nheaders++] = strdup(line);
    }
    f->body_len = (size_t)h.body_len;
    size_t first = rest - (size_t)h.url_len - (size_t)h.headers_len;
    if (first > f->body_len) first = f->body_len;
    if (f->body_len) {
        f->body = malloc(f->body_len);
        if (f->body) memcpy(f->body, p + h.url_len + h.headers_len, first);
        f->body_got = first;
    }
    pthread_mutex_lock(&g_mu);
    f->next = g_fetches;
    g_fetches = f;
    if (!f->url || !f->headers || (f->body_len && !f->body)) {
        f->freed = 1;
        fetch_unref(f);
    } else if (f->body_got == f->body_len) {
        fetch_start(f);
    }
    pthread_mutex_unlock(&g_mu);
}

static void on_body(const struct ubulk_msg *m) {
    pthread_mutex_lock(&g_mu);
    struct nfetch *f = fetch_find(m->pid, m->id);
    if (f && !f->started && f->body && f->body_got + m->len <= f->body_len) {
        memcpy(f->body + f->body_got, m->data, m->len);
        f->body_got += m->len;
        if (f->body_got == f->body_len) fetch_start(f);
    }
    pthread_mutex_unlock(&g_mu);
}

static void on_abort(const struct ubulk_msg *m) {
    pthread_mutex_lock(&g_mu);
    struct nfetch *f = fetch_find(m->pid, m->id);
    if (f && !f->freed) {
        f->freed = 1;
        if (!f->dl && !f->dl_name) f->cancel = 1;   // a download goes on without the page
        if (f->finished && f->keep_replay && f->replay && !f->replay_over && !f->dl)
            f->linger_until = sys_monotonic_ns() + LINGER_NS;   // the list's reference, kept
        else
            fetch_unref(f);
    }
    pthread_mutex_unlock(&g_mu);
}

// Fetches `url` again from the start, straight into a new download:
// what WN_DOWNLOAD becomes when the copy of the first response is gone.
static void refetch_as_download(const char *url, const char *name) {
    struct nfetch *f = calloc(1, sizeof *f);
    if (!f) return;
    f->url = strdup(url);
    f->headers = calloc(2, sizeof *f->headers);
    f->refs = 1;
    f->freed = 1;              // no content process is waiting on it
    f->keep_replay = 0;
    if (!f->url || !f->headers) { fetch_free(f); return; }
    struct download *d = dl_new(name, url);
    if (!d) { fetch_free(f); return; }
    f->dl = (int)d->id;
    d->fetch = f;
    dl_report(d, 1);
    f->next = g_fetches;
    g_fetches = f;
    fetch_start(f);
    fetch_unref(f);            // the worker's reference keeps it
}

static void on_download(const struct ubulk_msg *m) {
    pthread_mutex_lock(&g_mu);
    struct nfetch *f = fetch_find(m->pid, m->id);
    if (f && !f->dl && !f->dl_name) {
        f->dl_name = strndup(m->data, m->len);
        if (f->finished) {
            // The whole response already went to the content process:
            // the copy is the file.
            if (maybe_become_download(f) == 0) {
                struct download *d = dl_find((uint32_t)f->dl);
                if (d && d->state == WN_DL_RUNNING) { d->total = d->got; dl_end(d, WN_DL_DONE); }
            } else if (!f->post) {
                refetch_as_download(f->url, f->dl_name);
            }
            if (f->linger_until) {   // its moment is used: let it go
                f->linger_until = 0;
                fetch_unref(f);
            }
        } else if (f->replay_over || !f->keep_replay) {
            f->cancel = 1;
            if (!f->post) refetch_as_download(f->url, f->dl_name);
        }
    }
    pthread_mutex_unlock(&g_mu);
}

static void on_cert_allow(const struct ubulk_msg *m) {
    char *url = strndup(m->data, m->len);
    struct urlbits *b = url ? url_bits(url) : NULL;
    free(url);
    if (!b) return;
    pthread_mutex_lock(&g_mu);
    int have = 0;
    for (int i = 0; i < g_nallow; i++) have |= !strcasecmp(g_allow[i], b->host);
    if (!have && g_nallow < (int)(sizeof g_allow / sizeof g_allow[0])) g_allow[g_nallow++] = strdup(b->host);
    pthread_mutex_unlock(&g_mu);
    free(b);
}

static void on_cookies(const struct ubulk_msg *m, int clear) {
    char host[UHTTP_HOST_MAX];
    snprintf(host, sizeof host, "%.*s", (int)m->len, (const char *)m->data);
    pthread_mutex_lock(&g_mu);
    struct wn_cookies r = { 0, 0 };
    if (clear) r.cleared = ucookie_clear(&g_jar, host);
    r.count = ucookie_count(&g_jar, host, now_s());
    pthread_mutex_unlock(&g_mu);
    ubulk_server_send(&g_srv, m->pid, WN_COOKIES, m->id, &r, sizeof r, 1000);
}

static void on_dl_cancel(const struct ubulk_msg *m) {
    pthread_mutex_lock(&g_mu);
    struct download *d = dl_find(m->id);
    if (d && d->state == WN_DL_RUNNING) {
        if (d->fetch) d->fetch->cancel = 1;
        dl_end(d, WN_DL_CANCELLED);
    }
    pthread_mutex_unlock(&g_mu);
}

static void handle(const struct ubulk_msg *m) {
    switch (m->type) {
    case WN_FETCH:         on_fetch(m); break;
    case WN_BODY:          on_body(m); break;
    case WN_ABORT:         on_abort(m); break;
    case WN_DOWNLOAD:      on_download(m); break;
    case WN_CERT_ALLOW:    on_cert_allow(m); break;
    case WN_HELLO_APP:     if (m->pid == g_app_pid) g_app_client = m->pid; break;
    case WN_COOKIES_COUNT: on_cookies(m, 0); break;
    case WN_COOKIES_CLEAR: on_cookies(m, 1); break;
    case WN_DL_CANCEL:     on_dl_cancel(m); break;
    }
}

// A content process that went away takes its fetches with it -- except
// one that became a download.
static void client_gone(int pid) {
    pthread_mutex_lock(&g_mu);
    for (struct nfetch *f = g_fetches, *next; f; f = next) {
        next = f->next;
        if (f->pid != pid || f->freed) continue;
        f->freed = 1;
        if (!f->dl) f->cancel = 1;
        fetch_unref(f);
    }
    pthread_mutex_unlock(&g_mu);
}

static void save_jar(void) {
    pthread_mutex_lock(&g_mu);
    if (g_jar.dirty && ucookie_save(&g_jar, JAR_FILE, now_s()) != 0)
        printf("webnet: could not save %s\n", JAR_FILE);
    pthread_mutex_unlock(&g_mu);
}

static int f_weak;
static const char *f_app;

static const struct uargs_opt OPTS[] = {
    { "app", 0, "PID", "serve the Web Browser with this process id", 0, &f_app },
    { "weak-entropy", 0, 0, "key TLS even with only timer jitter for randomness", &f_weak, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "webnet",
    .usage = "--app PID [--weak-entropy]",
    .summary = "The Web Browser's network process: sockets, TLS, cookies and downloads.\n"
               "Started by the Web Browser; it ends when the browser does.",
    .opts = OPTS,
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    setvbuf(stdout, NULL, _IOLBF, 0);   // the application log is a pipe: a line at a time
    g_app_pid = f_app ? atoi(f_app) : 0;
    g_weak = f_weak != 0;
    if (g_app_pid <= 0) return uargs_error(&PROG, "--app PID is required (the Web Browser starts it)");
    char service[UCHAN_NAME_MAX];
    snprintf(service, sizeof service, WEBNET_SERVICE_FMT, g_app_pid);
    ucookie_jar_init(&g_jar);
    g_jar.is_public_suffix = is_public_suffix;
    mkdir("/var/lib", 0755);
    mkdir(STATE_DIR, 0755);
    if (exists(JAR_FILE)) {
        ucookie_load(&g_jar, JAR_FILE, now_s());
    } else {
        int n = ucookie_import_netsurf(&g_jar, NETSURF_JAR, now_s());
        if (n) printf("webnet: %d cookies taken from NetSurf's jar\n", n);
    }
    struct uhttp_pool_limits lim = { .per_host = 6, .total = 16, .idle_ms = 30000 };
    g_pool = uhttp_pool_new(&lim);
    if (!g_pool || ubulk_server_open(&g_srv, service) != 0) {
        fprintf(stderr, "webnet: cannot serve %s\n", service);
        return 1;
    }
    printf("webnet: serving %s\n", service);

    static char buf[UBULK_REC_MAX];
    unsigned long long last_save = sys_monotonic_ns(), started = last_save;
    for (;;) {
        ubulk_server_wait_change(&g_srv, 1000);
        int gone[UBULK_PEERS];
        int ng = ubulk_server_poll(&g_srv, gone, UBULK_PEERS);
        int app_gone = 0;
        for (int i = 0; i < ng && i < UBULK_PEERS; i++) {
            client_gone(gone[i]);
            app_gone |= gone[i] == g_app_pid;
        }
        struct ubulk_msg m;
        while (ubulk_server_recv(&g_srv, &m, buf, sizeof buf)) handle(&m);
        pthread_mutex_lock(&g_mu);
        linger_sweep();
        pthread_mutex_unlock(&g_mu);
        unsigned long long now = sys_monotonic_ns();
        if (now - last_save > SAVE_EVERY_MS * 1000000ull) {
            save_jar();
            last_save = now;
        }
        // The browser closed -- or never arrived to say hello.
        if (app_gone || (!g_app_client && now - started > 15000000000ull)) break;
    }
    save_jar();
    printf("webnet: the Web Browser is gone; exiting\n");
    return 0;
}
