// See uui_webview.h. docs/browser-design.md, stage 5, has the protocol's
// reasons; this file is the app's half of it.
//
// THREADS. The app's loop parks on the compositor's channel, so two
// kinds of helper thread exist: ONE listener, parked on this app's
// channel, that uapp_post()s when a content process writes; and one
// REAPER per content process, parked in sys_waitpid(), that posts its
// death. Neither touches a view -- the main thread reads the channel and
// the deaths in uui_webview_on_post().
#include "ui/uui_webview.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "lib/ubulk.h"
#include "lib/ufileurl.h"
#include "lib/umd.h"
#include "query_abi.h"
#include "sandbox_abi.h"
#include "rt/sys.h"
#include "signal_abi.h"
#include "syscall_abi.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "win_proto.h"   // WIN_CURSOR_*
#include "keyboard.h"    // KEY_*

#define ENGINE      "/bin/webcontent"
// WHAT THE ENGINE KEEPS, from its first instruction (SPAWN_SANDBOX): its
// own memory and threads, reading files (narrowed again to its fonts and
// resources by its own unveil), shared memory for its buffers and
// channels, and reading settings. No network, no spawn, no windows, no
// writing anywhere.
#define ENGINE_PROMISES (PROMISE_STDIO | PROMISE_RPATH | PROMISE_SHM | PROMISE_SETTINGS)
// The most of a local file the widget reads for a page -- the engine's
// own cap on a scheme answer (webcontent/wc_scheme.c).
#define FILE_MAX    (8u * 1024 * 1024)
#define SEND_MS     50     // the UI thread never waits longer on a busy engine
#define REPORT_FMT  "/var/crash/webcontent-%d.crash"

static struct ubulk_server g_srv;
static int g_open, g_post_id;
static struct uapp *g_app;
static struct uui_webview *g_views;
static int g_limit;
static volatile int g_posted;

// Deaths the reapers saw, for the main thread: a small ring of pids.
#define DEATHS 32
static pthread_mutex_t g_death_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_death[DEATHS], g_ndeath;

static void post(void) {
    // One post outstanding at a time: on_post drains everything.
    if (__atomic_exchange_n(&g_posted, 1, __ATOMIC_SEQ_CST)) return;
    if (uapp_post(g_app, g_post_id, 0) != 0) g_posted = 0;
}

static void *listener(void *arg) {
    (void)arg;
    for (;;)
        if (ubulk_server_wait_change(&g_srv, 5000)) post();
    return NULL;
}

static void *reaper(void *arg) {
    int pid = (int)(intptr_t)arg;
    sys_waitpid(pid, NULL);
    pthread_mutex_lock(&g_death_mu);
    if (g_ndeath < DEATHS) g_death[g_ndeath++] = pid;
    pthread_mutex_unlock(&g_death_mu);
    g_posted = 0;   // a death must not be swallowed by a post in flight
    post();
    return NULL;
}

int uui_webview_setup(struct uapp *a, int post_id) {
    if (g_open) return 0;
    char name[UCHAN_NAME_MAX];
    snprintf(name, sizeof name, WEBVIEW_SERVICE_FMT, sys_getpid());
    if (ubulk_server_open(&g_srv, name) != 0) return -1;
    g_app = a;
    g_post_id = post_id;
    pthread_t t;
    if (pthread_create(&t, NULL, listener, NULL) != 0) return -1;
    g_open = 1;
    return 0;
}

int uui_webview_available(void) {
    struct stat st;
    return stat(ENGINE, &st) == 0;
}

// --- sending ------------------------------------------------------------

static int send_rec(struct uui_webview *v, uint32_t type, const void *data, uint32_t len) {
    if (!v->pid || !v->ready) return -1;
    return ubulk_server_send(&g_srv, v->pid, type, 0, data, len, SEND_MS);
}

static void send_int(struct uui_webview *v, uint32_t type, int value) {
    struct wv_int i = { value };
    send_rec(v, type, &i, sizeof i);
}

static int page_w(const struct uui_webview *v) {
    int w = v->w - uui_sbar_width();
    return w > 1 ? w : 1;
}

static void send_size(struct uui_webview *v) {
    if (!v->ready || (v->sent_w == page_w(v) && v->sent_h == v->h)) return;
    struct wv_size s = { page_w(v), v->h > 1 ? v->h : 1, ugfx_char_h() };
    if (send_rec(v, WV_RESIZE, &s, sizeof s) == 0) {
        v->sent_w = s.w;
        v->sent_h = s.h;
    }
}

// --- the content process's lifetime ------------------------------------------

static int live_count(void) {
    int n = 0;
    for (struct uui_webview *v = g_views; v; v = v->next) n += v->pid != 0;
    return n;
}

// The machine's share: a content process laying out a heavy page holds
// tens of MiB, and the rest of the desktop needs room too.
static int limit(void) {
    if (g_limit > 0) return g_limit;
    struct query_meminfo m;
    memset(&m, 0, sizeof m);
    uint64_t mib = 2048;
    if (sys_query_record(QUERY_MEMINFO, 0, &m, sizeof m) > 0 && m.frame_bytes)
        mib = m.frame_total * m.frame_bytes >> 20;
    int n = (int)((mib > 512 ? mib - 512 : 0) / 160);
    return n < 2 ? 2 : n;
}

void uui_webview_set_limit(int max_live) { g_limit = max_live; }

static void unmap_buf(struct uui_webview *v) {
    if (v->buf) sys_munmap((void *)v->buf, v->bbytes);
    v->buf = NULL;
    v->bbytes = 0;
}

static void end_process(struct uui_webview *v) {
    if (v->pid) sys_kill(v->pid, SIGKILL);
    v->pid = 0;
    v->ready = 0;
    unmap_buf(v);
}

static void unload_oldest(struct uui_webview *keep) {
    while (live_count() > limit()) {
        struct uui_webview *old = NULL;
        for (struct uui_webview *v = g_views; v; v = v->next)
            if (v->pid && v != keep && !v->visible && (!old || v->shown_ns < old->shown_ns)) old = v;
        if (!old) return;
        ulogf("webview: unloaded %d (%s)\n", old->pid, old->url);
        uui_webview_history_snapshot(old);
        end_process(old);
        old->unloaded = 1;
    }
}

static void start_process(struct uui_webview *v) {
    if (v->pid || !g_open) return;
    char args[176];
    snprintf(args, sizeof args, "--app " WEBVIEW_SERVICE_FMT "%s%s%s%s", sys_getpid(),
             v->net ? " --net " : "", v->net ? v->net : "", v->scheme ? " --scheme" : "",
             v->files ? " --files" : "");
    extern char **environ;
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = args;
    o.env = environ;
    o.stdout_fd = SPAWN_FD_LOG;
    o.flags = SPAWN_SANDBOX;
    o.promises = ENGINE_PROMISES;
    int pid = sys_spawn_opts(ENGINE, &o);
    v->stopped = v->unloaded = 0;
    v->report[0] = '\0';
    if (pid <= 0) {
        // No engine (a NOGPL build) or no memory: the stopped view says so.
        v->stopped = 1;
        ulogf("webview: could not start " ENGINE "\n");
        return;
    }
    v->pid = pid;
    v->ready = 0;
    v->sent_w = v->sent_h = 0;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16384);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, reaper, (void *)(intptr_t)pid);
    pthread_attr_destroy(&at);
    ulogf("webview: content %d\n", pid);
    unload_oldest(v);
}

void uui_webview_init(struct uui_webview *v, const struct uui_webview_host *host, void *user) {
    memset(v, 0, sizeof *v);
    v->host = host;
    v->user = user;
    v->hover_x = v->hover_y = -1;
    v->st.zoom = 100;
    uui_sbar_init(&v->sb);
    uui_infobar_init(&v->bar, NULL,
                     "This page stopped. What you see is its last picture; links and scrolling no "
                     "longer work.",
                     "Reload");
    uui_infobar_set_second(&v->bar, "View report");
    v->bar.error = 1;
    v->next = g_views;
    g_views = v;
}

static void hist_clear(struct uui_webview *v) {
    for (int i = 0; i < v->nhist; i++) {
        free(v->hist[i].url);
        free(v->hist[i].title);
    }
    v->nhist = v->hist_at = 0;
}

void uui_webview_destroy(struct uui_webview *v) {
    end_process(v);
    hist_clear(v);
    free(v->copy);
    free(v->pending_nav);
    v->copy = NULL;
    v->pending_nav = NULL;
    for (struct uui_webview **p = &g_views; *p; p = &(*p)->next)
        if (*p == v) { *p = v->next; break; }
}

static void changed(struct uui_webview *v, unsigned what) {
    if (v->host && v->host->changed) v->host->changed(v, what);
}

// --- local pages ----------------------------------------------------------------

// A file: URL the engine cannot read for itself: anything outside its own
// resources. A URL that does not even parse counts -- it is refused
// below, by a process that was never a web page's.
static int local_page(const char *url) {
    if (strncasecmp(url, "file:", 5) != 0) return 0;
    static char path[4096];
    if (ufileurl_to_path(url, path, sizeof path) != 0) return 1;
    size_t n = sizeof WEBVIEW_ENGINE_FILES - 1;
    return strncmp(path, WEBVIEW_ENGINE_FILES, n) != 0 || (path[n] && path[n] != '/');
}

// ONE KIND OF PAGE PER PROCESS: a local page's process reads files
// through this widget and a web page's never does, so a view moving
// between the two starts a fresh content process (Chrome's process swap
// across a site boundary). Called before anything is sent for `url`.
static void match_kind(struct uui_webview *v, const char *url) {
    int want = local_page(url);
    if (want == v->files) return;
    if (v->pid) {
        uui_webview_history_snapshot(v);
        end_process(v);
    }
    v->files = want;
}

// --- what the app asks for -------------------------------------------------

static void navigate_as(struct uui_webview *v, const char *url, int upgraded) {
    uint32_t type = upgraded ? WV_NAVIGATE_UPGRADED : WV_NAVIGATE;
    match_kind(v, url);
    // A live page keeps ITS address until its content process reports
    // the new one: an app shown the new address beside the old page's
    // finished state would take the old page for the new.
    if (!v->ready) snprintf(v->url, sizeof v->url, "%s", url);
    if (v->ready) {
        send_rec(v, type, url, (uint32_t)strlen(url));
        return;
    }
    free(v->pending_nav);
    v->pending_nav = strdup(url);
    v->pending_upgraded = upgraded;
    v->pending_go = 0;
    // LOADING FROM HERE, not from the engine's first word: a page waiting
    // for its process (a fresh one after a swap, say) is not the old
    // page at rest, and an app told it was would act on a page that has
    // not arrived -- its next navigation then replaces this one unseen.
    v->st.loading = 1;
    if (!v->pid) start_process(v);
    if (v->stopped) changed(v, UUI_WV_STOPPED);
    changed(v, UUI_WV_STATE);
}

void uui_webview_navigate(struct uui_webview *v, const char *url) { navigate_as(v, url, 0); }
void uui_webview_navigate_upgraded(struct uui_webview *v, const char *url) { navigate_as(v, url, 1); }

// --- back/forward ---------------------------------------------------------------

static void hist_sync(struct uui_webview *v) {
    v->can_back = v->hist_at > 0;
    v->can_forward = v->hist_at < v->nhist - 1;
}

static char *dup_or_empty(const char *s) { return strdup(s ? s : ""); }

void uui_webview_history_snapshot(struct uui_webview *v) {
    if (!v->nhist || !v->hist_live) return;
    v->hist[v->hist_at].sx = v->live_sx;
    v->hist[v->hist_at].sy = v->live_sy;
}

static void send_go(struct uui_webview *v) {
    const struct uui_wv_entry *e = &v->hist[v->hist_at];
    static char buf[sizeof(struct wv_go) + WEBVIEW_URL_MAX];
    struct wv_go g = { e->sx, e->sy };
    size_t n = strlen(e->url);
    if (n > WEBVIEW_URL_MAX) n = WEBVIEW_URL_MAX;
    memcpy(buf, &g, sizeof g);
    memcpy(buf + sizeof g, e->url, n);
    send_rec(v, WV_GO, buf, (uint32_t)(sizeof g + n));
}

// Loads hist[hist_at] without adding an entry -- through a content
// process that is alive, or one started for it.
static void go_entry(struct uui_webview *v) {
    const struct uui_wv_entry *e = &v->hist[v->hist_at];
    match_kind(v, e->url);
    snprintf(v->url, sizeof v->url, "%s", e->url);
    snprintf(v->title, sizeof v->title, "%s", e->title);
    v->hist_live = 0;
    v->unloaded = 0;
    hist_sync(v);
    if (v->ready) {
        send_go(v);
    } else {
        free(v->pending_nav);
        v->pending_nav = NULL;
        v->pending_go = 1;
        v->st.loading = 1;   // as navigate_as() says
        if (v->stopped) {
            v->stopped = 0;
            changed(v, UUI_WV_STOPPED);
        }
        if (!v->pid) start_process(v);
    }
    changed(v, UUI_WV_TITLE | UUI_WV_URL | UUI_WV_STATE);
}

static void step(struct uui_webview *v, int delta) {
    int to = v->hist_at + delta;
    if (to < 0 || to >= v->nhist) return;
    uui_webview_history_snapshot(v);
    v->hist_at = to;
    go_entry(v);
}

void uui_webview_back(struct uui_webview *v) { step(v, -1); }
void uui_webview_forward(struct uui_webview *v) { step(v, +1); }

// The engine committed a page. A new one truncates what was ahead of the
// current entry, as every browser does; the same address again is the
// current entry, not a step (Chrome makes it a replacement).
static void hist_commit(struct uui_webview *v, const char *url, int replace) {
    if (!replace && v->nhist && !strcmp(v->hist[v->hist_at].url, url)) replace = 1;
    if (replace && v->nhist) {
        free(v->hist[v->hist_at].url);
        v->hist[v->hist_at].url = dup_or_empty(url);
    } else {
        uui_webview_history_snapshot(v);
        while (v->nhist > v->hist_at + 1) {
            v->nhist--;
            free(v->hist[v->nhist].url);
            free(v->hist[v->nhist].title);
        }
        if (v->nhist == UUI_WV_HISTORY_MAX) {
            free(v->hist[0].url);
            free(v->hist[0].title);
            memmove(v->hist, v->hist + 1, (UUI_WV_HISTORY_MAX - 1) * sizeof v->hist[0]);
            v->nhist--;
        }
        struct uui_wv_entry *e = &v->hist[v->nhist++];
        e->url = dup_or_empty(url);
        e->title = dup_or_empty("");
        e->sx = e->sy = 0;
        v->hist_at = v->nhist - 1;
    }
    v->hist_live = 0;
    hist_sync(v);
}

void uui_webview_history_set(struct uui_webview *v, const struct uui_wv_entry *e, int n, int at) {
    hist_clear(v);
    if (n > UUI_WV_HISTORY_MAX) {
        at -= n - UUI_WV_HISTORY_MAX;
        e += n - UUI_WV_HISTORY_MAX;
        n = UUI_WV_HISTORY_MAX;
    }
    for (int i = 0; i < n; i++) {
        v->hist[i].url = dup_or_empty(e[i].url);
        v->hist[i].title = dup_or_empty(e[i].title);
        v->hist[i].sx = e[i].sx;
        v->hist[i].sy = e[i].sy;
    }
    v->nhist = n;
    v->hist_at = at < 0 ? 0 : at >= n ? n - 1 : at;
    v->hist_live = 0;
    hist_sync(v);
    if (!n) return;
    snprintf(v->url, sizeof v->url, "%s", v->hist[v->hist_at].url);
    snprintf(v->title, sizeof v->title, "%s", v->hist[v->hist_at].title);
    if (!v->pid) v->unloaded = 1;
    changed(v, UUI_WV_TITLE | UUI_WV_URL | UUI_WV_STATE);
}

// Reloads -- and a stopped or unloaded view goes back to its current
// entry, scrolled where it was, rather than adding the page again.
void uui_webview_reload(struct uui_webview *v) {
    if (v->pid && v->ready && !v->stopped) {
        send_rec(v, WV_RELOAD, NULL, 0);
        return;
    }
    if (v->nhist) {
        go_entry(v);
        return;
    }
    if (v->stopped) {
        v->stopped = 0;
        changed(v, UUI_WV_STOPPED);
    }
    uui_webview_navigate(v, v->url);
}

void uui_webview_stop(struct uui_webview *v) { send_rec(v, WV_STOP, NULL, 0); }
void uui_webview_zoom(struct uui_webview *v, int dir) { send_int(v, WV_ZOOM, dir); }
void uui_webview_crash(struct uui_webview *v) { send_rec(v, WV_CRASH, NULL, 0); }
void uui_webview_find_clear(struct uui_webview *v) { send_rec(v, WV_FIND_CLEAR, NULL, 0); }

void uui_webview_find(struct uui_webview *v, const char *query, int dir) {
    char buf[sizeof(struct wv_find) + 256];
    struct wv_find f = { dir };
    size_t n = strlen(query);
    if (n > 255) n = 255;
    memcpy(buf, &f, sizeof f);
    memcpy(buf + sizeof f, query, n);
    send_rec(v, WV_FIND, buf, (uint32_t)(sizeof f + n));
}

void uui_webview_set_visible(struct uui_webview *v, int visible) {
    v->visible = visible;
    if (visible) {
        v->shown_ns = sys_monotonic_ns();
        if (v->unloaded && v->url[0]) {
            v->unloaded = 0;
            if (v->nhist) go_entry(v);
            else uui_webview_navigate(v, v->url);
        }
    }
    send_int(v, WV_VISIBLE, visible);
}

void uui_webview_scheme_reply(struct uui_webview *v, uint32_t id, int status, const char *mime,
                              const void *data, uint32_t len, int done) {
    static char buf[UBULK_REC_MAX];
    size_t ml = mime ? strlen(mime) : 0;
    const char *p = data;
    do {
        size_t room = sizeof buf - sizeof(struct wv_scheme) - ml;
        size_t n = len > room ? room : len;
        struct wv_scheme h = { status, done && n == len, (int32_t)ml };
        memcpy(buf, &h, sizeof h);
        memcpy(buf + sizeof h, mime ? mime : "", ml);
        if (n) memcpy(buf + sizeof h + ml, p, n);
        if (!v->pid || ubulk_server_send(&g_srv, v->pid, WV_SCHEME_DATA, id, buf,
                                         (uint32_t)(sizeof h + ml + n), 2000) != 0)
            return;
        p += n;
        len -= (uint32_t)n;
        ml = 0;
        mime = NULL;
    } while (len);
}

// THE ENGINE'S LOCAL FILES, read here because it cannot. Only for a
// process started for a local page; a web page's gets 403 and the engine
// shows "A web page cannot open files on this computer."
static int html_put(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t c = *cap ? *cap * 2 : 4096;
        while (c < *len + n + 1) c *= 2;
        char *g = realloc(*buf, c);
        if (!g) return 0;
        *buf = g;
        *cap = c;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

static int html_puts(char **buf, size_t *len, size_t *cap, const char *s) {
    return html_put(buf, len, cap, s, strlen(s));
}

// `s` as HTML text, or (as_url) percent-escaped for an href.
static int html_put_escaped(char **buf, size_t *len, size_t *cap, const char *s, int as_url) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char e[8];
        const char *out = e;
        if (as_url) {
            if (*p <= ' ' || *p >= 0x7F || strchr("%#?\"'<>&", *p)) snprintf(e, sizeof e, "%%%02X", *p);
            else e[0] = (char)*p, e[1] = 0;
        } else {
            out = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : *p == '"' ? "&quot;" : 0;
            if (!out) e[0] = (char)*p, e[1] = 0, out = e;
        }
        if (!html_put(buf, len, cap, out, strlen(out))) return 0;
    }
    return 1;
}

static int dirent_order(const void *a, const void *b) {
    const struct dirent *x = *(const struct dirent *const *)a, *y = *(const struct dirent *const *)b;
    if ((x->d_type == DT_DIR) != (y->d_type == DT_DIR)) return x->d_type == DT_DIR ? -1 : 1;
    return strcasecmp(x->d_name, y->d_name);
}

// A folder as a page of links, folders first: what a browser shows for a
// file: URL naming a directory.
static char *dir_page(const char *path, size_t *out_len) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    struct dirent **ents = NULL;
    size_t n = 0, cap_e = 0;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap_e) {
            cap_e = cap_e ? cap_e * 2 : 64;
            struct dirent **g = realloc(ents, cap_e * sizeof *ents);
            if (!g) break;
            ents = g;
        }
        ents[n] = malloc(sizeof **ents);
        if (!ents[n]) break;
        memcpy(ents[n++], e, sizeof **ents);
    }
    closedir(d);
    qsort(ents, n, sizeof *ents, dirent_order);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int ok = html_puts(&buf, &len, &cap, "<!doctype html><html><head><meta charset=\"utf-8\"><title>") &&
             html_put_escaped(&buf, &len, &cap, path, 0) &&
             html_puts(&buf, &len, &cap, "</title></head><body><h1>") &&
             html_put_escaped(&buf, &len, &cap, path, 0) &&
             html_puts(&buf, &len, &cap, "</h1><ul>");
    if (ok && strcmp(path, "/")) ok = html_puts(&buf, &len, &cap, "<li><a href=\"../\">..</a></li>");
    for (size_t i = 0; ok && i < n; i++) {
        int dir = ents[i]->d_type == DT_DIR;
        ok = html_puts(&buf, &len, &cap, "<li><a href=\"") &&
             html_put_escaped(&buf, &len, &cap, ents[i]->d_name, 1) &&
             html_puts(&buf, &len, &cap, dir ? "/\">" : "\">") &&
             html_put_escaped(&buf, &len, &cap, ents[i]->d_name, 0) &&
             html_puts(&buf, &len, &cap, dir ? "/</a></li>" : "</a></li>");
    }
    if (ok) ok = html_puts(&buf, &len, &cap, "</ul></body></html>");
    for (size_t i = 0; i < n; i++) free(ents[i]);
    free(ents);
    if (!ok) { free(buf); return NULL; }
    *out_len = len;
    return buf;
}

static void serve_file(struct uui_webview *v, uint32_t id, const char *url) {
    static char path[4096];
    if (!v->files) {
        ulogf("webview: refused %s to a web page (content %d)\n", url, v->pid);
        uui_webview_scheme_reply(v, id, 403, NULL, NULL, 0, 1);
        return;
    }
    struct stat st;
    if (ufileurl_to_path(url, path, sizeof path) != 0 || stat(path, &st) != 0) {
        uui_webview_scheme_reply(v, id, 404, NULL, NULL, 0, 1);
        return;
    }
    char *body = NULL;
    size_t len = 0;
    const char *mime = "text/html";
    if (S_ISDIR(st.st_mode)) {
        body = dir_page(path, &len);
    } else if ((uint64_t)st.st_size <= FILE_MAX) {
        FILE *f = fopen(path, "rb");
        body = f ? malloc((size_t)st.st_size + 1) : NULL;
        if (body) len = fread(body, 1, (size_t)st.st_size, f);
        if (f) fclose(f);
        mime = ufileurl_mime(path);
    }
    if (body) uui_webview_scheme_reply(v, id, 200, mime, body, (uint32_t)len, 1);
    else uui_webview_scheme_reply(v, id, 404, NULL, NULL, 0, 1);
    free(body);
}

// --- what the content process says ---------------------------------------------

static struct uui_webview *by_pid(int pid) {
    for (struct uui_webview *v = g_views; v; v = v->next)
        if (v->pid == pid) return v;
    return NULL;
}

static void copy_str(char *dst, size_t cap, const struct ubulk_msg *m, size_t skip, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, (const char *)m->data + skip, n);
    dst[n] = '\0';
}

static void on_buffer(struct uui_webview *v, const struct wv_buffer *b) {
    unmap_buf(v);
    char nm[32];
    snprintf(nm, sizeof nm, WEBVIEW_BUF_FMT, v->pid, b->gen);
    uint64_t bytes = ((uint64_t)b->w * (uint64_t)b->h * 4 + 4095) & ~4095ull;
    int fd = sys_shm_open(nm, 0, 0);
    if (fd < 0) return;
    // READ-ONLY, and exactly the claimed size: a content process that
    // lies about its buffer costs only its own view.
    void *p = sys_mmap(0, bytes, SYS_PROT_READ, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) return;
    v->buf = p;
    v->bbytes = bytes;
    v->bw = b->w;
    v->bh = b->h;
    v->gen = b->gen;
    if (v->cw != b->w || v->ch != b->h) {
        uint32_t *c = malloc((size_t)b->w * (size_t)b->h * 4);
        if (!c) return;
        for (size_t i = 0; i < (size_t)b->w * (size_t)b->h; i++) c[i] = 0xFFFFFFFFu;
        // What is kept of the old copy stays where it was until the first
        // frame of the new size lands.
        if (v->copy)
            for (int r = 0; r < b->h && r < v->ch; r++)
                memcpy(c + (size_t)r * (size_t)b->w, v->copy + (size_t)r * (size_t)v->cw,
                       (size_t)(b->w < v->cw ? b->w : v->cw) * 4);
        free(v->copy);
        v->copy = c;
        v->cw = b->w;
        v->ch = b->h;
    }
}

// The scroll first -- what was on screen moves -- then the rectangles,
// then the ack that lets the engine paint the next frame.
static void on_frame(struct uui_webview *v, const struct wv_frame *f) {
    if (v->copy && v->buf && f->gen == v->gen && v->cw == v->bw && v->ch == v->bh) {
        int dx = f->dx, dy = f->dy, w = v->cw, h = v->ch;
        if ((dx || dy) && abs(dx) < w && abs(dy) < h) {
            int cw = w - abs(dx), chh = h - abs(dy);
            int sx = dx > 0 ? dx : 0, tx = dx > 0 ? 0 : -dx;
            int sy = dy > 0 ? dy : 0, ty = dy > 0 ? 0 : -dy;
            if (dy > 0)
                for (int r = 0; r < chh; r++)
                    memmove(v->copy + (size_t)(ty + r) * (size_t)w + tx,
                            v->copy + (size_t)(sy + r) * (size_t)w + sx, (size_t)cw * 4);
            else
                for (int r = chh - 1; r >= 0; r--)
                    memmove(v->copy + (size_t)(ty + r) * (size_t)w + tx,
                            v->copy + (size_t)(sy + r) * (size_t)w + sx, (size_t)cw * 4);
        }
        for (int i = 0; i < f->nrects && i < WV_FRAME_RECTS; i++) {
            struct wv_rect r = f->r[i];
            if (r.x < 0) r.w += r.x, r.x = 0;
            if (r.y < 0) r.h += r.y, r.y = 0;
            if (r.x + r.w > w) r.w = w - r.x;
            if (r.y + r.h > h) r.h = h - r.y;
            for (int row = 0; row < r.h; row++)
                memcpy(v->copy + (size_t)(r.y + row) * (size_t)w + r.x,
                       v->buf + (size_t)(r.y + row) * (size_t)w + r.x, (size_t)r.w * 4);
        }
    }
    send_int(v, WV_FRAME_ACK, (int)f->seq);
}

static void on_hello(struct uui_webview *v) {
    v->ready = 1;
    send_size(v);
    send_int(v, WV_VISIBLE, v->visible);
    if (v->pending_nav) {
        send_rec(v, v->pending_upgraded ? WV_NAVIGATE_UPGRADED : WV_NAVIGATE, v->pending_nav,
                 (uint32_t)strlen(v->pending_nav));
        free(v->pending_nav);
        v->pending_nav = NULL;
    } else if (v->pending_go && v->nhist) {
        send_go(v);
    }
    v->pending_go = 0;
}

static void handle(const struct ubulk_msg *m) {
    struct uui_webview *v = by_pid(m->pid);
    if (!v) return;
    switch (m->type) {
    case WV_HELLO:  on_hello(v); break;
    case WV_BUFFER:
        if (m->len >= sizeof(struct wv_buffer)) on_buffer(v, m->data);
        break;
    case WV_FRAME:
        if (m->len >= sizeof(struct wv_frame)) on_frame(v, m->data);
        break;
    case WV_TITLE:
        copy_str(v->title, sizeof v->title, m, 0, m->len);
        if (v->nhist) {
            free(v->hist[v->hist_at].title);
            v->hist[v->hist_at].title = dup_or_empty(v->title);
        }
        changed(v, UUI_WV_TITLE);
        break;
    case WV_COMMIT: {
        struct wv_commit c;
        if (m->len < sizeof c) break;
        memcpy(&c, m->data, sizeof c);
        static char url[WEBVIEW_URL_MAX];
        copy_str(url, sizeof url, m, sizeof c, m->len - sizeof c);
        hist_commit(v, url, c.replace);
        changed(v, UUI_WV_STATE);
        break;
    }
    case WV_URL:
        copy_str(v->url, sizeof v->url, m, 0, m->len);
        v->nlinks = 0;   // a new page's links come after it
        changed(v, UUI_WV_URL);
        break;
    case WV_STATUS:
        copy_str(v->status, sizeof v->status, m, 0, m->len);
        changed(v, UUI_WV_STATUS);
        break;
    case WV_STATE:
        if (m->len >= sizeof v->st) {
            int was = v->st.pointer;
            memcpy(&v->st, m->data, sizeof v->st);
            // Only a page at rest says where it was left: a navigation
            // resets the scroll as it starts, before its commit arrives.
            if (!v->st.loading) {
                v->hist_live = 1;
                v->live_sx = v->st.sx;
                v->live_sy = v->st.sy;
            }
            // The shape arrives a moment after the motion that asked for
            // it, and the toolkit asks a widget only ON a motion: a
            // pointer held still over text would keep the arrow.
            if (v->st.pointer != was && v->hover_x >= 0 && v->visible && g_app)
                uapp_set_cursor(g_app, uui_webview_ops.cursor(v, v->hover_x, v->hover_y));
            changed(v, UUI_WV_STATE);
        }
        break;
    case WV_SITE:
        if (m->len >= sizeof v->site) {
            memcpy(&v->site, m->data, sizeof v->site);
            v->site.host[sizeof v->site.host - 1] = '\0';
            v->site.version[sizeof v->site.version - 1] = '\0';
            v->site.cipher[sizeof v->site.cipher - 1] = '\0';
            changed(v, UUI_WV_SITE);
        }
        break;
    case WV_FIND_RESULT:
        if (m->len >= sizeof v->find) {
            memcpy(&v->find, m->data, sizeof v->find);
            changed(v, UUI_WV_FIND);
        }
        break;
    case WV_DOWNGRADED: {
        char host[256];
        copy_str(host, sizeof host, m, 0, m->len);
        if (v->host && v->host->downgraded) v->host->downgraded(v, host);
        break;
    }
    case WV_OPEN_TAB:
    case WV_EXTERNAL: {
        static char url[WEBVIEW_URL_MAX];
        copy_str(url, sizeof url, m, 0, m->len);
        // A web page cannot reach a local file by opening it in a new
        // tab either: that tab would be started as a local page's.
        if (m->type == WV_OPEN_TAB && !v->files && local_page(url)) {
            ulogf("webview: refused to open %s from a web page\n", url);
            break;
        }
        if (m->type == WV_OPEN_TAB && v->host && v->host->open_tab) v->host->open_tab(v, url);
        if (m->type == WV_EXTERNAL && v->host && v->host->external) v->host->external(v, url);
        break;
    }
    case WV_VISITED: {
        struct wv_visited h;
        if (m->len < sizeof h) break;
        memcpy(&h, m->data, sizeof h);
        if (h.title_len < 0 || sizeof h + (size_t)h.title_len > m->len) break;
        static char title[256], url[WEBVIEW_URL_MAX];
        copy_str(title, sizeof title, m, sizeof h, (size_t)h.title_len);
        copy_str(url, sizeof url, m, sizeof h + (size_t)h.title_len, m->len - sizeof h - (size_t)h.title_len);
        if (v->host && v->host->visited) v->host->visited(v, url, title);
        break;
    }
    case WV_LINKS: {
        size_t at = 0;
        v->nlinks = 0;
        while (at + sizeof(struct wv_link) <= m->len && v->nlinks < WV_LINKS_MAX) {
            struct wv_link l;
            memcpy(&l, (const char *)m->data + at, sizeof l);
            at += sizeof l;
            if (l.href_len < 0 || at + (size_t)l.href_len > m->len) break;
            v->link[v->nlinks].x = l.x, v->link[v->nlinks].y = l.y;
            v->link[v->nlinks].w = l.w, v->link[v->nlinks].h = l.h;
            copy_str(v->link[v->nlinks].href, sizeof v->link[0].href, m, at, (size_t)l.href_len);
            at += (size_t)l.href_len;
            v->nlinks++;
        }
        changed(v, UUI_WV_LINKS);
        break;
    }
    case WV_SWAP: {
        // Only a local page's process has anything to leave behind.
        static char url[WEBVIEW_URL_MAX];
        copy_str(url, sizeof url, m, 0, m->len);
        if (v->files && !local_page(url)) uui_webview_navigate(v, url);
        break;
    }
    case WV_SCHEME_GET: {
        static char url[WEBVIEW_URL_MAX];
        copy_str(url, sizeof url, m, 0, m->len);
        if (!strncasecmp(url, "file:", 5)) serve_file(v, m->id, url);
        else if (v->host && v->host->scheme) v->host->scheme(v, m->id, url);
        else uui_webview_scheme_reply(v, m->id, 404, NULL, NULL, 0, 1);
        break;
    }
    }
}

// A content process ended. Deliberately (unloaded, closed) is nothing;
// otherwise the page STOPPED and the C3 view goes up over its last frame.
static void died(int pid) {
    struct uui_webview *v = by_pid(pid);
    if (!v) return;
    uui_webview_history_snapshot(v);
    v->pid = 0;
    v->ready = 0;
    unmap_buf(v);
    v->stopped = 1;
    char path[96];
    snprintf(path, sizeof path, REPORT_FMT, pid);
    struct stat st;
    v->report[0] = '\0';
    if (stat(path, &st) == 0) snprintf(v->report, sizeof v->report, "%s", path);
    ulogf("webview: content %d stopped%s%s\n", pid, v->report[0] ? ", report " : "", v->report);
    changed(v, UUI_WV_STOPPED);
}

int uui_webview_on_post(struct uapp *a, int a0, int a1) {
    (void)a;
    (void)a1;
    if (!g_open || a0 != g_post_id) return 0;
    g_posted = 0;
    int gone[UBULK_PEERS];
    ubulk_server_poll(&g_srv, gone, UBULK_PEERS);
    static char buf[UBULK_REC_MAX];
    struct ubulk_msg m;
    while (ubulk_server_recv(&g_srv, &m, buf, sizeof buf)) handle(&m);
    int deaths[DEATHS], n;
    pthread_mutex_lock(&g_death_mu);
    n = g_ndeath;
    memcpy(deaths, g_death, (size_t)n * sizeof deaths[0]);
    g_ndeath = 0;
    pthread_mutex_unlock(&g_death_mu);
    for (int i = 0; i < n; i++) died(deaths[i]);
    return 1;
}

// --- a document page ----------------------------------------------------------

static void hex(char *out, uint32_t c) { sprintf(out, "#%06x", (unsigned)(c & 0xFFFFFF)); }

char *uui_webview_markdown_page(const char *title, const char *md, int md_len,
                                const struct umd_html_opts *opts, int *len) {
    const struct utheme *t = utheme_current();
    char text[8], muted[8], bg[8], rule[8], link[8], code[8], panel[8];
    hex(text, t->text);
    hex(muted, t->text_muted);
    hex(bg, t->field_bg);
    hex(rule, t->separator);
    hex(link, utheme_action(UTHEME_ACT_NAV));
    hex(code, ugfx_blend(t->field_bg, t->text, 7));
    hex(panel, t->panel_bg);
    int cap = md_len * 2 + 8192;
    char *page = malloc((size_t)cap);
    if (!page) return NULL;
    struct umd_out out = { page, cap, 0, 0 };
    // THE PAGE'S TEXT IS THE DESKTOP'S SIZE, and its headings scale with
    // it: a fixed 15px read smaller than the window around it on a
    // machine whose font is larger.
    int px = ugfx_font_session_px();
    if (px < 8) px = 8;
    int n = snprintf(page, (size_t)cap,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title><style>"
        "body{margin:0;background:%s;color:%s;font-family:sans-serif;font-size:%dpx;line-height:1.55}"
        "main{max-width:40em;padding:18px 30px 30px}"
        "h1{font-family:serif;font-size:2em;font-weight:normal;line-height:1.2;"
        "border-bottom:1px solid %s;padding-bottom:6px;margin:0 0 12px}"
        "h2{font-family:serif;font-size:1.4em;font-weight:normal;margin:20px 0 6px}"
        "h3{font-size:1.07em;margin:16px 0 4px}"
        "a{color:%s}"
        "code,pre{font-family:monospace;font-size:0.87em;background:%s}"
        "pre{padding:8px 10px;line-height:1.4}pre code{font-size:1em}"
        "table{border-collapse:collapse;margin:8px 0}"
        "th,td{border:1px solid %s;padding:3px 8px;text-align:left;vertical-align:top}"
        "th{background:%s}"
        "p.fold{background:%s;border:1px solid %s;padding:8px 12px}"
        "</style></head><body><main>",
        title ? title : "", bg, text, px, rule, link, code, rule, panel, panel, rule);
    (void)muted;
    out.len = n < cap ? n : cap - 1;
    umd_html(md, md_len, opts, &out);
    const char *end = "</main></body></html>\n";
    if (out.len + (int)strlen(end) < cap) {
        memcpy(page + out.len, end, strlen(end) + 1);
        out.len += (int)strlen(end);
    }
    *len = out.len;
    return page;
}

// --- the widget --------------------------------------------------------------

static int bar_h(const struct uui_webview *v) {
    if (!v->stopped) return 0;
    int w, h;
    uui_infobar_ops.natural_size(&v->bar, &w, &h);
    return h;
}

static void place(struct uui_webview *v) {
    uui_sbar_place(&v->sb, v->x + v->w - uui_sbar_width(), v->y, v->h);
    uui_sbar_set(&v->sb, v->st.ext_h, v->h, v->st.sy);
    v->sb.step = ugfx_char_h() * 3;
    if (v->stopped) uui_infobar_ops.set_geometry(&v->bar, v->x, v->y, v->w, bar_h(v));
}

static void ops_natural_size(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = ugfx_char_advance('n') * 40;
    *oh = ugfx_char_h() * 10;
}

static void ops_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_webview *v = w;
    v->x = x;
    v->y = y;
    v->w = width;
    v->h = height;
    place(v);
    send_size(v);
}

static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_webview *v = w;
    *x = v->x, *y = v->y, *ow = v->w, *oh = v->h;
}

// The last frame of a stopped page: grey, and lifted toward white, so it
// reads as a picture of the page rather than the page.
static uint32_t dimmed(uint32_t c) {
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    int grey = (r * 3 + g * 6 + b) / 10;
    grey += (255 - grey) * 55 / 100;
    return ugfx_rgb(grey, grey, grey);
}

// A hovered link's address in the view's bottom-left corner, over the
// page -- where every browser puts it, and gone when the pointer is.
static void draw_status(struct ugfx_surface *s, const struct uui_webview *v) {
    int pad = ugfx_char_advance('n') / 2 + 2;
    int h = ugfx_char_h() + 6;
    int max = v->w * 2 / 3;
    int w = ugfx_text_width(v->status) + 2 * pad;
    if (w > max) w = max;
    int x = v->x, y = v->y + v->h - h;
    ugfx_fill_rect(s, x, y, w, h, UTHEME_PANEL_BG);
    ugfx_draw_rect(s, x - 1, y, w + 1, h + 1, UTHEME_OUTLINE);
    ugfx_draw_string_elided(s, x + pad, y + 3, w - 2 * pad, v->status, UTHEME_TEXT, UTHEME_PANEL_BG);
}

static void ops_draw(struct ugfx_surface *s, const void *w) {
    struct uui_webview *v = (struct uui_webview *)w;
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, v->x, v->y, v->w, v->h);
    place(v);
    int top = bar_h(v), pw = page_w(v);
    ugfx_fill_rect(s, v->x, v->y, v->w, v->h, ugfx_rgb(255, 255, 255));
    if (v->copy) {
        int rows = v->ch < v->h - top ? v->ch : v->h - top, cols = v->cw < pw ? v->cw : pw;
        if (!v->stopped) {
            ugfx_blit(s, v->x, v->y, cols, rows, v->copy, v->cw);
        } else {
            for (int r = 0; r < rows; r++) {
                const uint32_t *src = v->copy + (size_t)r * (size_t)v->cw;
                for (int c = 0; c < cols; c++) ugfx_put_pixel(s, v->x + c, v->y + top + r, dimmed(src[c]));
            }
        }
    }
    if (!v->stopped) {
        if (v->st.caret_h && v->focused)
            ugfx_fill_rect(s, v->x + v->st.caret_x, v->y + v->st.caret_y, 1, v->st.caret_h,
                           ugfx_rgb(0, 0, 0));
        uui_sbar_draw(&v->sb, s, ugfx_rgb(255, 255, 255), UTHEME_TEXT);
        if (v->status[0] && v->hover_x >= 0) draw_status(s, v);
    } else {
        v->bar.second.disabled = !v->report[0];
        uui_infobar_ops.draw(s, &v->bar);
    }
    ugfx_clip_restore(s, &saved);
}

static int inside(const struct uui_webview *v, int cx, int cy) {
    return cx >= v->x && cy >= v->y && cx < v->x + v->w && cy < v->y + v->h;
}

static int ops_hit(const void *w, int cx, int cy) { return inside(w, cx, cy); }

static void send_mouse(struct uui_webview *v, int kind, int cx, int cy, unsigned mods, unsigned buttons) {
    struct wv_mouse m = { kind, cx - v->x, cy - v->y, mods, buttons };
    send_rec(v, WV_MOUSE, &m, sizeof m);
}

static int bar_result(struct uui_webview *v, int r) {
    if (r & UUI_SBAR_MOVED) {
        struct wv_point p = { v->st.sx, v->sb.top };
        send_rec(v, WV_SCROLL_TO, &p, sizeof p);
    }
    return r != 0;
}

static int ops_press(void *w, int cx, int cy, unsigned mods) {
    struct uui_webview *v = w;
    if (!inside(v, cx, cy)) return 0;
    if (v->stopped) {
        v->bar_pressed = cy < v->y + bar_h(v);
        if (v->bar_pressed) uui_infobar_ops.press(&v->bar, cx, cy, mods);
        return 1;
    }
    place(v);
    if (uui_sbar_hit(&v->sb, cx, cy)) return bar_result(v, uui_sbar_press(&v->sb, cx, cy)) | 1;
    v->pressed = 1;
    send_mouse(v, WV_MOUSE_PRESS, cx, cy, mods, 1);
    return 1;
}

static int ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_webview *v = w;
    if (v->stopped) return 0;
    if (v->sb.grab >= 0) return bar_result(v, uui_sbar_motion(&v->sb, cx, cy, buttons));
    int changed_ = 0;
    if (inside(v, cx, cy) || v->pressed) {
        changed_ |= bar_result(v, uui_sbar_motion(&v->sb, cx, cy, buttons));
        v->hover_x = cx;
        v->hover_y = cy;
        send_mouse(v, WV_MOUSE_MOVE, cx, cy, 0, buttons);
    } else if (v->hover_x >= 0) {
        v->hover_x = v->hover_y = -1;
        uui_sbar_leave(&v->sb);
        send_mouse(v, WV_MOUSE_LEAVE, cx, cy, 0, 0);
        changed_ = 1;   // the status overlay goes with the pointer
    }
    return changed_;
}

static int ops_release(void *w, int cx, int cy) {
    struct uui_webview *v = w;
    if (v->stopped) {
        if (v->bar_pressed) {
            uui_infobar_ops.release(&v->bar, cx, cy);
            switch (uui_infobar_take(&v->bar)) {
            case 1: uui_webview_reload(v); break;
            case 2:
                if (v->report[0]) {
                    char args[128];
                    snprintf(args, sizeof args, "--report %s", v->report);
                    sys_spawn("/bin/wm/apps/crashreports", args, -1);
                }
                break;
            }
        }
        v->bar_pressed = 0;
        return 1;
    }
    if (v->sb.grab >= 0) return bar_result(v, uui_sbar_release(&v->sb)) | 1;
    if (!v->pressed) return 0;
    v->pressed = 0;
    send_mouse(v, WV_MOUSE_RELEASE, cx, cy, 0, 0);
    return 1;
}

static int ops_wheel(void *w, int notches) {
    struct uui_webview *v = w;
    if (v->stopped) return 0;
    struct wv_wheel wh = { v->hover_x >= 0 ? v->hover_x - v->x : -1,
                           v->hover_y >= 0 ? v->hover_y - v->y : -1, notches };
    send_rec(v, WV_WHEEL, &wh, sizeof wh);
    return 1;
}

// The keys a PAGE may want -- typing in a form field, moving through a
// selection, scrolling. The rest stay the app's (Ctrl+T, F5, Alt+Left),
// as Chrome's browser process keeps its shortcuts. Esc goes to the page
// AND on to the app, which closes its own find bar with it.
static int page_key(int key, unsigned mods) {
    if (mods & KEY_MOD_ALT) return 0;
    if (mods & KEY_MOD_CTRL)
        return key == 1 || key == 3 || key == 22 || key == 24 || key == 25 || key == 26 ||
               key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT || key == KEY_HOME || key == KEY_END;
    switch (key) {
    case '\b': case '\n': case '\r': case ' ':
    case KEY_DELETE: case KEY_ARROW_LEFT: case KEY_ARROW_RIGHT: case KEY_ARROW_UP:
    case KEY_ARROW_DOWN: case KEY_HOME: case KEY_END: case KEY_PAGE_UP: case KEY_PAGE_DOWN:
        return 1;
    }
    return IS_PRINTABLE_KEY(key);
}

static int ops_key(void *w, int key, unsigned mods) {
    struct uui_webview *v = w;
    if (v->stopped || !v->ready) return 0;
    if (key == 0x1B) {
        struct wv_key k = { key, mods };
        send_rec(v, WV_KEY, &k, sizeof k);
        return 0;
    }
    if (!page_key(key, mods)) return 0;
    struct wv_key k = { key, mods };
    send_rec(v, WV_KEY, &k, sizeof k);
    return 1;
}

// For a test: which process draws the page, whether it stopped, and the
// stopped bar's buttons (View report only when there is a report).
static void ops_describe(const void *w, const struct uui_describe *d) {
    const struct uui_webview *v = w;
    uui_describe_int(d, "pid", v->pid);
    uui_describe_int(d, "stopped", v->stopped);
    char url[96];
    snprintf(url, sizeof url, "%s", v->url[0] ? v->url : "-");
    uui_describe_str(d, "url", url);
    // The links in view: "link i x y w h", then "href.i <address>".
    for (int i = 0; i < v->nlinks && !v->stopped; i++) {
        char part[16];
        uui_describe_rect_i(d, "link", i, v->x + v->link[i].x, v->y + v->link[i].y, v->link[i].w,
                            v->link[i].h);
        snprintf(part, sizeof part, "href.%d", i);
        uui_describe_str(d, part, v->link[i].href);
    }
    if (!v->stopped) return;
    uui_describe_rect(d, "bar", v->bar.x, v->bar.y, v->bar.w, v->bar.h);
    const struct uui_button *b = &v->bar.action;
    uui_describe_rect(d, "reload", b->x, b->y, b->w, b->h);
    b = &v->bar.second;
    if (v->report[0]) uui_describe_rect(d, "report", b->x, b->y, b->w, b->h);
}

static int ops_accepts_focus(const void *w) { (void)w; return 1; }
static void ops_set_focused(void *w, int focused) { ((struct uui_webview *)w)->focused = focused; }

static int ops_cursor(const void *w, int cx, int cy) {
    const struct uui_webview *v = w;
    if (v->stopped || uui_sbar_hit(&v->sb, cx, cy)) return WIN_CURSOR_DEFAULT;
    return v->st.pointer ? v->st.pointer : WIN_CURSOR_DEFAULT;
}

const struct uui_widget_ops uui_webview_ops = {
    .natural_size = ops_natural_size,
    .set_geometry = ops_set_geometry,
    .bounds = ops_bounds,
    .draw = ops_draw,
    .hit = ops_hit,
    .press = ops_press,
    .motion = ops_motion,
    .release = ops_release,
    .wheel = ops_wheel,
    .key = ops_key,
    .accepts_focus = ops_accepts_focus,
    .set_focused = ops_set_focused,
    .cursor = ops_cursor,
    .describe = ops_describe,
};
