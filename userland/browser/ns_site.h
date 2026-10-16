#ifndef BROWSER_NS_SITE_H
#define BROWSER_NS_SITE_H

// What the address field and the lock's connection card say about a
// tab's page: how it was reached, over which protocol and cipher, the
// certificate it was served with (all three the content process's to
// say, lib/webview_proto.h's wv_site), and the cookies its site keeps
// (the network process's: ns_net.h).
#include "utls.h"
#include "lib/webview_proto.h"

enum ns_site_state {
    NS_SITE_NONE,        // nothing loaded yet
    NS_SITE_INTERNAL,    // about:, data:, resource: -- the browser's own
    NS_SITE_FILE,        // file:
    NS_SITE_INSECURE,    // http:
    NS_SITE_OVERRIDE,    // https, but its certificate was refused and the person went on
    NS_SITE_ISSUES,      // https, with parts of the page fetched over http
    NS_SITE_SECURE,      // https
};

struct ns_site {
    int state;                     // enum ns_site_state
    char host[128];
    char version[24], cipher[64];  // empty when this run has no record of them
    int chain_depth;               // certificates presented, 0 for none
    int have_leaf;
    struct utls_cert_info leaf;    // the site's own certificate
    int cookies;                   // stored for this host and its parents
    int logins;                    // HTTP sign-ins webnet keeps for it
};

// The content process's account of the page, in the card's terms.
void ns_site_from(struct ns_site *out, const struct wv_site *in, int cookies, int logins);

#endif
