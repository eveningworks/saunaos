// `gui wait` and `gui waited`: a CONDITION the compositor evaluates, for a
// test that must wait for a state rather than a duration
// (docs/gui-test-sync-design.md).
//
// A TICKET, NOT A BLOCKING COMMAND: a `gui` command crosses one kernel
// slot that gives up after about two seconds (kernel/core/diag.c), and
// this compositor answers it from its frame loop. So `gui wait` registers
// the condition and answers at once; the test asks `gui waited <ticket>`
// until it is decided. The condition is evaluated on every frame AND on
// every ask -- the compositor sleeps between frames when nothing moves,
// and the ask is what wakes it.
//
// A program that EXITS before its window appears fails the wait at once
// (`--pid`): the common real failure is a crash at start, and it should
// not cost a test the whole timeout. The window is still matched by TITLE
// alone, never by that pid: a single-instance app's second copy hands over
// to the window already open and exits, which is a success.
#include "wm_internal.h"
#include "wm_debug.h"
#include "base/ugrow.h"
#include "rt/sys.h"
#include <string.h>

enum { W_WINDOW, W_GONE, W_WIDGET };
enum { S_PENDING, S_DONE, S_TIMEOUT, S_EXITED };
static const char *const STATE[] = { "pending", "done", "timeout", "exited" };

struct wait {
    unsigned ticket;
    int kind, state, pid;
    char title[WIN_TITLE_MAX];
    char name[WIN_TITLE_MAX];
    uint64_t start_ms, deadline_ms, end_ms;
};

static struct wait *g_waits;   // grown to fit; an entry is dropped once its end is read
static int g_nwaits, g_waits_cap;
static unsigned g_ticket;

#define DEFAULT_MS 60000   // generous: a wait ends as soon as it holds, so this costs only a real failure

static uint64_t now_ms(void) { return sys_monotonic_ns() / 1000000ULL; }

static int find_window(const char *title) {
    for (int i = window_count - 1; i >= 0; i--)
        if (!strcmp(windows[i].title, title)) return i;
    return -1;
}

// Up: listed, and for a client's window, its first frame presented -- a
// window in the list with no pixels yet is what made tests sample the
// desktop through it.
static int window_up(const struct window *w) {
    return !w->client_pid || w->first_w > 0;
}

static int holds(const struct wait *x) {
    int i = find_window(x->title);
    switch (x->kind) {
    case W_GONE: return i < 0;
    case W_WINDOW: return i >= 0 && window_up(&windows[i]);
    case W_WIDGET:
        if (i < 0 || !window_up(&windows[i])) return 0;
        for (int k = 0; k < windows[i].widget_count; k++) {
            const struct wm_widget *e = &windows[i].widgets[k];
            if (!strcmp(e->name, x->name)) return e->w > 0 && e->h > 0;
        }
        return 0;
    }
    return 0;
}

static void decide(struct wait *x, uint64_t t) {
    if (x->state != S_PENDING) return;
    if (holds(x)) x->state = S_DONE;
    else if (x->pid > 0 && sys_getpgid(x->pid) < 0) x->state = S_EXITED;
    else if (t >= x->deadline_ms) x->state = S_TIMEOUT;
    else return;
    x->end_ms = t;
}

void wm_debug_waits_frame(void) {
    if (!g_nwaits) return;
    uint64_t t = now_ms();
    for (int i = 0; i < g_nwaits; i++) decide(&g_waits[i], t);
}

static void report(struct dbg_out *o, const struct wait *x) {
    uint64_t t = x->state == S_PENDING ? now_ms() : x->end_ms;
    dbg_out_printf(o, "{\"ticket\":%u,\"state\":\"%s\",\"ms\":%u}\r\n", x->ticket, STATE[x->state],
                   (unsigned)(t - x->start_ms));
}

static char *tok(char **p) {
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *p = s; return 0; }
    char *e = s;
    while (*e && *e != ' ' && *e != '\t') e++;
    if (*e) *e++ = '\0';
    *p = e;
    return s;
}

static int number(const char *s, int *out) {
    if (!s || !*s) return 0;
    long v = 0;
    for (const char *q = s; *q; q++) {
        if (*q < '0' || *q > '9' || v > 100000000L) return 0;
        v = v * 10 + (*q - '0');
    }
    *out = (int)v;
    return 1;
}

static void usage(struct dbg_out *o) {
    dbg_out_write(o, "usage: gui wait window [--pid N] [--ms N] TITLE\r\n"
                     "       gui wait gone [--ms N] TITLE\r\n"
                     "       gui wait widget [--ms N] NAME TITLE\r\n"
                     "       gui waited TICKET\r\n");
}

// Ended entries nobody asked about (a test that gave up) are dropped
// once they are a minute old, so the list stays the size of what is live.
static void prune(uint64_t t) {
    int k = 0;
    for (int i = 0; i < g_nwaits; i++)
        if (g_waits[i].state == S_PENDING || t - g_waits[i].end_ms < 60000) g_waits[k++] = g_waits[i];
    g_nwaits = k;
}

void wm_debug_wait_cmd(struct dbg_out *o, char *p) {
    char *kind = tok(&p);
    struct wait x;
    memset(&x, 0, sizeof x);
    if (!kind) { usage(o); return; }
    if (!strcmp(kind, "window")) x.kind = W_WINDOW;
    else if (!strcmp(kind, "gone")) x.kind = W_GONE;
    else if (!strcmp(kind, "widget")) x.kind = W_WIDGET;
    else { usage(o); return; }
    int ms = DEFAULT_MS;
    for (;;) {
        while (*p == ' ') p++;
        if (!strncmp(p, "--pid ", 6)) { p += 6; if (!number(tok(&p), &x.pid)) { usage(o); return; } }
        else if (!strncmp(p, "--ms ", 5)) { p += 5; if (!number(tok(&p), &ms)) { usage(o); return; } }
        else break;
    }
    if (x.kind == W_WIDGET) {
        char *name = tok(&p);
        if (!name) { usage(o); return; }
        strlcpy(x.name, name, sizeof x.name);
    }
    while (*p == ' ') p++;
    if (!*p) { usage(o); return; }   // the title is the rest of the line: it may hold spaces
    strlcpy(x.title, p, sizeof x.title);

    uint64_t t = now_ms();
    prune(t);
    if (!UGROW(g_waits, g_waits_cap, g_nwaits + 1)) {
        dbg_out_write(o, "{\"ticket\":0,\"state\":\"refused\",\"ms\":0}\r\n");
        return;
    }
    if (++g_ticket == 0) g_ticket = 1;
    x.ticket = g_ticket;
    x.start_ms = t;
    x.deadline_ms = t + (uint64_t)ms;
    decide(&x, t);
    g_waits[g_nwaits++] = x;
    report(o, &x);
}

void wm_debug_waited_cmd(struct dbg_out *o, char *p) {
    int ticket;
    if (!number(tok(&p), &ticket)) { usage(o); return; }
    uint64_t t = now_ms();
    for (int i = 0; i < g_nwaits; i++) {
        struct wait *x = &g_waits[i];
        if (x->ticket != (unsigned)ticket) continue;
        decide(x, t);
        report(o, x);
        if (x->state != S_PENDING) g_waits[i] = g_waits[--g_nwaits];   // read: done with
        return;
    }
    dbg_out_printf(o, "{\"ticket\":%d,\"state\":\"unknown\",\"ms\":0}\r\n", ticket);
}
