// file: URLs, read by the APP for a sandboxed engine. This process sees
// only WC_OWN_FILES (its stylesheets and start page) through its unveil
// list; every other local file is asked for as WV_SCHEME_GET, and
// ui/uui_webview.c decides -- Chrome's renderer has no file access
// either, and its browser process grants it per navigation.
//
// NETSURF'S FILE FETCHER IS KEPT, NOT REPLACED: `--wrap=fetcher_add`
// (Makefile) catches its registration and puts a dispatcher in its slot,
// which hands a URL under WC_OWN_FILES to NetSurf's own fetcher and any
// other to the app's (wc_scheme.c). The vendored source is untouched.
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "webcontent/wc.h"

#include "content/fetch.h"
#include "content/fetchers.h"
#include "utils/corestrings.h"
#include "utils/nsurl.h"

int g_wc_files;

nserror __real_fetcher_add(lwc_string *scheme, const struct fetcher_operation_table *ops);

static struct fetcher_operation_table g_native;

// Each fetch remembers whose it is, so start, abort and free reach the
// fetcher that set it up.
struct handle {
    int native;
    void *inner;
};

static int own_file(const nsurl *url) {
    lwc_string *path = nsurl_get_component(url, NSURL_PATH);
    if (!path) return 0;
    const char *p = lwc_string_data(path);
    size_t n = sizeof WC_OWN_FILES - 1;
    int own = !strncmp(p, WC_OWN_FILES, n) && (p[n] == '/' || !p[n]) && !strstr(p, "/..") &&
              !nsurl_has_component(url, NSURL_HOST);
    lwc_string_unref(path);
    return own;
}

static bool d_initialise(lwc_string *scheme) {
    return g_native.initialise(scheme) && wc_scheme_ops()->initialise(scheme);
}

static bool d_acceptable(const nsurl *url) { return g_native.acceptable(url); }

static void *d_setup(struct fetch *parent, nsurl *url, bool only_2xx, bool downgrade_tls,
                     const char *post_urlenc, const struct fetch_multipart_data *post_multipart,
                     const char **headers) {
    struct handle *h = malloc(sizeof *h);
    if (!h) return NULL;
    h->native = own_file(url);
    const struct fetcher_operation_table *ops = h->native ? &g_native : wc_scheme_ops();
    h->inner = ops->setup(parent, url, only_2xx, downgrade_tls, post_urlenc, post_multipart,
                          headers);
    if (!h->inner) { free(h); return NULL; }
    return h;
}

static const struct fetcher_operation_table *ops_of(const struct handle *h) {
    return h->native ? &g_native : wc_scheme_ops();
}

static bool d_start(void *v) { struct handle *h = v; return ops_of(h)->start(h->inner); }
static void d_abort(void *v) { struct handle *h = v; ops_of(h)->abort(h->inner); }

static void d_free(void *v) {
    struct handle *h = v;
    ops_of(h)->free(h->inner);
    free(h);
}

static void d_poll(lwc_string *scheme) {
    g_native.poll(scheme);
    wc_scheme_ops()->poll(scheme);
}

static int d_fdset(lwc_string *scheme, fd_set *r, fd_set *w, fd_set *e) {
    return g_native.fdset ? g_native.fdset(scheme, r, w, e) : -1;
}

static void d_finalise(lwc_string *scheme) {
    g_native.finalise(scheme);
    wc_scheme_ops()->finalise(scheme);
}

nserror __wrap_fetcher_add(lwc_string *scheme, const struct fetcher_operation_table *ops) {
    bool is_file = false;
    if (lwc_string_caseless_isequal(scheme, corestring_lwc_file, &is_file) != lwc_error_ok ||
        !is_file)
        return __real_fetcher_add(scheme, ops);
    static const struct fetcher_operation_table dispatch = {
        .initialise = d_initialise,
        .acceptable = d_acceptable,
        .setup = d_setup,
        .start = d_start,
        .abort = d_abort,
        .free = d_free,
        .poll = d_poll,
        .fdset = d_fdset,
        .finalise = d_finalise,
    };
    g_native = *ops;
    return __real_fetcher_add(scheme, &dispatch);
}
