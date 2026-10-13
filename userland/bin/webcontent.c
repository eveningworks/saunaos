// webcontent -- a web page's engine, ONE PROCESS PER VIEW
// (docs/browser-design.md, stage 5): NetSurf parsing, laying out and
// painting one page into shared memory for the app that embeds it
// (ui/uui_webview.h), which starts it. Chrome's renderer, WebKit's
// WebContent process.
//
// THE ONE GPL-2.0 BINARY OF THE BROWSER. NetSurf is linked here and
// nowhere else, so the Web Browser, Help and System Update stay MIT; a
// page that crashes NetSurf costs this process, and the app shows the
// last frame it had (C3).
//
// It reaches the network only through /bin/webnet (`--net`), and a view
// given none -- Help -- reaches only local pages and the `app:` scheme.
// It has no window: input arrives as the app's records, and the page
// leaves as a pixel buffer and FRAMEs.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "webcontent/ns_bitmap.h"
#include "webcontent/ns_font.h"
#include "webcontent/ns_sched.h"
#include "webcontent/wc.h"

#include "netsurf/browser.h"
#include "netsurf/browser_window.h"
#include "netsurf/netsurf.h"
#include "content/fetch.h"
#include "desktop/version.h"
#include "backends/netsurf/fetch_http.h"
#include "backends/netsurf/image_uimg.h"
#include "utils/messages.h"
#include "utils/nsoption.h"

#include "lib/uargs.h"
#include "lib/usetting.h"
#include "rt/sys.h"

#define MESSAGES "/usr/share/netsurf/Messages"
#define SEND_WAIT_MS 2000

struct ubulk_client g_wc_app, g_wc_net;
int g_wc_has_net;
static char g_net_service[UCHAN_NAME_MAX];
static int g_app_gone;

void wc_send_id(uint32_t type, uint32_t id, const void *data, uint32_t len) {
    if (!g_app_gone && ubulk_send(&g_wc_app, type, id, data, len, SEND_WAIT_MS) != 0)
        g_app_gone = !ubulk_client_server_alive(&g_wc_app);
}

void wc_send(uint32_t type, const void *data, uint32_t len) { wc_send_id(type, 0, data, len); }

void wc_send_str(uint32_t type, const char *s) { wc_send(type, s, (uint32_t)strlen(s)); }

// The User-Agent, from browser.user_agent -- read at each navigation, so
// a change in System Settings applies to the next page loaded.
static void apply_user_agent(void) {
    char which[16], ua[256];
    if (usetting_get("browser.user_agent", which, sizeof which) < 0) strcpy(which, "toyos");
    if (!strcmp(which, "custom") &&
        usetting_get("browser.user_agent_custom", ua, sizeof ua) >= 0 && ua[0]) {
        ns_fetch_http_set_user_agent(ua);
        return;
    }
    snprintf(ua, sizeof ua, "Mozilla/5.0 (%s) NetSurf/%d.%d",
             strcmp(which, "linux") ? "toy-os; x86_64" : "X11; Linux x86_64",
             netsurf_version_major, netsurf_version_minor);
    ns_fetch_http_set_user_agent(ua);
}

// --- the network process's doorbell --------------------------------------
//
// This process parks on the APP's channel; the network rings another. A
// thread watches that one and kicks the app channel's wait, the one thing
// it does -- the main thread still reads both.
static void *net_watch(void *arg) {
    (void)arg;
    uint32_t seen = 0;
    for (;;) {
        if (ubulk_client_wait_change(&g_wc_net, &seen, 1000)) ubulk_client_kick(&g_wc_app);
    }
    return NULL;
}

// Joins the network process, and starts the watcher the first time.
static int net_connect(void) {
    static int watching;
    if (ubulk_client_open(&g_wc_net, g_net_service) != 0) return 0;
    g_wc_has_net = 1;
    if (!watching) {
        pthread_t t;
        watching = pthread_create(&t, NULL, net_watch, NULL) == 0;
    }
    return 1;
}

static nserror set_defaults(struct nsoption_s *defaults) {
    (void)defaults;
    return NSERROR_OK;
}

static int engine_init(int argc, char **argv, int scheme) {
    // STATIC: netsurf_register() keeps the POINTER.
    static struct netsurf_table table;
    table.misc = wc_misc_table;
    table.window = wc_window_table;
    table.fetch = wc_fetch_table;
    table.bitmap = ns_bitmap_table;
    table.layout = ns_layout_table;
    table.download = wc_download_table;
    if (netsurf_register(&table) != NSERROR_OK) return 0;
    ns_bitmap_set_format();
    if (nsoption_init(set_defaults, &nsoptions, &nsoptions_default) != NSERROR_OK) return 0;
    nsoption_commandline(&argc, argv, nsoptions);
    // Six fetches at once and six to one host -- Chrome's and Firefox's
    // per-host figure.
    nsoption_set_int(max_fetchers, 6);
    nsoption_set_int(max_fetchers_per_host, 6);
    if (messages_add_from_file(MESSAGES) != NSERROR_OK) printf("webcontent: no %s\n", MESSAGES);
    if (netsurf_init(NULL) != NSERROR_OK) return 0;
    if (g_net_service[0]) {
        wc_net_init();
        if (ns_fetch_http_register() != 0) printf("webcontent: no http fetcher\n");
    }
    if (scheme && wc_scheme_register() != 0) printf("webcontent: no app: scheme\n");
    if (ns_image_uimg_register() != NSERROR_OK) printf("webcontent: no PNG/JPEG handler\n");
    apply_user_agent();
    browser_set_dpi(96);
    return 1;
}

static const char *f_app, *f_net;
static int f_scheme;

static const struct uargs_opt OPTS[] = {
    { "app", 0, "SERVICE", "the embedding app's channel (wv<pid>)", 0, &f_app },
    { "net", 0, "SERVICE", "the network process's channel (wn<pid>); none: local pages only", 0, &f_net },
    { "scheme", 0, 0, "ask the embedding app for app: pages", &f_scheme, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "webcontent",
    .usage = "--app SERVICE [--net SERVICE] [--scheme]",
    .summary = "A web page's engine, one process per view: NetSurf painting into shared\n"
               "memory for the app that embeds it. Started by ui/uui_webview, not by hand.",
    .opts = OPTS,
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    setvbuf(stdout, NULL, _IOLBF, 0);   // the application log is a pipe: a line at a time
    if (!f_app) return uargs_error(&PROG, "--app SERVICE is required");
    if (ubulk_client_open(&g_wc_app, f_app) != 0) {
        fprintf(stderr, "webcontent: no app at %s\n", f_app);
        return 1;
    }
    if (f_net) snprintf(g_net_service, sizeof g_net_service, "%s", f_net);
    if (g_net_service[0] && !net_connect()) printf("webcontent: no network process at %s\n", f_net);
    struct wv_int hello = { WEBVIEW_VERSION };
    wc_send(WV_HELLO, &hello, sizeof hello);
    if (!ns_font_init()) printf("webcontent: no page fonts -- text draws as boxes\n");
    char *nargv[] = { argv[0], NULL };
    if (!engine_init(1, nargv, f_scheme)) {
        printf("webcontent: NetSurf failed to start\n");
        return 1;
    }

    static char buf[UBULK_REC_MAX];
    unsigned long long last_check = sys_monotonic_ns();
    while (!g_app_gone) {
        struct ubulk_msg m;
        while (ubulk_client_recv(&g_wc_app, &m, buf, sizeof buf)) {
            if (m.type == WV_NAVIGATE || m.type == WV_RELOAD) apply_user_agent();
            wc_window_handle(&m);
        }
        // Every fetcher's poll -- the network's records and the app's
        // answers are both delivered there.
        fd_set r, w, e;
        int maxfd;
        fetch_fdset(&r, &w, &e, &maxfd);
        int due = ns_sched_run();
        wc_window_flush();

        unsigned long long now = sys_monotonic_ns();
        if (now - last_check > 1000000000ull) {
            last_check = now;
            if (!ubulk_client_server_alive(&g_wc_app)) break;
            // A network process that died ends what was in flight; its
            // successor (the app restarts it) is joined when it appears.
            // The old rings stay mapped: the watcher may be parked on one.
            if (g_wc_has_net && !ubulk_client_server_alive(&g_wc_net)) {
                ns_fetch_http_remote_fail_all("the network process stopped");
                g_wc_has_net = 0;
            }
            if (!g_wc_has_net && g_net_service[0]) net_connect();
        }
        ubulk_client_wait(&g_wc_app, due < 0 ? 1000 : due < 1 ? 1 : due);
    }
    return 0;
}
