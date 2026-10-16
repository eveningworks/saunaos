// The content process's network: backends/netsurf/fetch_http.c in remote
// mode, over /bin/webnet's records (lib/webnet_proto.h). NetSurf's fetch
// logic is unchanged above it; the sockets, TLS and cookies are webnet's.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "webcontent/wc.h"

#include "utils/nsurl.h"
#include "backends/netsurf/fetch_http.h"
#include "lib/webnet_proto.h"
#include "utls.h"

#define SEND_WAIT_MS 5000

// The upgraded navigation's URL, and its fetch once started (wc_net_upgrade()).
static char g_upgrade[WC_URL_MAX];
static uint32_t g_upgrade_rid;
static int g_upgrade_answered;   // a header came: https was reached

void wc_net_upgrade(const char *url) {
    snprintf(g_upgrade, sizeof g_upgrade, "%s", url ? url : "");
    g_upgrade_rid = 0;
    g_upgrade_answered = 0;
}

static int remote_start(uint32_t id, const char *url, int post, const char *const *headers,
                        const void *body, size_t body_len) {
    if (!g_wc_has_net) return -1;
    static char rec[UBULK_REC_MAX];
    struct wn_fetch h = { post ? WN_METHOD_POST : WN_METHOD_GET, (int32_t)strlen(url), 0,
                          (int32_t)body_len, 0 };
    if (g_upgrade[0] && !g_upgrade_rid && !strcmp(url, g_upgrade)) {
        h.flags = WN_FETCH_UPGRADED;
        g_upgrade_rid = id;
    }
    size_t n = sizeof h;
    if (n + (size_t)h.url_len > sizeof rec) return -1;
    memcpy(rec + n, url, (size_t)h.url_len);
    n += (size_t)h.url_len;
    for (int i = 0; headers && headers[i]; i++) {
        size_t l = strlen(headers[i]) + 1;
        if (n + l > sizeof rec) return -1;
        memcpy(rec + n, headers[i], l);
        n += l;
        h.headers_len += (int32_t)l;
    }
    size_t first = sizeof rec - n;
    if (first > body_len) first = body_len;
    if (first) memcpy(rec + n, body, first);
    n += first;
    memcpy(rec, &h, sizeof h);
    if (ubulk_send(&g_wc_net, WN_FETCH, id, rec, (uint32_t)n, SEND_WAIT_MS) != 0) return -1;
    for (size_t off = first; off < body_len;) {
        size_t k = body_len - off > UBULK_REC_MAX ? UBULK_REC_MAX : body_len - off;
        if (ubulk_send(&g_wc_net, WN_BODY, id, (const char *)body + off, (uint32_t)k, SEND_WAIT_MS) != 0)
            return -1;
        off += k;
    }
    return 0;
}

static void remote_abort(uint32_t id) {
    if (g_wc_has_net) ubulk_send(&g_wc_net, WN_ABORT, id, NULL, 0, SEND_WAIT_MS);
}

static void deliver_certs(const struct ubulk_msg *m) {
    struct wn_certs h;
    if (m->len < sizeof h) return;
    memcpy(&h, m->data, sizeof h);
    struct utls_chain c;
    memset(&c, 0, sizeof c);
    const unsigned char *p = (const unsigned char *)m->data + sizeof h;
    size_t left = m->len - sizeof h;
    for (int i = 0; i < h.depth && i < WN_CERTS_MAX && i < UTLS_CHAIN_MAX; i++) {
        if (h.len[i] < 0 || (size_t)h.len[i] > left) break;
        c.certs[i].der = (unsigned char *)p;   // lent: fetch_http copies it
        c.certs[i].der_len = (size_t)h.len[i];
        c.certs[i].err = (enum utls_cert_err)h.err[i];
        c.depth = i + 1;
        p += h.len[i];
        left -= (size_t)h.len[i];
    }
    ns_fetch_http_remote_certs(m->id, &c);
}

void wc_net_pump(void) {
    if (!g_wc_has_net) return;
    static char buf[UBULK_REC_MAX + 1];
    struct ubulk_msg m;
    while (ubulk_client_recv(&g_wc_net, &m, buf, UBULK_REC_MAX)) {
        switch (m.type) {
        case WN_HEADER:
            if (m.id == g_upgrade_rid) g_upgrade_answered = 1;
            ns_fetch_http_remote_header(m.id, m.data, m.len);
            break;
        case WN_DATA:
            ns_fetch_http_remote_data(m.id, m.data, m.len);
            break;
        case WN_TLS: {
            buf[m.len] = '\0';
            const char *version = buf;
            size_t vl = strlen(version);
            ns_fetch_http_remote_tls(m.id, version, vl < m.len ? version + vl + 1 : "");
            break;
        }
        case WN_CERTS:
            deliver_certs(&m);
            break;
        case WN_DONE: {
            struct wn_done d;
            if (m.len < sizeof d) break;
            memcpy(&d, m.data, sizeof d);
            buf[m.len] = '\0';
            if (g_upgrade_rid && m.id == g_upgrade_rid) {
                int fall = d.rc != 0 && !g_upgrade_answered;
                static char url[WC_URL_MAX];   // off the ring-3 frame
                snprintf(url, sizeof url, "%s", g_upgrade);
                wc_net_upgrade(NULL);
                // Not handed to NetSurf: no error page, the http twin
                // replaces the load (which aborts this fetch).
                if (fall) {
                    wc_window_downgrade(url);
                    break;
                }
            }
            ns_fetch_http_remote_done(m.id, d.rc, d.cert_err, buf + sizeof d);
            break;
        }
        }
    }
}

void wc_net_download(uint32_t id, const char *name) {
    if (g_wc_has_net && id) ubulk_send(&g_wc_net, WN_DOWNLOAD, id, name, (uint32_t)strlen(name), SEND_WAIT_MS);
}

static const struct ns_fetch_remote g_remote = {
    .start = remote_start,
    .abort = remote_abort,
    .pump = wc_net_pump,
};

void wc_net_init(void) { ns_fetch_http_set_remote(&g_remote); }

// "Proceed anyway" on NetSurf's certificate page: the exception is
// webnet's to keep, since webnet makes the connection. NetSurf's own
// record stays too -- its page-info state reads it. A link-time wrap
// (the Makefile's --wrap), not an edit to the vendored urldb.
void __real_urldb_set_cert_permissions(struct nsurl *url, bool permit);
void __wrap_urldb_set_cert_permissions(struct nsurl *url, bool permit) {
    __real_urldb_set_cert_permissions(url, permit);
    if (permit && g_wc_has_net) {
        const char *u = nsurl_access(url);
        ubulk_send(&g_wc_net, WN_CERT_ALLOW, 0, u, (uint32_t)strlen(u), SEND_WAIT_MS);
    }
}
