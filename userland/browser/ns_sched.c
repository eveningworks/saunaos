// NetSurf's timers (ns_sched.h). A short list scanned in full: a page
// keeps a handful pending at once, so a heap would buy nothing.
//
// THE CONTRACT IS NETSURF'S: scheduling a (callback, pointer) pair that
// is already pending MOVES it rather than adding a second, and a
// negative time cancels it.
#include <stdlib.h>
#include <time.h>

#include "browser/ns_sched.h"

struct entry {
    long long due_ms;
    void (*cb)(void *p);
    void *p;
    struct entry *next;
};

static struct entry *g_head;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void unlink_pair(void (*cb)(void *p), void *p) {
    for (struct entry **e = &g_head; *e;) {
        if ((*e)->cb == cb && (*e)->p == p) {
            struct entry *gone = *e;
            *e = gone->next;
            free(gone);
        } else {
            e = &(*e)->next;
        }
    }
}

nserror ns_schedule(int ms, void (*cb)(void *p), void *p) {
    unlink_pair(cb, p);
    if (ms < 0) return NSERROR_OK;
    struct entry *e = malloc(sizeof *e);
    if (!e) return NSERROR_NOMEM;
    e->due_ms = now_ms() + ms;
    e->cb = cb;
    e->p = p;
    e->next = g_head;
    g_head = e;
    return NSERROR_OK;
}

int ns_sched_run(void) {
    // One due entry at a time, unlinked before it runs: a callback may
    // schedule (even itself) or cancel anything, so the list is re-read
    // from the head after every call rather than walked across one.
    for (;;) {
        long long now = now_ms();
        struct entry **due = NULL;
        for (struct entry **e = &g_head; *e; e = &(*e)->next)
            if ((*e)->due_ms <= now && (!due || (*e)->due_ms < (*due)->due_ms)) due = e;
        if (!due) break;
        struct entry *run = *due;
        *due = run->next;
        void (*cb)(void *p) = run->cb;
        void *p = run->p;
        free(run);
        cb(p);
    }
    if (!g_head) return -1;
    long long next = g_head->due_ms;
    for (struct entry *e = g_head->next; e; e = e->next)
        if (e->due_ms < next) next = e->due_ms;
    long long wait = next - now_ms();
    return wait < 0 ? 0 : (int)wait;
}
