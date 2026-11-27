// The desktop as privd's AGENT (base/upriv.h; docs/users-design.md,
// stage B): privd sends the questions, this draws each as the detailed
// confirm dialog -- the compositor's own layer, which no app can draw
// over or click for the person -- and sends back Allow or Cancel.
//
// Questions arriving while a dialog is up wait their turn, oldest first.
// Once the asker's account has a password (users D2a) the dialog asks for
// it; Allow then waits, BUSY, for privd -- which closes the prompt when the
// password was right and sends a RETRY when it was not.
#include "wm_priv.h"
#include "confirm_dialog.h"
#include "base/upriv.h"
#include "base/ubulk.h"
#include "rt/sys.h"
#include <errno.h>
#include <pwd.h>
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
static int g_showing_pw;             // ...and it asks for a password
static int g_policy = WM_PRIV_ASK;
static unsigned g_questions;

void wm_priv_set_policy(int policy) { g_policy = policy; }
int wm_priv_policy(void) { return g_policy; }
int wm_priv_registered(void) { return g_agent != 0; }
unsigned wm_priv_questions(void) { return g_questions; }

static void answered(int allow) {
    if (g_agent && g_showing) upriv_agent_answer(g_agent, g_showing, allow);
    g_showing = 0;
    g_showing_pw = 0;
}
// A password dialog stays up, BUSY, until privd's CANCEL or RETRY.
static void on_allow(void) {
    if (!g_showing_pw) { answered(1); return; }
    const char *pw = confirm_dialog_password();
    if (g_agent) upriv_agent_answer_password(g_agent, g_showing, pw ? pw : "");
}
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
    // What changes first (users D3b), then who asks.
    for (int i = 0; i < q->p.nrows && d.nrows < CONFIRM_DETAIL_ROWS - 3; i++) {
        d.key[d.nrows] = q->p.row_key[i];
        d.val[d.nrows] = q->p.row_val[i];
        d.nrows++;
    }
    d.key[d.nrows] = "Action";   d.val[d.nrows++] = q->p.action;
    d.key[d.nrows] = "Program";  d.val[d.nrows++] = q->p.program;
    d.key[d.nrows] = "Asked by"; d.val[d.nrows++] = q->p.asker;
    // "Password for User": the account's real name (GECOS), else its name.
    char label[96];
    g_showing_pw = q->p.password_for[0] != '\0';
    if (g_showing_pw) {
        struct passwd *pw = getpwnam(q->p.password_for);
        const char *who = pw && pw->pw_gecos && pw->pw_gecos[0] ? pw->pw_gecos : q->p.password_for;
        char real[64];
        snprintf(real, sizeof real, "%s", who);
        char *comma = strchr(real, ',');   // GECOS's other fields
        if (comma) *comma = '\0';
        snprintf(label, sizeof label, "Password for %s", real);
        d.password_label = label;
    }
    g_showing = q->p.id;
    confirm_dialog_open_detailed(&d, "Allow", "Cancel", on_allow, on_cancel);
    free(q);
}

static void drop(uint32_t id) {
    if (id == g_showing) {
        g_showing = 0;
        g_showing_pw = 0;
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

void wm_priv_forget(void) {
    upriv_agent_forget(g_agent);
}

static int g_reopen;   // main thread only

void wm_priv_reopen(void) { g_reopen = 1; }
int wm_priv_busy(void) { return g_showing || g_queue; }

static void lost(void) {
    upriv_agent_close(g_agent);
    g_agent = 0;
    while (g_queue) { struct queued *q = g_queue; g_queue = q->next; free(q); }
    if (g_showing) { g_showing = 0; g_showing_pw = 0; confirm_dialog_close(); }
}

int wm_priv_seat_action(const char *action, const char *const *args) {
    if (!g_agent) return -1;
    char rec[512];
    uint32_t flags = 0;
    memcpy(rec, &flags, sizeof flags);
    size_t n = sizeof flags;
    for (const char *const *a = (const char *const[]){ action, 0 }; *a; a++) {
        size_t k = strlen(*a) + 1;
        if (n + k > sizeof rec) return -1;
        memcpy(rec + n, *a, k);
        n += k;
    }
    for (int i = 0; args && args[i]; i++) {
        size_t k = strlen(args[i]) + 1;
        if (n + k > sizeof rec) return -1;
        memcpy(rec + n, args[i], k);
        n += k;
    }
    return ubulk_send(g_agent, UPRIV_T_ACTION, 0, rec, (uint32_t)n, 1000);
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
    if (g_reopen) {
        g_reopen = 0;
        if (g_agent) lost();
        g_next_try = 0;
    }
    if (!g_agent) {
        if (now < g_next_try) return;
        g_next_try = now + 200;   // every 2 s while privd is not there
        g_agent = upriv_agent_open();
        if (!g_agent) return;
    }
    static struct upriv_prompt p;   // 4 KiB; the main thread's alone
    int r;
    while ((r = upriv_agent_next(g_agent, &p)) != 0) {
        if (r < 0) { drop(p.id); continue; }
        if (r == 2) {
            if (p.id == g_showing) {
                char why[64];
                snprintf(why, sizeof why, "Wrong password. %u %s left.", (unsigned)p.tries_left,
                         p.tries_left == 1 ? "try" : "tries");
                confirm_dialog_password_wrong(why);
            }
            continue;
        }
        g_questions++;
        if (g_policy != WM_PRIV_ASK) {
            // A question that needs a password is never allowed by policy:
            // the policy knows none, and privd reads a bare Allow as Cancel.
            upriv_agent_answer(g_agent, p.id, g_policy == WM_PRIV_ALLOW && !p.password_for[0]);
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
        q->p.password_for = q->p.buf + (p.password_for - p.buf);
        q->p.admins = q->p.buf + (p.admins - p.buf);
        for (int i = 0; i < p.nrows; i++) {
            q->p.row_key[i] = q->p.buf + (p.row_key[i] - p.buf);
            q->p.row_val[i] = q->p.buf + (p.row_val[i] - p.buf);
        }
        struct queued **tail = &g_queue;
        while (*tail) tail = &(*tail)->next;
        *tail = q;
    }
    // A dialog the person closed some other way answers nothing: say no.
    if (g_showing && !confirm_dialog_open) answered(0);
    show_next();
    if (!ubulk_client_server_alive(g_agent)) lost();
}
