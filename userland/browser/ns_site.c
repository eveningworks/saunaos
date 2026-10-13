// What a tab's page is, for the address field and the connection card.
// See ns_site.h.
#include <stdio.h>
#include <string.h>

#include "utils/errors.h"
#include "browser/ns_site.h"
#include "browser/ns_view.h"

#include "netsurf/browser_window.h"
#include "netsurf/cookie_db.h"
#include "netsurf/ssl_certs.h"
#include "utils/nsurl.h"
#include "backends/netsurf/fetch_http.h"

int ns_site_state(struct gui_window *gw) {
    if (!gw || !gw->bw) return NS_SITE_NONE;
    switch (browser_window_get_page_info_state(gw->bw)) {
    case PAGE_STATE_INTERNAL:        return NS_SITE_INTERNAL;
    case PAGE_STATE_LOCAL:           return NS_SITE_FILE;
    case PAGE_STATE_INSECURE:        return NS_SITE_INSECURE;
    case PAGE_STATE_SECURE_OVERRIDE: return NS_SITE_OVERRIDE;
    case PAGE_STATE_SECURE_ISSUES:   return NS_SITE_ISSUES;
    case PAGE_STATE_SECURE:          return NS_SITE_SECURE;
    default:                         return NS_SITE_NONE;
    }
}

static void host_of(struct gui_window *gw, char *out, size_t cap) {
    out[0] = '\0';
    struct nsurl *u = NULL;
    if (!gw->url[0] || nsurl_create(gw->url, &u) != NSERROR_OK) return;
    lwc_string *h = nsurl_get_component(u, NSURL_HOST);
    if (h) {
        snprintf(out, cap, "%.*s", (int)lwc_string_length(h), lwc_string_data(h));
        lwc_string_unref(h);
    }
    nsurl_unref(u);
}

// Would a cookie for `domain` be sent to `host`? RFC 6265's domain
// match: the host itself, or a host under it ("example.com" covers
// "www.example.com"); a stored leading dot is the older spelling.
static int domain_match(const char *host, const char *domain) {
    if (*domain == '.') domain++;
    size_t h = strlen(host), d = strlen(domain);
    if (!d || d > h) return 0;
    if (strcasecmp(host + h - d, domain)) return 0;
    return h == d || host[h - d - 1] == '.';
}

// urldb_iterate_cookies() takes no context pointer, so the walk's state
// is file-scope; only the main thread walks.
static const char *g_walk_host;
static int g_walk_count;
#define CLEAR_MAX 64
static struct { char domain[128], path[256], name[128]; } g_doomed[CLEAR_MAX];
static int g_ndoomed, g_collect;

static bool walk(const struct cookie_data *c) {
    if (!c->domain || !domain_match(g_walk_host, c->domain)) return true;
    g_walk_count++;
    if (g_collect && g_ndoomed < CLEAR_MAX) {
        snprintf(g_doomed[g_ndoomed].domain, sizeof g_doomed[0].domain, "%s", c->domain);
        snprintf(g_doomed[g_ndoomed].path, sizeof g_doomed[0].path, "%s", c->path ? c->path : "/");
        snprintf(g_doomed[g_ndoomed].name, sizeof g_doomed[0].name, "%s", c->name ? c->name : "");
        g_ndoomed++;
    }
    return true;
}

static int count_cookies(const char *host, int collect) {
    if (!host[0]) return 0;
    g_walk_host = host;
    g_walk_count = 0;
    g_ndoomed = 0;
    g_collect = collect;
    urldb_iterate_cookies(walk);
    return g_walk_count;
}

void ns_site_get(struct gui_window *gw, struct ns_site *out) {
    memset(out, 0, sizeof *out);
    out->state = ns_site_state(gw);
    if (!gw || !gw->bw) return;
    host_of(gw, out->host, sizeof out->host);
    ns_fetch_http_tls_info(out->host, out->version, sizeof out->version, out->cipher,
                           sizeof out->cipher);
    struct cert_chain *chain = NULL;
    if (browser_window_get_ssl_chain(gw->bw, &chain) == NSERROR_OK && chain && chain->depth) {
        out->chain_depth = (int)chain->depth;
        out->have_leaf = utls_cert_describe(chain->certs[0].der, chain->certs[0].der_length,
                                            &out->leaf) == 0;
    }
    out->cookies = count_cookies(out->host, 0);
}

int ns_site_clear_cookies(struct gui_window *gw) {
    char host[128];
    if (!gw) return 0;
    host_of(gw, host, sizeof host);
    // Collected first and deleted after: deleting inside the walk would
    // unlink the list it is walking.
    count_cookies(host, 1);
    for (int i = 0; i < g_ndoomed; i++)
        urldb_delete_cookie(g_doomed[i].domain, g_doomed[i].path, g_doomed[i].name);
    return g_ndoomed;
}
