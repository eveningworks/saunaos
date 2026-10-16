// The app: scheme -- a page the EMBEDDING APP makes, as WebKit's
// WKURLSchemeHandler and Qt's QWebEngineUrlSchemeHandler serve one. Help
// answers app:help/<page> with HTML it made from its Markdown. The
// request goes to the app as WV_SCHEME_GET; its answer comes back in
// pieces (WV_SCHEME_DATA) and is handed to NetSurf whole, from poll, the
// way content/fetchers/data.c hands over a data: URL.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "webcontent/wc.h"

#include "content/fetch.h"
#include "content/fetchers.h"
#include "utils/nsurl.h"

#define BODY_MAX (8 * 1024 * 1024)

struct sfetch {
    struct sfetch *next;
    struct fetch *parent;
    uint32_t id;
    char *url;
    int sent, done, aborted, locked;
    int status;
    char mime[96];
    char *body;
    size_t len, cap;
};

static struct sfetch *g_list;
static uint32_t g_next_id = 1;

static void *sc_setup(struct fetch *parent, nsurl *url, bool only_2xx, bool downgrade_tls,
                      const char *post_urlenc, const struct fetch_multipart_data *post_multipart,
                      const char **headers) {
    (void)only_2xx, (void)downgrade_tls, (void)post_urlenc, (void)post_multipart, (void)headers;
    struct sfetch *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->parent = parent;
    f->url = strdup(nsurl_access(url));
    f->id = g_next_id++;
    f->status = 200;
    if (!f->url) { free(f); return NULL; }
    f->next = g_list;
    g_list = f;
    return f;
}

static bool sc_start(void *vf) {
    struct sfetch *f = vf;
    f->sent = 1;
    wc_send_id(WV_SCHEME_GET, f->id, f->url, (uint32_t)strlen(f->url));
    return true;
}

static void unlink_fetch(struct sfetch *f) {
    for (struct sfetch **p = &g_list; *p; p = &(*p)->next)
        if (*p == f) { *p = f->next; break; }
}

static void sc_free(void *vf) {
    struct sfetch *f = vf;
    unlink_fetch(f);
    free(f->url);
    free(f->body);
    free(f);
}

static void sc_abort(void *vf) {
    struct sfetch *f = vf;
    f->aborted = 1;
}

static void send_msg(struct sfetch *f, fetch_msg *msg) {
    f->locked = 1;
    fetch_send_callback(msg, f->parent);
    f->locked = 0;
}

static void send_header(struct sfetch *f, const char *h) {
    fetch_msg msg = { .type = FETCH_HEADER };
    msg.data.header_or_data.buf = (const uint8_t *)h;
    msg.data.header_or_data.len = strlen(h);
    send_msg(f, &msg);
}

static void sc_poll(lwc_string *scheme) {
    (void)scheme;
    for (struct sfetch *f = g_list, *next; f; f = next) {
        next = f->next;
        if (f->locked || (!f->aborted && !f->done)) continue;
        if (!f->aborted) {
            fetch_msg msg;
            if (f->status != 200) {
                msg.type = FETCH_ERROR;
                msg.data.error = f->status == 403
                    ? "A web page cannot open files on this computer."
                    : "There is no such page.";
                send_msg(f, &msg);
            } else {
                fetch_set_http_code(f->parent, 200);
                char h[128];
                snprintf(h, sizeof h, "Content-Type: %s", f->mime[0] ? f->mime : "text/html");
                send_header(f, h);
                if (!f->aborted) {
                    msg.type = FETCH_DATA;
                    msg.data.header_or_data.buf = (const uint8_t *)(f->body ? f->body : "");
                    msg.data.header_or_data.len = f->len;
                    send_msg(f, &msg);
                }
                if (!f->aborted) {
                    msg.type = FETCH_FINISHED;
                    send_msg(f, &msg);
                }
            }
        }
        fetch_remove_from_queues(f->parent);
        fetch_free(f->parent);
    }
}

void wc_scheme_data(const struct ubulk_msg *m) {
    struct sfetch *f = g_list;
    while (f && f->id != m->id) f = f->next;
    struct wv_scheme h;
    if (!f || f->done || m->len < sizeof h) return;
    memcpy(&h, m->data, sizeof h);
    const char *p = (const char *)m->data + sizeof h;
    size_t n = m->len - sizeof h;
    if (h.mime_len > 0 && (size_t)h.mime_len <= n) {
        snprintf(f->mime, sizeof f->mime, "%.*s", h.mime_len, p);
        p += h.mime_len;
        n -= (size_t)h.mime_len;
    }
    f->status = h.status;
    if (n && f->len + n <= BODY_MAX) {
        if (f->len + n > f->cap) {
            size_t cap = f->cap ? f->cap : 16384;
            while (cap < f->len + n) cap *= 2;
            char *g = realloc(f->body, cap);
            if (!g) { f->status = 500; f->done = 1; return; }
            f->body = g;
            f->cap = cap;
        }
        memcpy(f->body + f->len, p, n);
        f->len += n;
    }
    if (h.done) f->done = 1;
}

static bool sc_initialise(lwc_string *scheme) { (void)scheme; return true; }
static void sc_finalise(lwc_string *scheme) { (void)scheme; }
static bool sc_acceptable(const nsurl *url) { (void)url; return true; }

static const struct fetcher_operation_table g_ops = {
    .initialise = sc_initialise,
    .acceptable = sc_acceptable,
    .setup = sc_setup,
    .start = sc_start,
    .abort = sc_abort,
    .free = sc_free,
    .poll = sc_poll,
    .finalise = sc_finalise,
};

const struct fetcher_operation_table *wc_scheme_ops(void) { return &g_ops; }

int wc_scheme_register(void) {
    lwc_string *s;
    if (lwc_intern_string("app", 3, &s) != lwc_error_ok) return -1;
    return fetcher_add(s, &g_ops) == NSERROR_OK ? 0 : -1;
}
