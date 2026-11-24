// The desktop as privd's AGENT (base/upriv.h; docs/users-design.md,
// stage B): privd sends the questions, this draws each as the detailed
// confirm dialog -- the compositor's own layer, which no app can draw
// over or click for the person -- and sends back Allow or Cancel.
//
// Questions arriving while a dialog is up wait their turn, oldest first.
#include "wm_priv.h"
#include "confirm_dialog.h"
#include "base/upriv.h"
#include "base/ubulk.h"
#include "rt/sys.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct queued {
    struct queued *next;
    struct upriv_prompt p;
};

static struct ubulk_client *g_agent;
static uint64_t g_next_try;          // sys_ticks() of the next registration attempt
static struct queued *g_queue;       // oldest first
static uint32_t g_showing;           // the prompt on screen, 0 for none
static int g_policy = WM_PRIV_ASK;
static unsigned g_questions;

void wm_priv_set_policy(int policy) { g_policy = policy; }
int wm_priv_policy(void) { return g_policy; }
int wm_priv_registered(void) { return g_agent != 0; }
unsigned wm_priv_questions(void) { return g_questions; }

static void answered(int allow) {
    if (g_agent && g_showing) upriv_agent_answer(g_agent, g_showing, allow);
    g_showing = 0;
}
static void on_allow(void)  { answered(1); }
static void on_cancel(void) { answered(0); }

static void show_next(void) {
    if (g_showing || !g_queue || confirm_dialog_open) return;
    struct queued *q = g_queue;
    g_queue = q->next;
    struct confirm_detail d;
    memset(&d, 0, sizeof d);
    d.title = "Authentication Required";
    d.headline = q->p.headline;
    d.body = q->p.body;
    d.key[0] = "Action";   d.val[0] = q->p.action;
    d.key[1] = "Program";  d.val[1] = q->p.program;
    d.key[2] = "Asked by"; d.val[2] = q->p.asker;
    d.nrows = 3;
    g_showing = q->p.id;
    confirm_dialog_open_detailed(&d, "Allow", "Cancel", on_allow, on_cancel);
    free(q);
}

static void drop(uint32_t id) {
    if (id == g_showing) {
        g_showing = 0;
        confirm_dialog_close();
        return;
    }
    for (struct queued **pp = &g_queue; *pp; pp = &(*pp)->next)
        if ((*pp)->p.id == id) {
            struct queued *q = *pp;
            *pp = q->next;
            free(q);
            return;
        }
}

static void lost(void) {
    upriv_agent_close(g_agent);
    g_agent = 0;
    while (g_queue) { struct queued *q = g_queue; g_queue = q->next; free(q); }
    if (g_showing) { g_showing = 0; confirm_dialog_close(); }
}

int wm_priv_kill(int pid, int sig) {
    if (sys_kill(pid, sig) == 0) return 0;
    if (sys_errno() != EPERM || !g_agent) return -1;
    char rec[48];
    uint32_t flags = 0;
    memcpy(rec, &flags, sizeof flags);
    int n = snprintf(rec + sizeof flags, sizeof rec - sizeof flags, "%s%c%d%c%d", UPRIV_ACT_KILL, 0,
                     pid, 0, sig);
    // The answer comes back on this connection and is ignored: the window
    // goes when its process does, through the ordinary path.
    return ubulk_send(g_agent, UPRIV_T_ACTION, 0, rec, (uint32_t)(sizeof flags + (size_t)n + 1), 1000);
}

void wm_priv_poll(void) {
    uint64_t now = sys_ticks();
    if (!g_agent) {
        if (now < g_next_try) return;
        g_next_try = now + 200;   // every 2 s while privd is not there
        g_agent = upriv_agent_open();
        if (!g_agent) return;
    }
    struct upriv_prompt p;
    int r;
    while ((r = upriv_agent_next(g_agent, &p)) != 0) {
        if (r < 0) { drop(p.id); continue; }
        g_questions++;
        if (g_policy != WM_PRIV_ASK) {
            upriv_agent_answer(g_agent, p.id, g_policy == WM_PRIV_ALLOW);
            continue;
        }
        struct queued *q = malloc(sizeof *q);
        if (!q) { upriv_agent_answer(g_agent, p.id, 0); continue; }
        q->next = 0;
        q->p = p;
        // The pointers point into p.buf: re-aim them at the copy's.
        q->p.headline = q->p.buf + (p.headline - p.buf);
        q->p.body     = q->p.buf + (p.body - p.buf);
        q->p.action   = q->p.buf + (p.action - p.buf);
        q->p.program  = q->p.buf + (p.program - p.buf);
        q->p.asker    = q->p.buf + (p.asker - p.buf);
        struct queued **tail = &g_queue;
        while (*tail) tail = &(*tail)->next;
        *tail = q;
    }
    // A dialog the person closed some other way answers nothing: say no.
    if (g_showing && !confirm_dialog_open) answered(0);
    show_next();
    if (!ubulk_client_server_alive(g_agent)) lost();
}
