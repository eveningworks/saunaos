// privd -- the root broker (docs/users-design.md, stage B; base/upriv.h
// has the protocol). polkit's shape: one long-running root service that
// takes a NARROW request -- an action from a fixed table, or one exact
// command line to run elevated -- decides by who is asking, and asks the
// person through an agent the compositor provides. No setuid program
// exists, so this is the only privileged code a normal account reaches.
//
// Policy, in order: euid 0 is granted; an account in `wheel` that holds
// the seat (CRED_SEAT) is ASKED; anyone else is refused -- there is no
// password to ask for before stage D.
#include "rt/sys.h"
#include "proc_info.h"
#include "query_abi.h"
#include "cred_abi.h"
#include "net_abi.h"
#include "signal_abi.h"
#include "base/ubulk.h"
#include "base/upriv.h"
#include "base/uappentry.h"
#include "base/uself.h"
#include "base/ubootcfg.h"
#include "base/ubootmenu.h"
#include "base/ubootmode.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AGENT_PATH "/bin/wm/system/toywm"

struct who {
    int      pid;
    uint32_t uid, euid;
    int      seat;
    char     path[QUERY_PROCPATH_MAX];
    char     user[64];
};

struct req {
    struct req *next;
    int         pid;          // the asker
    uint32_t    id;           // its request id
    uint32_t    type;         // UPRIV_T_SPAWN / UPRIV_T_ACTION
    uint32_t    prompt;       // the agent's prompt id, 0 when not asked
    uint32_t    uid;          // who asked, and running what: a verdict for a
    char        path[QUERY_PROCPATH_MAX];   // pid since recycled must not land
    char      **strs;         // path or action, then the arguments
    int         nstrs;
    char       *blob;
};

static struct ubulk_server g_srv;
static struct req *g_reqs;
static int g_agent;          // the agent's pid, 0 for none
static int g_agent_ever;     // an agent registered this boot: a graphical one
static uint32_t g_next_prompt = 1;

static void logf1(const char *fmt, const char *a, int n) {
    printf(fmt, a, n);
    fflush(stdout);
}

static int identify(int pid, struct who *w) {
    memset(w, 0, sizeof *w);
    struct proc_info pi;
    int found = 0;
    for (int i = 0; !found && sys_proc_info(i, &pi) == 0; i++)
        found = pi.pid != 0 && pi.pid == pid && pi.tgid == pid;
    if (!found) return 0;
    w->pid = pid;
    w->uid = pi.uid;
    w->euid = pi.euid;
    w->seat = (pi.cred_flags & CRED_SEAT) != 0;
    if (uproc_path(pid, w->path, sizeof w->path) < 0) w->path[0] = '\0';
    struct passwd *pw = getpwuid(pi.uid);
    if (pw) snprintf(w->user, sizeof w->user, "%s (%u)", pw->pw_name, (unsigned)pi.uid);
    else snprintf(w->user, sizeof w->user, "%u", (unsigned)pi.uid);
    return 1;
}

// polkit's unix-group: membership by the ACCOUNT's databases, not by the
// asking process's groups.
static int in_wheel(uint32_t uid) {
    struct passwd *pw = getpwuid(uid);
    if (!pw) return 0;
    char name[64];
    snprintf(name, sizeof name, "%s", pw->pw_name);
    gid_t primary = pw->pw_gid;
    struct group *gr = getgrnam("wheel");
    if (!gr) return 0;
    if (gr->gr_gid == primary) return 1;
    for (int i = 0; gr->gr_mem && gr->gr_mem[i]; i++)
        if (!strcmp(gr->gr_mem[i], name)) return 1;
    return 0;
}

static void answer(int pid, uint32_t id, int32_t rc) {
    ubulk_server_send(&g_srv, pid, UPRIV_T_ANSWER, id, &rc, sizeof rc, 1000);
}

static void req_free(struct req *r) {
    free(r->strs);
    free(r->blob);
    free(r);
}

static void req_unlink(struct req *r) {
    for (struct req **pp = &g_reqs; *pp; pp = &(*pp)->next)
        if (*pp == r) { *pp = r->next; return; }
}

// --- doing it ---------------------------------------------------------------

// Only the variables the seat may set: which entry boots next, and the
// screen-size trial's own words.
static int32_t boot_env(const char *name, const char *value) {
    static const char *const OK[] = { UBOOTMENU_ENV_NEXT, UBOOTMODE_TRY, UBOOTMODE_KEPT,
                                      UBOOTMODE_TRIED };
    for (unsigned i = 0; i < sizeof OK / sizeof OK[0]; i++)
        if (!strcmp(name, OK[i]))
            return ubootmenu_env_set(name, value[0] ? value : 0) < 0 ? -EIO : 0;
    return -EPERM;
}

// grub.cfg, whole: parsed here, so what is saved is a boot menu, and only
// at its own path.
static int32_t boot_cfg(const char *path, const char *text) {
    if (strcmp(path, UBOOTMENU_CFG) != 0) return -EPERM;
    static struct ubootcfg c;
    if (ubootcfg_parse(&c, text, (int)strlen(text)) < 0) return -EINVAL;
    const char *why = 0;
    return ubootcfg_save(&c, path, &why) < 0 ? -EIO : 0;
}

// Granted to the person at the machine without asking (base/upriv.h).
static int seat_action(const struct req *r) {
    return r->type == UPRIV_T_ACTION &&
           (!strcmp(r->strs[0], UPRIV_ACT_BOOTENV) || !strcmp(r->strs[0], UPRIV_ACT_BOOTMODE) ||
            !strcmp(r->strs[0], UPRIV_ACT_REMOTE));
}

// A remote session's leader, and one of the three signals a session
// answers -- never any other process, never any other signal.
static int32_t remote_signal(const char *pid_s, const char *sig_s) {
    int pid = atoi(pid_s), sig = atoi(sig_s);
    if (sig != SIGTERM && sig != SIGUSR1 && sig != SIGUSR2) return -EPERM;
    struct query_remotesess s;
    for (int i = 0; sys_query_record(QUERY_REMOTESESS, i, &s, sizeof s) > 0; i++)
        if ((int)s.pid == pid) return sys_kill(pid, sig) < 0 ? -sys_errno() : 0;
    return -EPERM;
}

static int32_t do_action(const struct req *r) {
    const char *a = r->strs[0];
    const char *x = r->nstrs > 1 ? r->strs[1] : 0;
    const char *y = r->nstrs > 2 ? r->strs[2] : 0;
    int rc;
    if (!strcmp(a, UPRIV_ACT_SETTIME) && x) rc = sys_settime((uint64_t)strtoull(x, 0, 10), 0);
    else if (!strcmp(a, UPRIV_ACT_MODLOAD) && x) rc = sys_modload(x);
    else if (!strcmp(a, UPRIV_ACT_MODUNLOAD) && x) rc = sys_modunload(x);
    else if (!strcmp(a, UPRIV_ACT_KILL) && x && y) rc = sys_kill(atoi(x), atoi(y));
    else if (!strcmp(a, UPRIV_ACT_BOOTENV) && x && y) return boot_env(x, y);
    else if (!strcmp(a, UPRIV_ACT_REMOTE) && x && y) return remote_signal(x, y);
    else if (!strcmp(a, UPRIV_ACT_BOOTMODE) && x) {
        const char *why = 0;
        return ubootmode_try(x[0] ? x : 0, &why) < 0 ? -EIO : 0;
    } else if (!strcmp(a, UPRIV_ACT_BOOTCFG) && x && y) return boot_cfg(x, y);
    else if (!strcmp(a, UPRIV_ACT_NETLINK) && r->nstrs >= 7) {
        struct net_linkcfg req;
        memset(&req, 0, sizeof req);
        snprintf(req.name, sizeof req.name, "%s", x);
        req.which = (uint32_t)strtoul(r->strs[2], 0, 10);
        req.rates = (uint32_t)strtoul(r->strs[3], 0, 10);
        req.eee = (uint32_t)strtoul(r->strs[4], 0, 10);
        req.flow = (uint32_t)strtoul(r->strs[5], 0, 10);
        req.moderation = (uint32_t)strtoul(r->strs[6], 0, 10);
        rc = sys_net_link(&req);
    }
    else return -EINVAL;
    return rc < 0 ? -sys_errno() : 0;
}

static void carry_out(struct req *r) {
    int32_t rc;
    if (r->type == UPRIV_T_SPAWN)
        // privd's own environment -- init's, root's -- not the asker's.
        rc = sys_cred_grant(r->pid, r->strs[0], &r->strs[1], environ) < 0 ? -sys_errno() : 0;
    else
        rc = do_action(r);
    logf1("privd: %s for pid %d\n", rc ? "refused by the kernel" : "done", r->pid);
    answer(r->pid, r->id, rc);
}

// --- the prompt's words -----------------------------------------------------

static void display_name(const char *path, char *out, size_t cap) {
    struct uappentry e;
    if (path[0] && uappentry_find_exec(path, &e)) { snprintf(out, cap, "%s", e.name); return; }
    const char *b = strrchr(path, '/');
    snprintf(out, cap, "%s", b ? b + 1 : (path[0] ? path : "A program"));
}

static void proc_name(int pid, char *out, size_t cap) {
    char path[QUERY_PROCPATH_MAX];
    if (uproc_path(pid, path, sizeof path) < 0) snprintf(out, cap, "pid %d", pid);
    else display_name(path, out, cap);
}

// headline\0 body\0 action\0 program\0 asker\0 into `out`; its length.
static uint32_t words(const struct req *r, const struct who *w, char *out, uint32_t cap) {
    char head[160], body[256], act[64], prog[320], name[64];
    display_name(w->path, name, sizeof name);
    const char *a = r->strs[0];
    if (r->type == UPRIV_T_SPAWN) {
        char target[64], cmd[200] = "";
        display_name(a, target, sizeof target);
        // The person approves what they READ: a command line that does not
        // fit the prompt whole is refused, never shown cut.
        for (int i = 1; i < r->nstrs; i++) {
            size_t n = strlen(cmd);
            int k = snprintf(cmd + n, sizeof cmd - n, "%s%s", n ? " " : "", r->strs[i]);
            if (k < 0 || (size_t)k >= sizeof cmd - n) return 0;
        }
        snprintf(head, sizeof head, "%s wants to run as administrator", target);
        if ((size_t)snprintf(body, sizeof body, "It runs with administrator rights until it closes. "
                             "Command: %s", cmd) >= sizeof body) return 0;
        snprintf(act, sizeof act, "Run as administrator");
    } else if (!strcmp(a, UPRIV_ACT_SETTIME) && r->nstrs > 1) {
        time_t t = (time_t)strtoll(r->strs[1], 0, 10);
        struct tm tm;
        char when[64] = "";
        if (localtime_r(&t, &tm)) strftime(when, sizeof when, "%d %B %Y, %H:%M", &tm);
        snprintf(head, sizeof head, "%s wants to set the system clock", name);
        snprintf(body, sizeof body, "The clock is set to %s.", when);
        snprintf(act, sizeof act, "Set the clock");
    } else if (!strcmp(a, UPRIV_ACT_MODLOAD) || !strcmp(a, UPRIV_ACT_MODUNLOAD)) {
        int load = !strcmp(a, UPRIV_ACT_MODLOAD);
        snprintf(head, sizeof head, "%s wants to %s a driver", name, load ? "load" : "unload");
        snprintf(body, sizeof body, "Driver: %s", r->nstrs > 1 ? r->strs[1] : "");
        snprintf(act, sizeof act, load ? "Load a driver" : "Unload a driver");
    } else if (!strcmp(a, UPRIV_ACT_NETLINK) && r->nstrs > 1) {
        snprintf(head, sizeof head, "%s wants to change a network card's settings", name);
        snprintf(body, sizeof body, "Card: %s -- its speed, energy saving, flow control or "
                 "interrupt moderation.", r->strs[1]);
        snprintf(act, sizeof act, "Change a network card");
    } else if (!strcmp(a, UPRIV_ACT_BOOTCFG)) {
        snprintf(head, sizeof head, "%s wants to change the boot menu", name);
        snprintf(body, sizeof body, "The file %s is rewritten; the old one is kept as .bak.",
                 r->nstrs > 1 ? r->strs[1] : "");
        snprintf(act, sizeof act, "Change the boot menu");
    } else if (!strcmp(a, UPRIV_ACT_KILL) && r->nstrs > 2) {
        char victim[64];
        int vpid = atoi(r->strs[1]);
        proc_name(vpid, victim, sizeof victim);
        snprintf(head, sizeof head, "%s wants to end a process of another account", name);
        snprintf(body, sizeof body, "%s (pid %d) is sent signal %s.", victim, vpid, r->strs[2]);
        snprintf(act, sizeof act, "End a process");
    } else {
        return 0;
    }
    snprintf(prog, sizeof prog, "%s (pid %d)", w->path[0] ? w->path : "?", w->pid);
    const char *f[5] = { head, body, act, prog, w->user };
    uint32_t n = 0;
    for (int i = 0; i < 5; i++) {
        size_t k = strlen(f[i]) + 1;
        if (n + k > cap) return 0;
        memcpy(out + n, f[i], k);
        n += (uint32_t)k;
    }
    return n;
}

// --- requests ---------------------------------------------------------------

static struct req *parse(const struct ubulk_msg *m) {
    if (m->len < sizeof(uint32_t) + 2) return 0;
    struct req *r = calloc(1, sizeof *r);
    if (!r) return 0;
    r->pid = m->pid;
    r->id = m->id;
    r->type = m->type;
    uint32_t n = m->len - (uint32_t)sizeof(uint32_t);
    r->blob = malloc(n);
    if (!r->blob) { free(r); return 0; }
    memcpy(r->blob, (const char *)m->data + sizeof(uint32_t), n);
    if (r->blob[n - 1] != '\0') { req_free(r); return 0; }   // a malformed record names nothing
    int count = 0;
    for (uint32_t i = 0; i < n; i++) count += r->blob[i] == '\0';
    r->strs = calloc((size_t)count + 1, sizeof *r->strs);
    if (!r->strs) { req_free(r); return 0; }
    for (uint32_t i = 0; i < n; ) {
        r->strs[r->nstrs++] = r->blob + i;
        i += (uint32_t)strlen(r->blob + i) + 1;
    }
    return r;
}

static void handle_request(const struct ubulk_msg *m) {
    uint32_t flags;
    memcpy(&flags, m->data, m->len >= sizeof flags ? sizeof flags : 0);
    struct req *r = parse(m);
    if (!r || r->nstrs < 1 || (r->type == UPRIV_T_SPAWN && r->nstrs < 2)) {
        if (r) req_free(r);
        answer(m->pid, m->id, -EINVAL);
        return;
    }
    struct who w;
    if (!identify(r->pid, &w)) { req_free(r); return; }   // gone already
    // The desktop's own Force Quit of an elevated window: the person chose
    // it there, so it is not asked again.
    if (r->pid == g_agent && r->type == UPRIV_T_ACTION && !strcmp(r->strs[0], UPRIV_ACT_KILL)) {
        carry_out(r);
        req_free(r);
        return;
    }
    if (w.euid == 0 || (w.seat && seat_action(r))) { carry_out(r); req_free(r); return; }
    if (!w.seat || !in_wheel(w.uid)) {
        logf1("privd: refused %s -- not an administrator at the machine (pid %d)\n", w.path, r->pid);
        answer(r->pid, r->id, -EPERM);
        req_free(r);
        return;
    }
    if (!g_agent) {
        // A text boot: sudo asked on its own terminal. On a graphical boot
        // with the desktop gone there is no one to ask.
        if (!g_agent_ever && (flags & UPRIV_SELF_CONSENT)) carry_out(r);
        else answer(r->pid, r->id, -ENXIO);
        req_free(r);
        return;
    }
    char rec[1024];
    uint32_t n = words(r, &w, rec, sizeof rec);
    if (!n) { answer(r->pid, r->id, -E2BIG); req_free(r); return; }   // cannot be shown whole
    r->prompt = g_next_prompt++;
    r->uid = w.uid;
    snprintf(r->path, sizeof r->path, "%s", w.path);
    if (ubulk_server_send(&g_srv, g_agent, UPRIV_T_PROMPT, r->prompt, rec, n, 1000) < 0) {
        answer(r->pid, r->id, -ENXIO);
        req_free(r);
        return;
    }
    r->next = g_reqs;
    g_reqs = r;
}

static void handle_agent(int pid) {
    struct who w;
    if (!identify(pid, &w) || !w.seat || strcmp(w.path, AGENT_PATH) != 0) {
        logf1("privd: %s is not the desktop -- not an agent (pid %d)\n", w.path, pid);
        return;
    }
    g_agent = pid;
    g_agent_ever = 1;
    logf1("privd: %s draws the prompt (pid %d)\n", w.path, pid);
}

static void handle_verdict(const struct ubulk_msg *m) {
    if (m->pid != g_agent || m->len < sizeof(uint32_t)) return;
    uint32_t allow;
    memcpy(&allow, m->data, sizeof allow);
    for (struct req *r = g_reqs; r; r = r->next) {
        if (r->prompt != m->id) continue;
        req_unlink(r);
        struct who now;
        // The asker may have gone while the person read the prompt, and its
        // pid gone to someone else: grant only the process that asked.
        int same = identify(r->pid, &now) && now.uid == r->uid && !strcmp(now.path, r->path);
        if (allow && same) carry_out(r);
        else answer(r->pid, r->id, allow ? -ESRCH : -ECANCELED);
        req_free(r);
        return;
    }
}

static void gone(int pid) {
    if (pid == g_agent) {
        g_agent = 0;
        // Whatever it was showing has no one to answer it.
        for (struct req *r = g_reqs; r; ) {
            struct req *n = r->next;
            answer(r->pid, r->id, -ENXIO);
            req_free(r);
            r = n;
        }
        g_reqs = 0;
        return;
    }
    for (struct req **pp = &g_reqs; *pp; ) {
        struct req *r = *pp;
        if (r->pid != pid) { pp = &r->next; continue; }
        *pp = r->next;
        if (g_agent) ubulk_server_send(&g_srv, g_agent, UPRIV_T_CANCEL, r->prompt, "", 0, 1000);
        req_free(r);
    }
}

int main(void) {
    if (ubulk_server_open(&g_srv, UPRIV_SERVICE) < 0) {
        printf("privd: the channel is taken -- not starting\n");
        return 1;
    }
    sys_notify_ready();
    char *buf = malloc(UBULK_REC_MAX);
    if (!buf) return 1;
    for (;;) {
        int left[16];
        int n = ubulk_server_poll(&g_srv, left, 16);
        for (int i = 0; i < n && i < 16; i++) gone(left[i]);
        struct ubulk_msg m;
        while (ubulk_server_recv(&g_srv, &m, buf, UBULK_REC_MAX) == 1) {
            if (m.type == UPRIV_T_SPAWN || m.type == UPRIV_T_ACTION) handle_request(&m);
            else if (m.type == UPRIV_T_AGENT) handle_agent(m.pid);
            else if (m.type == UPRIV_T_VERDICT) handle_verdict(&m);
        }
        ubulk_server_wait_change(&g_srv, 1000);
    }
}
