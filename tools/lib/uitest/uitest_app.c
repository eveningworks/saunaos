// A whole app on the host, against a stand-in compositor -- see
// uitest_app.h. The protocol answers here are toywm's (wm_client.c),
// reduced to one client and one window; a request it does not know is
// dropped. A CALL it does not know goes unanswered and times out once the
// clock passes the caller's deadline, which uapp reads as the refusal an
// older server gives.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uitest_app.h"
#include "uitest_host.h"
#include "base/uchan.h"
#include "base/uwmchan.h"
#include "keyboard.h"
#include "rt/sys.h"
#include "ui/uui_anim.h"
#include "win_proto.h"

extern int (*uitest_futex_hook)(volatile uint32_t *word, uint32_t expected, int timeout_ms);
extern int (*uitest_sleep_hook)(int ms);
extern const char *uitest_case;

struct widget {
    char name[WIN_TITLE_LEN];
    int x, y, w, h;
};

static struct {
    struct uchan_server srv;
    int pid;                        // the app's, once it has spoken
    int argc;
    char **argv;
    int status;
    // Where the app is parked: the word, what it expects, for how long.
    // A NULL word is a park that wants serving and resuming at once (a
    // full ring, sys_sleep_ms).
    volatile uint32_t *word;
    uint32_t expected;
    int timeout_ms;                 // 0: no deadline
    uint64_t parked_ns;             // the clock when it parked
    // The window: its last presented frame and what it said about itself.
    int w, h;
    uint32_t *frame;
    int frames;
    int front, front_gen;
    uint32_t front_seq;
    char title[WIN_TITLE_LEN];
    struct widget *widgets;
    int nwidgets, cap;
} g;

static void fail(const char *what) {
    if (uitest_report) uitest_report(0, what);
    else fprintf(stderr, "uitest_app: FAIL %s\n", what);
}

// --- the app's side: parking --------------------------------------------------

static int futex_hook(volatile uint32_t *word, uint32_t expected, int timeout_ms) {
    if (!uh_coro_in_app()) return -1;
    g.word = word;
    g.expected = expected;
    g.timeout_ms = timeout_ms;
    g.parked_ns = uitest_now_ns();
    uh_coro_yield();
    if (*word != expected) return 0;
    return -1;   // the timeout: the clock was moved past it
}

static int sleep_hook(int ms) {
    if (!uh_coro_in_app()) return 0;
    g.word = NULL;
    g.timeout_ms = ms;
    uh_coro_yield();
    return 1;
}

static void entry(void) { g.status = uitest_app_main(g.argc, g.argv); }

// --- the compositor's side ------------------------------------------------------

static void send_event(uint32_t type, int a, int b, unsigned mods) {
    if (!g.pid) return;
    struct win_event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.window = 0;
    e.a = a;
    e.b = b;
    e.mods = mods;
    if (uchan_server_send(&g.srv, g.pid, &e, sizeof e) < 0) fail("the app's inbox has room for an event");
}

static void reply(int pid, int a, int b, int c) {
    struct wmchan_msg r;
    memset(&r, 0, sizeof r);
    r.a = a;
    r.b = b;
    r.c = c;
    uchan_server_reply(&g.srv, pid, &r, sizeof r);
}

static void take_present(int pid, const struct wmchan_msg *m) {
    if (m->window != 0) return;   // a popup surface: none are granted
    int buf = WIN_PRESENT_BUF(m->a), gen = (int)WIN_PRESENT_GEN(m->a);
    int w = WIN_PRESENT_W(m->b), h = WIN_PRESENT_H(m->b);
    char name[WIN_BUF_NAME_MAX];
    snprintf(name, sizeof name, WIN_BUF_NAME_FMT, pid, (int)m->window, buf);
    int fd = sys_shm_open_from(name, pid);
    uint32_t *px = fd >= 0 ? sys_mmap(0, (uint64_t)w * (uint64_t)h * 4, SYS_PROT_READ, SYS_MAP_SHARED, fd, 0)
                           : (void *)-1;
    if (fd >= 0) sys_close(fd);
    if (px == (void *)-1) { fail("a presented buffer maps"); return; }
    if (w != g.w || h != g.h || !g.frame) {
        free(g.frame);
        g.frame = malloc((size_t)w * (size_t)h * 4);
        if (!g.frame) { fail("the frame fits in memory"); return; }
        g.w = w;
        g.h = h;
    }
    memcpy(g.frame, px, (size_t)w * (size_t)h * 4);
    g.frames++;
    // A FRAME TAKES A FRAME'S TIME. The release below wakes the app at
    // once, so without this an animation's clock never moved and it drew
    // the same first step for ever, a present per resume.
    uitest_advance_ms(UUI_ANIM_FRAME_MS);
    // The buffer that WAS the front can be drawn into again: wl_buffer.release.
    if (g.front >= 0 && g.front != buf)
        send_event(WIN_EV_BUF_RELEASE, g.front, g.front_gen, g.front_seq);
    g.front = buf;
    g.front_gen = gen;
    g.front_seq = (uint32_t)m->c;
}

static void take_widget(const struct wmchan_msg *m) {
    if (g.nwidgets == g.cap) {
        int cap = g.cap ? g.cap * 2 : 32;
        struct widget *grown = realloc(g.widgets, (size_t)cap * sizeof *grown);
        if (!grown) return;
        g.widgets = grown;
        g.cap = cap;
    }
    struct widget *wd = &g.widgets[g.nwidgets++];
    strlcpy(wd->name, m->text, sizeof wd->name);
    wd->x = m->a;
    wd->y = m->b;
    wd->w = WIN_WIDGET_W(m->c);
    wd->h = WIN_WIDGET_H(m->c);
}

static void handle(int pid, const struct wmchan_msg *m) {
    switch (m->type) {
    case WIN_REQ_CREATE:
        g.pid = pid;
        reply(pid, (int)m->window, 0, 0);   // the slot it proposed
        send_event(WIN_EV_FOCUS, 1, 0, 0);
        break;
    case WIN_REQ_PRESENT:      take_present(pid, m); break;
    case WIN_REQ_WIDGET_RESET: if (m->window == 0) g.nwidgets = 0; break;
    case WIN_REQ_WIDGET:       if (m->window == 0) take_widget(m); break;
    case WIN_REQ_TITLE:        strlcpy(g.title, m->text, sizeof g.title); break;
    // The calls this stand-in refuses; uapp has a path for each refusal
    // (a popup drawn in the window, no dialog, no notice).
    case WIN_REQ_POPUP: case WIN_REQ_DIALOG: case WIN_REQ_NOTICE: case WIN_REQ_NOTIFY:
    case WIN_REQ_CURSOR_IMAGE: case WIN_REQ_LAUNCH: case WIN_REQ_ACTIVATE:
        reply(pid, -1, 0, 0);
        break;
    default:
        break;   // hints, timers, cursors, sync answers: nothing to do here
    }
}

static void serve(void) {
    int gone[8];
    uchan_server_scan(&g.srv, gone, 8);
    struct wmchan_msg m;
    int pid;
    while ((pid = uchan_server_recv(&g.srv, &m, sizeof m)) > 0) handle(pid, &m);
}

// A park whose deadline the clock has moved past: the wait timed out.
static int deadline_passed(void) {
    return g.timeout_ms > 0 && uitest_now_ns() >= g.parked_ns + (uint64_t)g.timeout_ms * 1000000ull;
}

// Resume the app until it parks with nothing changed by serving it and
// its deadline still ahead.
static void run(void) {
    for (int spins = 0; !uh_coro_done(); spins++) {
        if (spins > 5000) { fail("the app parks (it ran 5000 resumes without one)"); return; }
        uh_coro_resume();
        if (uh_coro_done()) return;
        serve();
        if (!g.word) continue;                        // a sleep: served, go on
        if (*g.word != g.expected) continue;          // a reply or an event: wake it
        if (deadline_passed()) continue;              // the clock moved past it: a timeout
        return;                                       // parked, and nothing for it
    }
}

// --- the test's side ---------------------------------------------------------------

int uitest_app_settle(void) {
    run();
    // A SHORT PARK IS AN ANIMATION OR A FRAME OWED, not idleness: the clock
    // moves to it and the app goes on, as uapp's loop would see time pass.
    // A long one (the 500 ms liveness park, a caret's blink) is idle.
    for (int i = 0; !uh_coro_done() && g.timeout_ms > 0 && g.timeout_ms <= 100; i++) {
        if (i > 2000) { fail("the app's animations settle"); break; }
        uitest_advance_ms(g.timeout_ms);
        run();
    }
    return !uh_coro_done();
}

// The hooks out and the channel closed: after the app has ended, or when
// it never got going, so nothing after it parks into a coroutine that is
// not there.
static void teardown(void) {
    uitest_futex_hook = NULL;
    uitest_sleep_hook = NULL;
    uchan_server_close(&g.srv);
}

int uitest_app_start(int argc, char **argv) {
    memset(&g, 0, sizeof g);
    g.front = -1;
    g.argc = argc;
    g.argv = argv;
    if (uchan_server_open(&g.srv, WMCHAN_SERVICE) < 0) { fail("the stand-in compositor opens its channel"); return 0; }
    uitest_futex_hook = futex_hook;
    uitest_sleep_hook = sleep_hook;
    if (uh_coro_start(entry, 8u << 20) < 0) { fail("the app's stack is made"); teardown(); return 0; }
    uitest_app_settle();
    if (uh_coro_done()) {
        char what[96];
        snprintf(what, sizeof what, "the app shows a window (its main returned %d first)", g.status);
        fail(what);
        teardown();
        return 0;
    }
    if (!g.frames) fail("the app presents a first frame");
    return g.frames > 0;
}

int uitest_app_w(void) { return g.w; }
int uitest_app_h(void) { return g.h; }
int uitest_app_frames(void) { return g.frames; }
const char *uitest_app_title(void) { return g.title; }

uint32_t uitest_app_pixel(int x, int y) {
    if (!g.frame || x < 0 || y < 0 || x >= g.w || y >= g.h) {
        char what[96];
        snprintf(what, sizeof what, "pixel (%d,%d) is inside the %dx%d frame", x, y, g.w, g.h);
        fail(what);
        return 0;
    }
    return g.frame[(size_t)y * (size_t)g.w + (size_t)x] & 0x00FFFFFFu;
}

int uitest_app_widget(const char *name, int *x, int *y, int *w, int *h) {
    for (int i = g.nwidgets - 1; i >= 0; i--) {
        if (strcmp(g.widgets[i].name, name)) continue;
        *x = g.widgets[i].x; *y = g.widgets[i].y; *w = g.widgets[i].w; *h = g.widgets[i].h;
        return 1;
    }
    char what[160];
    snprintf(what, sizeof what, "the app's widget map names \"%s\"", name);
    fail(what);
    *x = *y = *w = *h = 0;
    return 0;
}

int uitest_app_has_widget(const char *name) {
    for (int i = 0; i < g.nwidgets; i++)
        if (!strcmp(g.widgets[i].name, name)) return 1;
    return 0;
}

void uitest_app_move_to(int x, int y, unsigned buttons) {
    send_event(WIN_EV_MOUSE_MOVE, x, y, buttons);
    uitest_advance_ms(10);
    uitest_app_settle();
}

void uitest_app_press_at(int x, int y) {
    uitest_advance_ms(1000);   // never a double click by accident (uitest.h)
    send_event(WIN_EV_MOUSE_MOVE, x, y, 0);
    send_event(WIN_EV_MOUSE_DOWN, x, y, 1);
    uitest_app_settle();
}

void uitest_app_release_at(int x, int y) {
    uitest_advance_ms(10);
    send_event(WIN_EV_MOUSE_UP, x, y, 0);
    uitest_app_settle();
}

void uitest_app_click_at(int x, int y) {
    uitest_app_press_at(x, y);
    uitest_app_release_at(x, y);
}

void uitest_app_rclick_at(int x, int y) {
    uitest_advance_ms(1000);
    send_event(WIN_EV_MOUSE_MOVE, x, y, 0);
    send_event(WIN_EV_MOUSE_DOWN, x, y, 2);   // the secondary button
    uitest_app_settle();
    uitest_advance_ms(10);
    send_event(WIN_EV_MOUSE_UP, x, y, 0);
    uitest_app_settle();
}

void uitest_app_click(const char *name) {
    int x, y, w, h;
    if (uitest_app_widget(name, &x, &y, &w, &h)) uitest_app_click_at(x + w / 2, y + h / 2);
}

void uitest_app_key(int key, unsigned mods) {
    send_event(WIN_EV_KEY, key, 0, mods);
    send_event(WIN_EV_KEY_UP, key, 0, mods);
    uitest_advance_ms(10);
    uitest_app_settle();
}

void uitest_app_type(const char *text) {
    for (const char *p = text; *p; p++) uitest_app_key((unsigned char)*p, 0);
}

void uitest_app_tick(int n) {
    for (int i = 0; i < n; i++) {
        send_event(WIN_EV_TIMER, 0, 0, 0);
        uitest_app_settle();
    }
}

int uitest_app_close(void) {
    send_event(WIN_EV_CLOSE, 0, 0, 0);
    uitest_app_settle();
    int done = uh_coro_done();
    if (!done) fail("the app exits when its window is asked to close");
    // Torn down either way: a case that goes on must not park into an app
    // still suspended on its stack.
    teardown();
    return done ? g.status : -1;
}

FILE *__real_fopen(const char *path, const char *mode);

void uitest_app_keep(const char *tag) {
    const char *dir = getenv("UITEST_KEEP");
    if (!dir || !*dir || !g.frame) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%s.ppm", dir, uitest_case ? uitest_case : "uitest", tag);
    FILE *f = __real_fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", g.w, g.h);
    for (int i = 0; i < g.w * g.h; i++) {
        uint32_t p = g.frame[i];
        unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
