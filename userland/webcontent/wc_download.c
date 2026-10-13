// Downloads, handed to /bin/webnet. NetSurf decides a response is a
// download -- an attachment, a type it draws nothing for -- from bytes it
// has been sent, and asks for a download window here. The response is
// webnet's to save: it kept a copy of what it sent and writes the rest
// straight to the file, so the download outlives this tab. Refusing the
// window ends NetSurf's side of it, and its abort is one webnet ignores
// for a fetch that became a download.
#include <stddef.h>

#include "utils/errors.h"
#include "webcontent/wc.h"

#include "desktop/download.h"
#include "netsurf/download.h"
#include "utils/nsurl.h"
#include "backends/netsurf/fetch_http.h"

static struct gui_download_window *dl_create(download_context *ctx, struct gui_window *parent) {
    (void)parent;
    const char *url = nsurl_access(download_context_get_url(ctx));
    const char *name = download_context_get_filename(ctx);
    wc_net_download(ns_fetch_http_remote_id_for(url), name ? name : "");
    return NULL;
}

static nserror dl_data(struct gui_download_window *dw, const char *data, unsigned size) {
    (void)dw;
    (void)data;
    (void)size;
    return NSERROR_SAVE_FAILED;
}

static void dl_error(struct gui_download_window *dw, const char *msg) {
    (void)dw;
    (void)msg;
}

static void dl_done(struct gui_download_window *dw) { (void)dw; }

static struct gui_download_table g_table = {
    .create = dl_create,
    .data = dl_data,
    .error = dl_error,
    .done = dl_done,
};
struct gui_download_table *wc_download_table = &g_table;
