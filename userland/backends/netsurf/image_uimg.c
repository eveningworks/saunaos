// PNG, JPEG and QOI for NetSurf, through toy-os's own lib/uimg.h.
//
// Upstream's png.c and jpeg.c sit on libpng and libjpeg, which are not
// vendored: lib/uimg.h already decodes all three and is checked against
// Pillow (tools/uimg_codec_hostcheck.py). GIF and BMP stay with NetSurf's
// libnsgif and libnsbmp, which animate.
//
// THE SHAPE IS UPSTREAM jpeg.c's -- the header read when the bytes are
// complete, the pixels decoded only when the image is first DRAWN, into
// image_cache, which may evict them and ask again -- with one difference:
// a large image is decoded in a THREAD, so a 20-megapixel photo does not
// stop the page scrolling. Until it is ready nothing is drawn in its box,
// and a timer on the main thread asks for the redraw that picks it up.
// The worker decodes a private COPY of the bytes: the content's own may
// be freed under it.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/bitmap.h"
#include "netsurf/misc.h"
#include "content/llcache.h"
#include "content/content.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"
#include "image/image_cache.h"

#include "lib/uimg.h"
#include "image_uimg.h"

// Decoded size at or under which the decode simply happens where it is
// asked for: an icon costs less than the thread would.
#define SYNC_BYTES (64 * 1024)
#define POLL_MS 15
#define DECODE_STACK (128 * 1024)

struct job {
    int refs;              // the content's and the worker's, under g_mu
    int done;
    uint8_t *src;
    size_t n;
    struct uimg im;        // the result, when rc == 0
    int rc;
};

struct uimg_content {
    struct content base;   // FIRST: the content machinery casts between the two
    struct job *job;       // in flight or finished and not yet taken
    bool polling;
    bool failed;           // the pixels would not decode: never asked again
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void job_unref(struct job *j) {
    pthread_mutex_lock(&g_mu);
    int last = --j->refs == 0;
    pthread_mutex_unlock(&g_mu);
    if (last) {
        free(j->src);
        uimg_free(&j->im);
        free(j);
    }
}

static void *worker(void *arg) {
    struct job *j = arg;
    struct uimg im = { 0 };
    int rc = uimg_decode(j->src, j->n, &im);
    pthread_mutex_lock(&g_mu);
    j->im = im;
    j->rc = rc;
    j->done = 1;
    pthread_mutex_unlock(&g_mu);
    job_unref(j);
    return NULL;
}

static int job_done(struct job *j) {
    pthread_mutex_lock(&g_mu);
    int d = j->done;
    pthread_mutex_unlock(&g_mu);
    return d;
}

static void poll_job(void *p);

// Starts the decode of `u`'s bytes in a thread. False if it could not
// (no memory, no thread), and then the caller decodes in place.
static bool start_job(struct uimg_content *u) {
    size_t n;
    const uint8_t *data = content__get_source_data(&u->base, &n);
    struct job *j = calloc(1, sizeof *j);
    if (!j || !data) { free(j); return false; }
    j->src = malloc(n);
    if (!j->src) { free(j); return false; }
    memcpy(j->src, data, n);
    j->n = n;
    j->refs = 2;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, DECODE_STACK);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int ok = pthread_create(&t, &at, worker, j) == 0;
    pthread_attr_destroy(&at);
    if (!ok) { free(j->src); free(j); return false; }
    u->job = j;
    u->polling = true;
    guit->misc->schedule(POLL_MS, poll_job, u);
    return true;
}

// The main thread's watch on a decode: when it has finished, the content
// asks to be drawn again, and that draw takes the pixels.
static void poll_job(void *p) {
    struct uimg_content *u = p;
    if (!u->job || !job_done(u->job)) {
        guit->misc->schedule(POLL_MS, poll_job, u);
        return;
    }
    u->polling = false;
    union content_msg_data data;
    data.redraw.x = 0;
    data.redraw.y = 0;
    data.redraw.width = u->base.width;
    data.redraw.height = u->base.height;
    content_broadcast(&u->base, CONTENT_MSG_REDRAW, &data);
}

// Pixels into a NetSurf bitmap: rows copied as they are, since the client
// format is declared to be uimg's own (0xAARRGGBB, straight alpha), and
// converted if a front end declared another.
static struct bitmap *to_bitmap(const struct uimg *im) {
    struct bitmap *b = guit->bitmap->create(im->w, im->h,
                                            im->has_alpha ? BITMAP_NONE : BITMAP_OPAQUE);
    if (!b) return NULL;
    uint8_t *dst = guit->bitmap->get_buffer(b);
    size_t stride = guit->bitmap->get_rowstride(b);
    if (!dst) {
        guit->bitmap->destroy(b);
        return NULL;
    }
    for (int y = 0; y < im->h; y++)
        memcpy(dst + (size_t)y * stride, im->px + (size_t)y * im->w, (size_t)im->w * 4);
    bitmap_fmt_t fmt = { .layout = BITMAP_LAYOUT_ARGB8888, .pma = false };
    bitmap_format_to_client(b, &fmt);
    guit->bitmap->modified(b);
    return b;
}

// image_cache's conversion: the finished thread's result, or for a small
// image (or no thread to be had) a decode right here.
static struct bitmap *uimgc_convert(struct content *c) {
    struct uimg_content *u = (struct uimg_content *)c;
    struct bitmap *b = NULL;
    if (u->job && job_done(u->job)) {
        if (u->job->rc == 0) b = to_bitmap(&u->job->im);
        else u->failed = true;
        job_unref(u->job);
        u->job = NULL;
        return b;
    }
    if (u->job) return NULL;   // still decoding
    size_t n;
    const uint8_t *data = content__get_source_data(c, &n);
    struct uimg im = { 0 };
    if (data && uimg_decode(data, n, &im) == 0) b = to_bitmap(&im);
    else u->failed = true;
    uimg_free(&im);
    return b;
}

static bool uimgc_redraw(struct content *c, struct content_redraw_data *data,
                         const struct rect *clip, const struct redraw_context *ctx) {
    struct uimg_content *u = (struct uimg_content *)c;
    if (u->failed) return true;   // a broken image: its box stays empty
    // Decoding, or about to: the box stays empty and poll_job() brings it
    // back. Everything else -- a bitmap in the cache, a finished job, a
    // small image -- is image_cache's ordinary path.
    if (!image_cache_find_bitmap(c) && c->size > SYNC_BYTES) {
        if (u->job && !job_done(u->job)) return true;
        if (!u->job && start_job(u)) return true;
    }
    return image_cache_redraw(c, data, clip, ctx);
}

static void uimgc_destroy(struct content *c) {
    struct uimg_content *u = (struct uimg_content *)c;
    if (u->polling) guit->misc->schedule(-1, poll_job, u);
    if (u->job) job_unref(u->job);   // a running worker keeps its own reference
    u->job = NULL;
    image_cache_destroy(c);
}

static nserror uimgc_create(const content_handler *handler, lwc_string *imime_type,
                            const struct http_parameter *params, llcache_handle *llcache,
                            const char *fallback_charset, bool quirks, struct content **c) {
    struct uimg_content *u = calloc(1, sizeof *u);
    if (!u) return NSERROR_NOMEM;
    nserror e = content__init(&u->base, handler, imime_type, params, llcache,
                              fallback_charset, quirks);
    if (e != NSERROR_OK) {
        free(u);
        return e;
    }
    *c = &u->base;
    return NSERROR_OK;
}

// The bytes are all here: read the header for the size, as jpeg.c does,
// and leave the pixels for the first draw.
static bool uimgc_convert_content(struct content *c) {
    size_t n;
    const uint8_t *data = content__get_source_data(c, &n);
    struct uimg_info info;
    if (!data || uimg_info(data, n, &info) != 0 || info.w <= 0 || info.h <= 0) {
        union content_msg_data msg;
        msg.errordata.errorcode = NSERROR_UNKNOWN;
        msg.errordata.errormsg = uimg_last_error();
        content_broadcast(c, CONTENT_MSG_ERROR, &msg);
        return false;
    }
    c->width = info.w;
    c->height = info.h;
    c->size = (size_t)info.w * info.h * 4;
    image_cache_add(c, NULL, uimgc_convert);

    // "photo.jpg (1920x1080)" -- what a tab showing the image alone says.
    char title[160];
    snprintf(title, sizeof title, "%s (%dx%d)",
             nsurl_access_leaf(llcache_handle_get_url(c->llcache)), info.w, info.h);
    content__set_title(c, title);
    content_set_ready(c);
    content_set_done(c);
    content_set_status(c, "");
    return true;
}

static nserror uimgc_clone(const struct content *old, struct content **newc) {
    struct uimg_content *u = calloc(1, sizeof *u);
    if (!u) return NSERROR_NOMEM;
    nserror e = content__clone(old, &u->base);
    if (e != NSERROR_OK) {
        content_destroy(&u->base);
        return e;
    }
    if ((old->status == CONTENT_STATUS_READY || old->status == CONTENT_STATUS_DONE) &&
        !uimgc_convert_content(&u->base)) {
        content_destroy(&u->base);
        return NSERROR_CLONE_FAILED;
    }
    *newc = &u->base;
    return NSERROR_OK;
}

static const content_handler g_handler = {
    .create = uimgc_create,
    .data_complete = uimgc_convert_content,
    .destroy = uimgc_destroy,
    .redraw = uimgc_redraw,
    .clone = uimgc_clone,
    .get_internal = image_cache_get_internal,
    .type = image_cache_content_type,
    .is_opaque = image_cache_is_opaque,
    .no_share = false,
};

static const char *g_types[] = {
    "image/png", "image/x-png", "image/jpeg", "image/jpg", "image/pjpeg", "image/qoi",
};

nserror ns_image_uimg_register(void) {
    for (size_t i = 0; i < sizeof g_types / sizeof g_types[0]; i++) {
        nserror e = content_factory_register_handler(g_types[i], &g_handler);
        if (e != NSERROR_OK) return e;
    }
    return NSERROR_OK;
}
