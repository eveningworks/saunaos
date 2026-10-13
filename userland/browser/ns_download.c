// Downloads. See ns_download.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/errors.h"
#include "browser/ns_download.h"

#include "desktop/download.h"
#include "netsurf/download.h"
#include "utils/nsurl.h"
#include "ui/ulog.h"

struct gui_download_window {
    download_context *ctx;     // NULL once it has ended
    FILE *f;
    char path[256], part[264];
    char url[512];
};

// Newest first: g_list[0] is the latest. Each row's uui_xfer and its
// window at the same index.
static struct uui_xfer g_items[NS_DOWNLOADS_MAX];
static struct gui_download_window *g_win[NS_DOWNLOADS_MAX];
static int g_count;

const struct uui_xfer *ns_downloads(int *count) {
    *count = g_count;
    return g_items;
}

int ns_downloads_running(void) {
    int n = 0;
    for (int i = 0; i < g_count; i++) n += g_items[i].state == UUI_XFER_RUNNING;
    return n;
}

static int index_of(const struct gui_download_window *dw) {
    for (int i = 0; i < g_count; i++)
        if (g_win[i] == dw) return i;
    return -1;
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// A name the server suggested, made safe for one path component, and
// made unique in the folder as Chrome does: "name (1).ext".
static void choose_path(const char *suggested, char *out, size_t cap) {
    char name[96];
    snprintf(name, sizeof name, "%s", suggested && suggested[0] ? suggested : "download");
    for (char *c = name; *c; c++)
        if (*c == '/' || (unsigned char)*c < 0x20) *c = '_';
    if (name[0] == '.') name[0] = '_';   // never a hidden file, never "." or ".."
    snprintf(out, cap, NS_DOWNLOAD_DIR "/%s", name);
    char part[280];
    snprintf(part, sizeof part, "%s.part", out);
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for (int n = 1; (exists(out) || exists(part)) && n < 100; n++) {
        snprintf(out, cap, NS_DOWNLOAD_DIR "/%.*s (%d)%s", stem, name, n, name + stem);
        snprintf(part, sizeof part, "%s.part", out);
    }
}

static struct gui_download_window *dl_create(download_context *ctx, struct gui_window *parent) {
    (void)parent;
    struct gui_download_window *dw = calloc(1, sizeof *dw);
    if (!dw) return NULL;
    if (g_count == NS_DOWNLOADS_MAX) {
        // The oldest that has ended makes room; with every slot still
        // running, the new one is refused.
        int k = g_count - 1;
        while (k >= 0 && g_items[k].state == UUI_XFER_RUNNING) k--;
        if (k < 0) { free(dw); return NULL; }
        free(g_win[k]);
        memmove(&g_items[k], &g_items[k + 1], (size_t)(g_count - k - 1) * sizeof g_items[0]);
        memmove(&g_win[k], &g_win[k + 1], (size_t)(g_count - k - 1) * sizeof g_win[0]);
        g_count--;
    }
    mkdir(NS_DOWNLOAD_DIR, 0755);
    dw->ctx = ctx;
    choose_path(download_context_get_filename(ctx), dw->path, sizeof dw->path);
    snprintf(dw->part, sizeof dw->part, "%s.part", dw->path);
    snprintf(dw->url, sizeof dw->url, "%s", nsurl_access(download_context_get_url(ctx)));
    dw->f = fopen(dw->part, "wb");
    memmove(&g_items[1], &g_items[0], (size_t)g_count * sizeof g_items[0]);
    memmove(&g_win[1], &g_win[0], (size_t)g_count * sizeof g_win[0]);
    g_count++;
    struct uui_xfer *x = &g_items[0];
    memset(x, 0, sizeof *x);
    const char *base = strrchr(dw->path, '/');
    snprintf(x->name, sizeof x->name, "%s", base ? base + 1 : dw->path);
    x->total = download_context_get_total_length(ctx);
    x->state = UUI_XFER_RUNNING;
    g_win[0] = dw;
    ulogf("browser: download start %s %llu\n", x->name, x->total);
    if (!dw->f) {
        // Nothing to write to: say so on the row rather than fail silently.
        x->state = UUI_XFER_FAILED;
        snprintf(x->why, sizeof x->why, "could not create the file in " NS_DOWNLOAD_DIR);
    }
    nsapp_download_changed(1);
    return dw;
}

static nserror dl_data(struct gui_download_window *dw, const char *data, unsigned size) {
    int i = index_of(dw);
    if (i < 0 || !dw->f) return NSERROR_SAVE_FAILED;
    if (fwrite(data, 1, size, dw->f) != size) {
        snprintf(g_items[i].why, sizeof g_items[i].why, "the disk did not take it");
        return NSERROR_SAVE_FAILED;   // NetSurf aborts the fetch; error() follows
    }
    g_items[i].got += size;
    nsapp_download_changed(0);
    return NSERROR_OK;
}

static void ended(struct gui_download_window *dw) {
    if (dw->f) fclose(dw->f);
    dw->f = NULL;
    if (dw->ctx) download_context_destroy(dw->ctx);   // the front end's to destroy
    dw->ctx = NULL;
}

static void dl_error(struct gui_download_window *dw, const char *msg) {
    int i = index_of(dw);
    ended(dw);
    unlink(dw->part);
    if (i < 0) return;
    g_items[i].state = UUI_XFER_FAILED;
    if (!g_items[i].why[0]) snprintf(g_items[i].why, sizeof g_items[i].why, "%s", msg ? msg : "it stopped");
    ulogf("browser: download failed %s %llu: %s\n", g_items[i].name, g_items[i].got, g_items[i].why);
    nsapp_download_changed(0);
}

static void dl_done(struct gui_download_window *dw) {
    int i = index_of(dw);
    ended(dw);
    if (i < 0) return;
    if (rename(dw->part, dw->path) != 0) {
        unlink(dw->part);
        g_items[i].state = UUI_XFER_FAILED;
        snprintf(g_items[i].why, sizeof g_items[i].why, "could not move it into place");
    } else {
        g_items[i].state = UUI_XFER_DONE;
    }
    ulogf("browser: download %s %s %llu\n", g_items[i].state == UUI_XFER_DONE ? "done" : "failed",
          g_items[i].name, g_items[i].got);
    nsapp_download_changed(0);
}

void ns_download_cancel(int i) {
    if (i < 0 || i >= g_count || g_items[i].state != UUI_XFER_RUNNING) return;
    struct gui_download_window *dw = g_win[i];
    if (dw->ctx) download_context_abort(dw->ctx);
    ended(dw);
    unlink(dw->part);
    g_items[i].state = UUI_XFER_CANCELLED;
    ulogf("browser: download cancelled %s %llu\n", g_items[i].name, g_items[i].got);
    nsapp_download_changed(0);
}

const char *ns_download_path(int i) { return i >= 0 && i < g_count ? g_win[i]->path : ""; }
const char *ns_download_url(int i) { return i >= 0 && i < g_count ? g_win[i]->url : ""; }

void ns_downloads_clear_finished(void) {
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_items[i].state == UUI_XFER_RUNNING) {
            g_items[n] = g_items[i];
            g_win[n++] = g_win[i];
        } else {
            free(g_win[i]);
        }
    }
    g_count = n;
    nsapp_download_changed(0);
}

static struct gui_download_table g_table = {
    .create = dl_create,
    .data = dl_data,
    .error = dl_error,
    .done = dl_done,
};
struct gui_download_table *ns_download_table = &g_table;
