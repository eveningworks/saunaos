// nsmonkey -- NetSurf's headless "monkey" front end, on toy-os.
//
// Commands in on stdin, one per line ("WINDOW NEW file:///a.html",
// "WINDOW REDRAW 0", "QUIT"), and everything the engine does out on
// stdout as text ("PLOT TEXT X 8 Y 52 STR Hello"). It is stage 1's test
// oracle (docs/browser-design.md): the same pinned NetSurf built on the
// host prints the same lines, so a difference is a port or libc bug.
//
// THIS FILE REPLACES upstream's frontends/monkey/main.c, which is not
// vendored: its loop waits in select() on stdin and the scheduler's next
// deadline at once, and toy-os cannot wait on several things. Instead a
// thread reads stdin a line at a time and hands each line on through a
// pipe the dispatcher's own fgets() then reads without blocking, and the
// main loop runs the scheduler, polling every POLL_MS while idle
// (upstream prints a "POLL" line per wait; this loop does not, so a
// comparison with the host's output drops those). Every
// other part of monkey -- the window, fetch, bitmap, layout and plot
// tables, the command dispatcher -- is upstream's, unchanged. http: and
// https: go through ours (userland/backends/netsurf/fetch_http.c), where
// the host build has curl; `--weak-entropy` lets https key a connection
// on a guest with no virtio-rng, as wget's flag does.
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils/filepath.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "netsurf/misc.h"
#include "netsurf/netsurf.h"
#include "netsurf/url_db.h"
#include "netsurf/cookie_db.h"
#include "monkey/output.h"
#include "monkey/dispatch.h"
#include "monkey/browser.h"
#include "monkey/401login.h"
#include "monkey/filetype.h"
#include "monkey/fetch.h"
#include "monkey/schedule.h"
#include "monkey/bitmap.h"
#include "monkey/layout.h"
#include "backends/netsurf/fetch_http.h"

// Where NetSurf's resources (Messages, default.css, mime.types) live on
// the image, then a directory named by $NETSURFRES for a test's own.
#define NSMONKEY_RESPATH "${NETSURFRES}:/usr/share/netsurf/"

// How long the loop sleeps when the scheduler has nothing due sooner --
// the latency a command on stdin can wait before it is read.
#define POLL_MS 10

// monkey/fetch.c looks resources up through this; upstream's main.c
// defines it, so this one does.
char **respaths;

static volatile int g_done;
static volatile int g_lines;        // lines queued in the pipe, not yet dispatched
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_pipe_w = -1;

static void quit_handler(int argc, char **argv) {
    (void)argc;
    (void)argv;
    g_done = 1;
}

static void options_handler(int argc, char **argv) {
    nsoption_commandline(&argc, argv, nsoptions);
}

static void monkey_quit(void) {
    urldb_save_cookies(nsoption_charp(cookie_jar));
    urldb_save(nsoption_charp(url_file));
    monkey_fetch_filetype_fin();
}

static nserror launch_url(struct nsurl *url) {
    moutf(MOUT_GENERIC, "LAUNCH URL %s", nsurl_access(url));
    return NSERROR_OK;
}

static nserror present_cookies(const char *search_term) {
    if (search_term) moutf(MOUT_GENERIC, "PRESENT_COOKIES %s", search_term);
    else moutf(MOUT_GENERIC, "PRESENT_COOKIES");
    return NSERROR_OK;
}

static struct gui_misc_table misc_table = {
    .schedule = monkey_schedule,
    .quit = monkey_quit,
    .launch_url = launch_url,
    .login = gui_401login_open,
    .present_cookies = present_cookies,
};

static nserror set_defaults(struct nsoption_s *defaults) {
    (void)defaults;
    // In memory only: a test run must not leave state for the next one.
    nsoption_setnull_charp(cookie_file, strdup(""));
    nsoption_setnull_charp(cookie_jar, strdup(""));
    nsoption_setnull_charp(url_file, strdup(""));
    return NSERROR_OK;
}

// Reads the REAL stdin (moved to `fd` before fd 0 became the pipe) and
// forwards whole lines. End of input forwards a QUIT, as upstream's
// dispatcher does when its own fgets() fails.
static void *reader(void *arg) {
    FILE *in = fdopen((int)(long)arg, "r");
    static char line[PATH_MAX];   // static: PATH_MAX is twice the ring-3 frame budget
    for (;;) {
        int eof = !in || !fgets(line, sizeof line, in);
        if (eof) strcpy(line, "QUIT\n");
        size_t n = strlen(line);
        if (n == 0 || line[n - 1] != '\n') {   // a final line with no newline
            if (n + 1 < sizeof line) { line[n++] = '\n'; line[n] = '\0'; }
        }
        if (write(g_pipe_w, line, n) != (long)n) eof = 1;
        pthread_mutex_lock(&g_lock);
        g_lines++;
        pthread_mutex_unlock(&g_lock);
        if (eof) return 0;
    }
}

static int take_line(void) {
    pthread_mutex_lock(&g_lock);
    int had = g_lines > 0;
    if (had) g_lines--;
    pthread_mutex_unlock(&g_lock);
    return had;
}

static void die(const char *why) {
    moutf(MOUT_DIE, "%s", why);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    struct netsurf_table table = {
        .misc = &misc_table,
        .window = monkey_window_table,
        .download = monkey_download_table,
        .fetch = monkey_fetch_table,
        .bitmap = monkey_bitmap_table,
        .layout = monkey_layout_table,
    };
    if (netsurf_register(&table) != NSERROR_OK) die("NetSurf operation table failed registration");

    // fd 0 becomes the pipe the dispatcher reads; the real stdin moves
    // to a new fd for the reader thread.
    int fds[2];
    int real_in = dup(0);
    if (real_in < 0 || pipe(fds) != 0 || dup2(fds[0], 0) < 0) die("cannot set up the command pipe");
    close(fds[0]);
    g_pipe_w = fds[1];
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    static const char *const langv[] = { "en", "", NULL };
    char **pathv = filepath_path_to_strvec(NSMONKEY_RESPATH);
    respaths = filepath_generate(pathv, langv);
    filepath_free_strvec(pathv);

    // Before NetSurf's own option parsing, which consumes "--" words.
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--weak-entropy")) ns_fetch_http_allow_weak_entropy(1);
    nslog_init(NULL, &argc, argv);
    if (nsoption_init(set_defaults, &nsoptions, &nsoptions_default) != NSERROR_OK)
        die("Options failed to initialise");
    nsoption_commandline(&argc, argv, nsoptions);

    char *messages = filepath_find(respaths, "Messages");
    if (messages_add_from_file(messages) != NSERROR_OK) NSLOG(netsurf, INFO, "Messages failed to load");
    if (netsurf_init(NULL) != NSERROR_OK) die("NetSurf failed to initialise");
    free(messages);
    if (ns_fetch_http_register() != 0) die("the http fetcher failed to register");

    static char buf[PATH_MAX];
    filepath_sfinddef(respaths, buf, "mime.types", "/etc/");
    monkey_fetch_filetype_init(buf);

    if (monkey_register_handler("QUIT", quit_handler) != NSERROR_OK ||
        monkey_register_handler("WINDOW", monkey_window_handle_command) != NSERROR_OK ||
        monkey_register_handler("OPTIONS", options_handler) != NSERROR_OK ||
        monkey_register_handler("LOGIN", monkey_login_handle_command) != NSERROR_OK)
        die("command handlers failed to register");

    pthread_t t;
    if (pthread_create(&t, NULL, reader, (void *)(long)real_in) != 0) die("cannot start the reader");
    pthread_detach(t);

    moutf(MOUT_GENERIC, "STARTED");
    while (!g_done) {
        int due = monkey_schedule_run();
        if (take_line()) {
            monkey_process_command();
            continue;
        }
        if (due == 0) continue;
        usleep((unsigned long)((due < 0 || due > POLL_MS) ? POLL_MS : due) * 1000);
    }

    moutf(MOUT_GENERIC, "CLOSING_DOWN");
    monkey_kill_browser_windows();
    netsurf_exit();
    moutf(MOUT_GENERIC, "FINISHED");
    for (char **s = respaths; *s; s++) free(*s);
    free(respaths);
    nsoption_finalise(nsoptions, nsoptions_default);
    nslog_finalise();
    monkey_free_handlers();
    return 0;
}
