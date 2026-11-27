// The desktop as privd's AGENT (base/upriv.h; docs/users-design.md,
// stage B): privd sends the questions, this draws each as the detailed
// confirm dialog -- the compositor's own layer, which no app can draw
// over or click for the person -- and sends back Allow or Cancel.
//
// Questions arriving while a dialog is up wait their turn, oldest first.
// Once the asker's account has a password (users D2a) the dialog asks for
// it; Allow then waits, BUSY, for privd -- which closes the prompt when the
// password was right and sends a RETRY when it was not. A STANDARD
// account's question (users D3b2b) lists the administrators who may sign
// it: the dialog asks for the chosen one's password and names them in the
// answer -- or, when none has a password, only says why, with Close.
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
static char **g_signers;             // ...and who may sign it, by account name
static int g_nsigners;
static int g_policy = WM_PRIV_ASK;
static unsigned g_questions;

void wm_priv_set_policy(int policy) { g_policy = policy; }
int wm_priv_policy(void) { return g_policy; }
int wm_priv_registered(void) { return g_agent != 0; }
unsigned wm_priv_questions(void) { return g_questions; }

static void forget_signers(void) {
    for (int i = 0; i < g_nsigners; i++) free(g_signers[i]);
    free(g_signers);
    g_signers = 0;
    g_nsigners = 0;
}

static void answered(int allow) {
    if (g_agent && g_showing) upriv_agent_answer(g_agent, g_showing, allow);
    g_showing = 0;
    g_showing_pw = 0;
    forget_signers();
}
// A password dialog stays up, BUSY, until privd's CANCEL or RETRY.
static void on_allow(void) {
    if (!g_showing_pw) { answered(1); return; }
    const char *pw = confirm_dialog_password();
    int i = confirm_dialog_signer();
    if (!g_agent) return;
    // The dialog could not copy its drop-down: no one chosen, so no one signs.
    if (g_nsigners && (i < 0 || i >= g_nsigners)) { confirm_dialog_close(); answered(0); return; }
    if (g_nsigners)
        upriv_agent_answer_signed(g_agent, g_showing, g_signers[i], pw ? pw : "");
    else
        upriv_agent_answer_password(g_agent, g_showing, pw ? pw : "");
}

// The account's real name (GECOS's first field), else its name.
static void real_name(const char *acct, char *out, size_t cap) {
    struct passwd *pw = getpwnam(acct);
    snprintf(out, cap, "%s", pw && pw->pw_gecos && pw->pw_gecos[0] ? pw->pw_gecos : acct);
    char *comma = strchr(out, ',');   // GECOS's other fields
    if (comma) *comma = '\0';
}

// privd's comma-separated signers into g_signers. The count.
static int split_signers(const char *list) {
    int n = 1;
    for (const char *c = list; *c; c++) n += *c == ',';
    g_signers = calloc((size_t)n, sizeof *g_signers);
    if (!g_signers) return 0;
    for (const char *p = list; *p && g_nsigners < n; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && (g_signers[g_nsigners] = strndup(p, len))) g_nsigners++;
        if (!e) break;
        p = e + 1;
    }
    return g_nsigners;
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
    char label[96], real[64];
    g_showing_pw = q->p.password_for[0] != '\0';
    if (g_showing_pw) {
        real_name(q->p.password_for, real, sizeof real);
        snprintf(label, sizeof label, "Password for %s", real);
        d.password_label = label;
    }
    g_showing = q->p.id;
    if (!strcmp(q->p.admins, UPRIV_NO_SIGNER)) {
        confirm_dialog_open_detailed(&d, "Close", 0, on_cancel, 0);
        free(q);
        return;
    }
    // A Standard asker's: "User (user)" in the drop-down, "Password for
    // User" over the field, for each administrator privd listed.
    char **items = 0, **labels = 0;
    int made = 0;
    if (q->p.admins[0] && (made = split_signers(q->p.admins)) > 0) {
        items = calloc((size_t)g_nsigners, sizeof *items);
        labels = calloc((size_t)g_nsigners, sizeof *labels);
        for (int i = 0; items && labels && i < g_nsigners; i++) {
            real_name(g_signers[i], real, sizeof real);
            if (asprintf(&items[i], "%s (%s)", real, g_signers[i]) < 0) items[i] = 0;
            if (asprintf(&labels[i], "Password for %s", real) < 0) labels[i] = 0;
        }
        d.signers = (const char *const *)items;
        d.signer_labels = (const char *const *)labels;
        d.nsigners = items && labels ? g_nsigners : 0;
        d.password_label = d.nsigners ? "" : 0;
        g_showing_pw = d.nsigners > 0;
        if (!d.nsigners) forget_signers();
    }
    confirm_dialog_open_detailed(&d, "Allow", "Cancel", on_allow, on_cancel);
    for (int i = 0; i < made; i++) {
        if (items) free(items[i]);
        if (labels) free(labels[i]);
    }
    free(items);
    free(labels);
    free(q);
}

static void drop(uint32_t id) {
    if (id == g_showing) {
        g_showing = 0;
        g_showing_pw = 0;
        forget_signers();
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
    if (g_showing) { g_showing = 0; g_showing_pw = 0; forget_signers(); confirm_dialog_close(); }
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
            upriv_agent_answer(g_agent, p.id, g_policy == WM_PRIV_ALLOW && !p.password_for[0] &&
                                              !p.admins[0]);
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
