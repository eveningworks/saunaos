// What the page is, for the app's address field and connection card:
// how it was reached, over which protocol and cipher, and the
// certificate it was served with. The cookies its site keeps are
// /bin/webnet's to count, and the app asks there.
#include <stdio.h>
#include <string.h>

#include "utils/errors.h"
#include "webcontent/wc.h"

#include "netsurf/browser_window.h"
#include "netsurf/ssl_certs.h"
#include "utils/nsurl.h"
#include "backends/netsurf/fetch_http.h"
#include "utls.h"

static int state_of(struct gui_window *gw) {
    if (!gw || !gw->bw) return WV_SITE_NONE;
    switch (browser_window_get_page_info_state(gw->bw)) {
    case PAGE_STATE_INTERNAL:        return WV_SITE_INTERNAL;
    case PAGE_STATE_LOCAL:           return WV_SITE_FILE;
    case PAGE_STATE_INSECURE:        return WV_SITE_INSECURE;
    case PAGE_STATE_SECURE_OVERRIDE: return WV_SITE_OVERRIDE;
    case PAGE_STATE_SECURE_ISSUES:   return WV_SITE_ISSUES;
    case PAGE_STATE_SECURE:          return WV_SITE_SECURE;
    default:                         return WV_SITE_NONE;
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

// Described once per certificate: decoding one is not free, and this is
// asked after every record the app sends. Keyed by a hash of the DER, not
// the chain's address, which a freed and remade chain can share.
static uint32_t g_described;
static struct wv_cert g_leaf;
static int g_have_leaf;

void wc_site_get(struct gui_window *gw, struct wv_site *out) {
    memset(out, 0, sizeof *out);
    out->state = state_of(gw);
    if (!gw || !gw->bw) return;
    out->js = gw->js;
    out->scripts = gw->scripts;
    host_of(gw, out->host, sizeof out->host);
    ns_fetch_http_tls_info(out->host, out->version, sizeof out->version, out->cipher,
                           sizeof out->cipher);
    struct cert_chain *chain = NULL;
    if (browser_window_get_ssl_chain(gw->bw, &chain) != NSERROR_OK || !chain || !chain->depth) return;
    out->chain_depth = (int)chain->depth;
    uint32_t key = 2166136261u;   // FNV-1a
    for (size_t i = 0; i < chain->certs[0].der_length; i++)
        key = (key ^ chain->certs[0].der[i]) * 16777619u;
    if (key != g_described) {
        g_described = key;
        struct utls_cert_info info;
        g_have_leaf = utls_cert_describe(chain->certs[0].der, chain->certs[0].der_length, &info) == 0;
        memset(&g_leaf, 0, sizeof g_leaf);
        if (g_have_leaf) {
            snprintf(g_leaf.subject_cn, sizeof g_leaf.subject_cn, "%s", info.subject_cn);
            snprintf(g_leaf.subject_org, sizeof g_leaf.subject_org, "%s", info.subject_org);
            snprintf(g_leaf.issuer_cn, sizeof g_leaf.issuer_cn, "%s", info.issuer_cn);
            snprintf(g_leaf.issuer_org, sizeof g_leaf.issuer_org, "%s", info.issuer_org);
            g_leaf.nb_year = info.not_before.year, g_leaf.nb_mon = info.not_before.mon;
            g_leaf.nb_day = info.not_before.day;
            g_leaf.na_year = info.not_after.year, g_leaf.na_mon = info.not_after.mon;
            g_leaf.na_day = info.not_after.day;
        }
    }
    out->have_leaf = g_have_leaf;
    out->leaf = g_leaf;
}
