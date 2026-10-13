#ifndef NS_FETCH_HTTP_H
#define NS_FETCH_HTTP_H

#include <stddef.h>
#include <stdint.h>

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

// The protocol and cipher of `host`'s last https connection
// ("TLSv1.3", "TLS1-3-AES-256-GCM-SHA384" -- mbedTLS's names), for the
// connection card. 1, or 0 when this run has not reached it over https.
int ns_fetch_http_tls_info(const char *host, char *version, size_t vcap, char *cipher, size_t ccap);

// --- remote mode: the transfer in another process (/bin/webnet) -------------
//
// Set before the first fetch. `start` sends a request numbered `id` (the
// headers NULL-terminated, the body `body_len` bytes); `abort` lets fetch
// `id` go, finished or not; `pump` is called at the top of every poll to
// deliver, through the calls below, whatever has arrived. All three run
// on the main thread.
struct utls_chain;
struct ns_fetch_remote {
    int (*start)(uint32_t id, const char *url, int post, const char *const *headers,
                 const void *body, size_t body_len);
    void (*abort)(uint32_t id);
    void (*pump)(void);
};
void ns_fetch_http_set_remote(const struct ns_fetch_remote *ops);

// What arrived for fetch `id`, in order. A header line comes WITH its
// CRLF. Unknown ids are dropped: a fetch let go may still have bytes in
// flight.
void ns_fetch_http_remote_header(uint32_t id, const char *line, size_t len);
void ns_fetch_http_remote_data(uint32_t id, const void *data, size_t len);
void ns_fetch_http_remote_tls(uint32_t id, const char *version, const char *cipher);
void ns_fetch_http_remote_certs(uint32_t id, const struct utls_chain *chain);
void ns_fetch_http_remote_done(uint32_t id, int rc, int cert_err, const char *err);
// Ends every fetch still running with `err` -- the network process died.
void ns_fetch_http_remote_fail_all(const char *err);
// The newest fetch of `url` that has had its headers -- active, or among
// the last few let go: the one a download NetSurf just decided on read.
// 0 when none.
uint32_t ns_fetch_http_remote_id_for(const char *url);
#endif
