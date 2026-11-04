#ifndef ULIB_UCOOKIE_H
#define ULIB_UCOOKIE_H

#include <stddef.h>
#include <stdint.h>

// ucookie -- an HTTP cookie jar, RFC 6265: what /bin/webnet keeps for
// the Web Browser (docs/browser-design.md, stage 5). Set-Cookie parsed
// as section 5.2 says (Expires in the 5.1.1 date grammar, Max-Age
// winning over it, Domain and Path defaulted), the Cookie header built
// as 5.4 says (longer paths first), and a public suffix refused as a
// Domain when the caller can say what one is.
//
// SameSite is accepted and not enforced: the jar does not know which
// site a request came FROM -- the network process would have to be told
// the top-level page of every subresource fetch.
//
// Kept in one file, a line per cookie and tab-separated, so `less`
// reads it. Session cookies are never written.

#define UCOOKIE_MAX_BYTES   4096   // a name and value together (RFC 6265 6.1)
#define UCOOKIE_PER_DOMAIN  50
#define UCOOKIE_TOTAL       3000

struct ucookie {
    char *name, *value;
    char *domain;          // lower case, no leading dot
    char *path;
    int64_t expires;       // a UTC epoch, -1 for a session cookie
    int64_t created, last_used;
    int host_only, secure, http_only;
};

struct ucookie_jar {
    struct ucookie *c;
    int n, cap;
    int dirty;             // changed since the last save
    // 1 when `domain` is a public suffix ("com", "co.uk"), which a
    // Domain attribute may not name. NULL: only a dotless one is refused.
    int (*is_public_suffix)(const char *domain);
};

void ucookie_jar_init(struct ucookie_jar *j);
void ucookie_jar_free(struct ucookie_jar *j);

// Stores what one Set-Cookie header (its value, after the colon) says,
// for a response from `host`/`path`, over https when `secure`. Returns 1
// when stored or deleted, 0 when refused.
int ucookie_set(struct ucookie_jar *j, const char *host, const char *path, int secure,
                const char *set_cookie, int64_t now);

// The Cookie header's value for a request to `host`/`path` -- "a=1; b=2"
// -- into `out`. Returns its length; 0 when no cookie applies, or when
// they do not fit (nothing is written then).
size_t ucookie_header(struct ucookie_jar *j, const char *host, const char *path, int secure,
                      int64_t now, char *out, size_t cap);

// How many cookies `host` would be sent on some path, and deleting them
// all -- the lock card's count and its Clear. Delete returns how many.
int ucookie_count(const struct ucookie_jar *j, const char *host, int64_t now);
int ucookie_clear(struct ucookie_jar *j, const char *host);

// The jar's file. Load adds to what the jar holds; both return 0, or -1
// (a missing file loads as empty and returns 0).
int ucookie_load(struct ucookie_jar *j, const char *path, int64_t now);
int ucookie_save(struct ucookie_jar *j, const char *path, int64_t now);

// NetSurf's own cookie file (its urldb's, format 100-102), read once
// when the Web Browser stopped linking NetSurf. Returns how many were taken.
int ucookie_import_netsurf(struct ucookie_jar *j, const char *path, int64_t now);

// An RFC 6265 5.1.1 cookie date as a UTC epoch; -1 when it is not one.
int64_t ucookie_parse_date(const char *s);

#endif
