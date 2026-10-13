// The Web Browser's side of /bin/webnet. See ns_net.h and ns_download.h.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "browser/ns_download.h"
#include "browser/ns_net.h"
#include "lib/ubulk.h"
#include "lib/webnet_proto.h"
#include "rt/sys.h"
#include "syscall_abi.h"
#include "ui/uapp.h"
#include "ui/ulog.h"

#define WEBNET "/bin/webnet"

static struct ubulk_client g_net;
static char g_service[UCHAN_NAME_MAX];
static struct uapp *g_app;
static int g_post_id, g_up;

// --- downloads, as webnet reports them --------------------------------------

// Newest first: row 0 is the latest. Each row's webnet id, path and URL
// at the same index.
static struct uui_xfer g_items[NS_DOWNLOADS_MAX];
static struct { uint32_t id; char path[256]; char url[1024]; } g_meta[NS_DOWNLOADS_MAX];
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

void ns_download_cancel(int i) {
    if (i >= 0 && i < g_count && g_items[i].state == UUI_XFER_RUNNING && g_up)
        ubulk_send(&g_net, WN_DL_CANCEL, g_meta[i].id, NULL, 0, 500);
}

const char *ns_download_path(int i) { return i >= 0 && i < g_count ? g_meta[i].path : ""; }
const char *ns_download_url(int i) { return i >= 0 && i < g_count ? g_meta[i].url : ""; }

void ns_downloads_clear_finished(void) {
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_items[i].state != UUI_XFER_RUNNING) continue;
        g_items[n] = g_items[i];
        g_meta[n++] = g_meta[i];
    }
    g_count = n;
    nsapp_download_changed(0);
}

static int row_of(uint32_t id) {
    for (int i = 0; i < g_count; i++) if (g_meta[i].id == id) return i;
    return -1;
}

static void on_dl(const struct ubulk_msg *m) {
    struct wn_dl h;
    if (m->len < sizeof h) return;
    memcpy(&h, m->data, sizeof h);
    if (h.name_len < 0 || h.path_len < 0 || h.url_len < 0 ||
        sizeof h + (size_t)h.name_len + (size_t)h.path_len + (size_t)h.url_len > m->len)
        return;
    const char *p = (const char *)m->data + sizeof h;
    int i = row_of(m->id), started = 0;
    if (i < 0) {
        if (g_count == NS_DOWNLOADS_MAX) {
            // The oldest that has ended makes room; with every slot
            // running, the oldest goes from the list (not from webnet).
            int k = g_count - 1;
            while (k > 0 && g_items[k].state == UUI_XFER_RUNNING) k--;
            memmove(&g_items[k], &g_items[k + 1], (size_t)(g_count - k - 1) * sizeof g_items[0]);
            memmove(&g_meta[k], &g_meta[k + 1], (size_t)(g_count - k - 1) * sizeof g_meta[0]);
            g_count--;
        }
        memmove(&g_items[1], &g_items[0], (size_t)g_count * sizeof g_items[0]);
        memmove(&g_meta[1], &g_meta[0], (size_t)g_count * sizeof g_meta[0]);
        g_count++;
        i = 0;
        memset(&g_items[0], 0, sizeof g_items[0]);
        memset(&g_meta[0], 0, sizeof g_meta[0]);
        g_meta[0].id = m->id;
        started = 1;
    }
    struct uui_xfer *x = &g_items[i];
    snprintf(x->name, sizeof x->name, "%.*s", h.name_len, p);
    snprintf(g_meta[i].path, sizeof g_meta[i].path, "%.*s", h.path_len, p + h.name_len);
    snprintf(g_meta[i].url, sizeof g_meta[i].url, "%.*s", h.url_len, p + h.name_len + h.path_len);
    int was = x->state;
    x->got = h.got > 0 ? (unsigned long long)h.got : 0;
    x->total = h.total > 0 ? (unsigned long long)h.total : 0;
    x->state = h.state == WN_DL_DONE ? UUI_XFER_DONE : h.state == WN_DL_FAILED ? UUI_XFER_FAILED
             : h.state == WN_DL_CANCELLED ? UUI_XFER_CANCELLED : UUI_XFER_RUNNING;
    if (x->state == UUI_XFER_FAILED && !x->why[0]) snprintf(x->why, sizeof x->why, "it stopped");
    if (started) ulogf("browser: download start %s %llu\n", x->name, x->total);
    if (x->state != was || started) {
        if (x->state == UUI_XFER_DONE) ulogf("browser: download done %s %llu\n", x->name, x->got);
        if (x->state == UUI_XFER_FAILED)
            ulogf("browser: download failed %s %llu: %s\n", x->name, x->got, x->why);
        if (x->state == UUI_XFER_CANCELLED) ulogf("browser: download cancelled %s %llu\n", x->name, x->got);
    }
    nsapp_download_changed(started);
}

// --- the process and its channel ------------------------------------------------

static void *watch(void *arg) {
    (void)arg;
    uint32_t seen = 0;
    for (;;)
        if (ubulk_client_wait_change(&g_net, &seen, 5000)) uapp_post(g_app, g_post_id, 0);
    return NULL;
}

int ns_net_start(struct uapp *a, int post_id, int weak_entropy) {
    g_app = a;
    g_post_id = post_id;
    snprintf(g_service, sizeof g_service, WEBNET_SERVICE_FMT, sys_getpid());
    char args[64];
    snprintf(args, sizeof args, "--app %d%s", sys_getpid(), weak_entropy ? " --weak-entropy" : "");
    int pid = sys_spawn(WEBNET, args, SPAWN_FD_LOG);
    if (pid <= 0) {
        ulogf("browser: could not start " WEBNET "\n");
        return 0;
    }
    uapp_track_child(a, pid);
    // It is up when its channel is: a second at most on a loaded machine.
    for (int i = 0; i < 200 && !g_up; i++) {
        if (ubulk_client_open(&g_net, g_service) == 0) g_up = 1;
        else sys_sleep_ms(10);
    }
    if (!g_up) {
        ulogf("browser: " WEBNET " did not come up\n");
        return 0;
    }
    ubulk_send(&g_net, WN_HELLO_APP, 0, NULL, 0, 500);
    pthread_t t;
    pthread_create(&t, NULL, watch, NULL);
    return 1;
}

const char *ns_net_service(void) { return g_service; }

static struct wn_cookies g_answer;
static int g_answered;

static void handle(const struct ubulk_msg *m) {
    if (m->type == WN_DL) on_dl(m);
    if (m->type == WN_COOKIES && m->len >= sizeof g_answer) {
        memcpy(&g_answer, m->data, sizeof g_answer);
        g_answered = 1;
    }
}

int ns_net_on_post(void) {
    if (!g_up) return 0;
    static char buf[UBULK_REC_MAX];
    struct ubulk_msg m;
    int any = 0;
    while (ubulk_client_recv(&g_net, &m, buf, sizeof buf)) {
        handle(&m);
        any = 1;
    }
    return any;
}

// One question, waited for: whatever else arrives meanwhile is handled.
static int ask(uint32_t type, const char *host, struct wn_cookies *out) {
    if (!g_up || !host || !host[0]) return -1;
    g_answered = 0;
    if (ubulk_send(&g_net, type, 0, host, (uint32_t)strlen(host), 500) != 0) return -1;
    unsigned long long until = sys_monotonic_ns() + 500000000ull;
    while (!g_answered && sys_monotonic_ns() < until) {
        ubulk_client_wait(&g_net, 50);
        ns_net_on_post();
    }
    if (!g_answered) return -1;
    *out = g_answer;
    return 0;
}

int ns_net_cookies(const char *host) {
    struct wn_cookies c;
    return ask(WN_COOKIES_COUNT, host, &c) == 0 ? c.count : -1;
}

int ns_net_cookies_clear(const char *host) {
    struct wn_cookies c;
    return ask(WN_COOKIES_CLEAR, host, &c) == 0 ? c.cleared : -1;
}
