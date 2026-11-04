#ifndef BROWSER_NS_NET_H
#define BROWSER_NS_NET_H

// The Web Browser's network process, /bin/webnet (docs/browser-design.md,
// stage 5): started here, one per browser, and asked about what it owns
// -- the cookies a site keeps, and the downloads (ns_download.h's list is
// filled from what it reports).
#include <stdint.h>

struct uapp;

// Starts /bin/webnet for this browser and joins it. `post_id` is posted
// (as a0) when it has something to say; hand those posts to
// ns_net_on_post(). 1, or 0 when it did not come up.
int ns_net_start(struct uapp *a, int post_id, int weak_entropy);
// Its channel, for each content process's --net. "" before it started.
const char *ns_net_service(void);
// Reads what webnet sent. 1 when something the window shows changed.
int ns_net_on_post(void);

// How many cookies `host` would be sent, and deleting them all, asked of
// webnet and waited for briefly (the card is opening). -1 when it did
// not answer.
// `*logins`, when not NULL, is how many HTTP logins webnet keeps for it.
int ns_net_cookies(const char *host, int *logins);
int ns_net_cookies_clear(const char *host);

// HTTP logins (lib/webnet_proto.h, WN_AUTH_ASK). webnet asks; the app
// shows a sign-in dialog over the tab whose content process is `pid`.
// The app implements these two, called from ns_net_on_post():
void nsapp_login_ask(uint32_t id, int pid, int retry, int secure, const char *host, const char *realm);
void nsapp_login_gone(uint32_t id);   // nobody waits for ask `id` any more
// The answer to ask `id`: a user name and password, or NULL for Cancel.
void ns_net_login_answer(uint32_t id, const char *user, const char *pass);

#endif
