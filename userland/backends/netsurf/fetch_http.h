#ifndef NS_FETCH_HTTP_H
#define NS_FETCH_HTTP_H

// NetSurf's http: and https: fetcher, over /lib/libhttp.so (<uhttp.h>).
// docs/browser-design.md's stage 2.
//
// EACH FETCH IS A THREAD, because a toy-os socket only blocks -- there is
// no select() for NetSurf's fdset slot (left NULL) to fill. The thread
// queues what arrives; NetSurf's poll, on the main thread, turns the
// queue into fetch_send_callback() messages. Nothing NetSurf owns is
// touched from a fetch thread.

// Registers both schemes. After netsurf_init(). 0, or -1.
int ns_fetch_http_register(void);

// The User-Agent sent with every request; NULL = NetSurf's own string.
// Copied.
void ns_fetch_http_set_user_agent(const char *ua);

// Called FROM A FETCH THREAD when it has queued something, so a main loop
// asleep until its next timer can wake for it (the Web Browser posts with
// uapp_post(), the one Toykit call a worker may make). May be NULL.
void ns_fetch_http_set_wake(void (*wake)(void));

// Key TLS even when this machine's randomness is only TSC jitter -- what
// `wget --weak-entropy` does, for a guest with no virtio-rng under test.
void ns_fetch_http_allow_weak_entropy(int on);

#endif
