#ifndef BROWSER_NS_SITE_H
#define BROWSER_NS_SITE_H

// What the address field and the lock's connection card say about a
// tab's page: how it was reached, over which protocol and cipher, the
// certificate it was served with, and the cookies its site keeps.
#include "utls.h"

struct gui_window;

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
};

int ns_site_state(struct gui_window *gw);
void ns_site_get(struct gui_window *gw, struct ns_site *out);
// Deletes every cookie the page's host would be sent. Returns how many.
int ns_site_clear_cookies(struct gui_window *gw);

#endif
