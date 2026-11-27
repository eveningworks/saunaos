// privd -- the root broker (docs/users-design.md, stage B; base/upriv.h
// has the protocol). polkit's shape: one long-running root service that
// takes a NARROW request -- an action from a fixed table, or one exact
// command line to run elevated -- decides by who is asking, and asks the
// person through an agent the compositor provides. No setuid program
// exists, so this is the only privileged code a normal account reaches.
//
// Policy, in order: euid 0 is granted; an account in `wheel` that holds
// the seat (CRED_SEAT) is ASKED -- for its password once it has one
// (users D2a), unless `login.admin_prompt` says Allow alone, always or for
// a few minutes after the last right one (UAC's consent prompt); a wheel
// account that sends its right password with the request (`sudo` on a
// terminal) is granted; anyone else is refused.
// Also the one password checker (pass.h).
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
#include "pass.h"
#include "accounts.h"
#include "base/usetting.h"
#include "base/uinitctl.h"
#include "base/uconf.h"
#include "base/udevice.h"
#include "media/usndfmt.h"
#include "net/uremote.h"
#include "net/unetlink.h"
#include "sound_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AGENT_PATH "/bin/wm/system/toywm"
#define GREETER_ACCOUNT "greeter"
#define LAST_SIGNIN "/var/lib/greeter/last"   // root's, 0644, in the greeter's home (wm_login.c reads it)
#define SET_AUTOLOGIN "login.autologin"   // /etc/login.conf, the machine's
#define SET_ADMIN_PROMPT "login.admin_prompt"   // the same file

struct who {
    int      pid;
    uint32_t uid, euid;
    int      seat;
    char     path[QUERY_PROCPATH_MAX];
    char     user[64];    // "name (uid)", for the prompt
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
    char       *blob;         // may hold a password: wiped when freed
    uint32_t    bloblen;
    int         tries;        // passwords the prompt may still take; 0 asks none
    char       *name;         // whose password the prompt asks for (owned)
    uint64_t    given_at;     // Allow alone: when the remembered password was given; 0 if not
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

// A wrong password's answer, sent UPRIV_FAIL_DELAY_MS later (pam_faildelay)
// -- queued rather than slept, so one guesser holds up nobody else.
// ONE WRONG PASSWORD AT A TIME PER ACCOUNT: while an account's failure
// is pending, its next check is refused (EAGAIN) unhashed -- or a loop of
// requests in flight would be hashed back to back, the delay costing a
// guesser nothing.
struct later {
    struct later *next;
    uint64_t at;            // sys_monotonic_ns()
    int      pid;
    uint32_t type, id;
    int32_t  v;
    uint32_t uid;           // whose wrong password it answers
};
static struct later *g_later;

// Per uid -- but ROOT's askers are every /bin/login at once (each VT, each
// telnet connection), so root's is per asking PROCESS, as Linux's
// pam_faildelay delays only the login that was wrong: one telnet guesser
// must not lock the console out.
static int penalized(uint32_t uid, int pid) {
    for (struct later *l = g_later; l; l = l->next)
        if (l->uid == uid && (uid != 0 || l->pid == pid)) return 1;
    return 0;
}

// Its answers go nowhere once `pid` has gone: its pid may be someone
// else's by the time they are due.
static void purge_later(int pid) {
    for (struct later **pp = &g_later; *pp; ) {
        struct later *l = *pp;
        if (l->pid != pid) { pp = &l->next; continue; }
        *pp = l->next;
        free(l);
    }
}

static void send_later(int pid, uint32_t type, uint32_t id, int32_t v, uint32_t uid) {
    struct later *l = malloc(sizeof *l);
    if (!l) { ubulk_server_send(&g_srv, pid, type, id, &v, sizeof v, 1000); return; }
    l->at = sys_monotonic_ns() + (uint64_t)UPRIV_FAIL_DELAY_MS * 1000000ull;
    l->uid = uid;
    l->pid = pid;
    l->type = type;
    l->id = id;
    l->v = v;
    struct later **pp = &g_later;   // kept in time order
    while (*pp && (*pp)->at <= l->at) pp = &(*pp)->next;
    l->next = *pp;
    *pp = l;
}

// Sends what is due; the milliseconds until the next, at most `cap`.
static int run_later(int cap) {
    uint64_t now = sys_monotonic_ns();
    while (g_later && g_later->at <= now) {
        struct later *l = g_later;
        g_later = l->next;
        ubulk_server_send(&g_srv, l->pid, l->type, l->id, &l->v, sizeof l->v, 1000);
        free(l);
    }
    if (!g_later) return cap;
    uint64_t ms = (g_later->at - now) / 1000000ull + 1;
    return ms < (uint64_t)cap ? (int)ms : cap;
}

// --- what the prompt asks: login.admin_prompt -------------------------------

// Its choices, from the most asked to the least; `keep` the minutes a
// right password lets the next prompts ask Allow alone.
static const struct { const char *name; int keep_min; } ADMIN_PROMPT[] = {
    { "password", 0 }, { "remember5", 5 }, { "remember15", 15 }, { "consent", -1 },
};
#define N_ADMIN_PROMPT ((int)(sizeof ADMIN_PROMPT / sizeof ADMIN_PROMPT[0]))

// Its index in ADMIN_PROMPT, -1 for no choice of it.
static int admin_prompt_index(const char *v) {
    for (int i = 0; i < N_ADMIN_PROMPT; i++)
        if (!strcmp(v, ADMIN_PROMPT[i].name)) return i;
    return -1;
}

// The machine's choice, read at each request; unreadable or unknown asks
// for the password, as if the setting were not there.
static int admin_prompt(void) {
    char v[32];
    int i = usetting_get(SET_ADMIN_PROMPT, v, sizeof v) ? admin_prompt_index(v) : -1;
    return i < 0 ? 0 : i;
}

// A right password, per account, while a `remember` choice keeps it. In
// memory only: forgotten when the screen locks or comes back from the
// saver (UPRIV_T_FORGET), when the agent goes (a sign-out), and at boot.
struct kept {
    struct kept *next;
    uint32_t uid;
    uint64_t at;            // sys_monotonic_ns() of the right password
};
static struct kept *g_kept;

static void kept_forget(void) {
    while (g_kept) {
        struct kept *k = g_kept;
        g_kept = k->next;
        free(k);
    }
}

static void kept_remember(uint32_t uid) {
    struct kept *k = g_kept;
    while (k && k->uid != uid) k = k->next;
    if (!k) {
        if (!(k = malloc(sizeof *k))) return;   // not remembered: the next prompt asks again
        k->uid = uid;
        k->next = g_kept;
        g_kept = k;
    }
    k->at = sys_monotonic_ns();
}

// When `uid` gave its right password, if the choice NOW in force still
// keeps it (a shorter choice applies at once); 0 when it asks again.
static uint64_t kept_since(uint32_t uid) {
    int keep = ADMIN_PROMPT[admin_prompt()].keep_min;
    if (keep <= 0) return 0;
    for (struct kept *k = g_kept; k; k = k->next)
        if (k->uid == uid && sys_monotonic_ns() - k->at < (uint64_t)keep * 60ull * 1000000000ull)
            return k->at;
    return 0;
}

static void req_free(struct req *r) {
    free(r->name);
    free(r->strs);
    if (r->blob) memset(r->blob, 0, r->bloblen);
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

static int is_action(const struct req *r, const char *a) {
    return r->type == UPRIV_T_ACTION && !strcmp(r->strs[0], a);
}

// Every setting in a `settings` request is the SEAT's (display.scale):
// stored as the machine's in a file root owns, and the person at the
// machine's to change (users D3b2a). Checked after settings_ok().
static int settings_all_seat(const struct req *r) {
    for (int i = 1; i + 1 < r->nstrs; i += 2) {
        static struct setting_msg m;
        if (usetting_info(r->strs[i], &m) != 0 || !(m.sflags & SETTING_ABI_SF_SEAT)) return 0;
    }
    return 1;
}

// Granted to the person at the machine without asking (base/upriv.h).
static int seat_action(const struct req *r) {
    return is_action(r, UPRIV_ACT_BOOTENV) || is_action(r, UPRIV_ACT_BOOTMODE) ||
           is_action(r, UPRIV_ACT_REMOTE) || is_action(r, UPRIV_ACT_APPVOLUME) ||
           is_action(r, UPRIV_ACT_SOUNDFORMAT) ||
           (is_action(r, UPRIV_ACT_SETTINGS) && settings_all_seat(r));
}

// --- the files the desktop changed in /etc (users D3b2a) --------------------
//
// privd is the boundary: every argument is checked here, before anyone is
// asked, whatever the library on the asking side already checked.

// A key one line of an /etc file can carry: a program's name in
// sound.conf. No '=', '[', ']', '#' or control character, nothing empty.
static int conf_key_ok(const char *k) {
    size_t n = strlen(k);
    if (!n || n >= 64) return 0;
    for (const unsigned char *p = (const unsigned char *)k; *p; p++)
        if (*p < ' ' || *p == 0x7f || *p == '=' || *p == '[' || *p == ']' || *p == '#') return 0;
    return 1;
}

static int percent_ok(const char *v) {
    char *end;
    long n = strtol(v, &end, 10);
    return v[0] >= '0' && v[0] <= '9' && !*end && n >= 0 && n <= 100 && strlen(v) <= 3;
}

static int appvolume_ok(const struct req *r) {
    return r->nstrs == 3 && conf_key_ok(r->strs[1]) && percent_ok(r->strs[2]);
}

static int soundformat_ok(const struct req *r, struct usndfmt *f) {
    struct usndfmt scratch;
    return r->nstrs == 5 && usndfmt_card_ok(r->strs[1]) &&
           usndfmt_from_words(r->strs[2], r->strs[3], r->strs[4], f ? f : &scratch);
}

// key, then "=value" or "-", per change -- each one uremote_change_ok()'s.
static int remote_ok(const struct req *r) {
    if (r->nstrs < 3 || (r->nstrs - 1) % 2) return 0;
    if ((r->nstrs - 1) / 2 > UPRIV_PROMPT_ROWS) return 0;
    for (int i = 1; i + 1 < r->nstrs; i += 2)
        if (!uremote_change_ok(r->strs[i], r->strs[i + 1])) return 0;
    return 1;
}

// The device a request names, looked up fresh: `*list` is the caller's
// to free. NULL for none.
static const struct udevice *device_of(const char *id, struct udevice **list) {
    *list = NULL;
    int n = udevice_list(list);
    for (int i = 0; i < n; i++)
        if (!strcmp((*list)[i].id, id)) return &(*list)[i];
    return NULL;
}

static int device_ok(const struct req *r) {
    if (r->nstrs != 4 || (strcmp(r->strs[1], "disable") && strcmp(r->strs[1], "enable")) ||
        (strcmp(r->strs[3], "0") && strcmp(r->strs[3], "1")))
        return 0;
    struct udevice *list;
    const struct udevice *d = device_of(r->strs[2], &list);
    int ok = d && d->can_disable;
    free(list);
    return ok;
}

static int32_t do_device(const struct req *r) {
    struct udevice *list;
    const struct udevice *d = device_of(r->strs[2], &list);
    int32_t rc = !d ? -ENODEV : !strcmp(r->strs[1], "disable") ? udevice_disable(d, r->strs[3][0] == '1')
                                                                : udevice_enable(d);
    free(list);
    return rc;
}

// The card's QUERY_NETDEV record, by its name; 1 when found.
static int netdev_of(const char *name, struct query_netdev *out) {
    for (int i = 0; sys_query_record(QUERY_NETDEV, i, out, sizeof *out) > 0; i++)
        if (!strcmp(out->name, name)) return 1;
    return 0;
}

// netlink's six numbers, and a card that is there.
static int netlink_ok(const struct req *r) {
    if (r->nstrs != 7) return 0;
    for (int i = 2; i < 7; i++) {
        char *end;
        strtoul(r->strs[i], &end, 10);
        if (!r->strs[i][0] || *end) return 0;
    }
    struct query_netdev d;
    return netdev_of(r->strs[1], &d);
}

// What an action's arguments must be before anyone is asked: 0 when
// they are, else the answer (-EINVAL; -E2BIG for more rows than a prompt).
static int32_t args_refused(const struct req *r) {
    if (r->type != UPRIV_T_ACTION) return 0;
    if (is_action(r, UPRIV_ACT_APPVOLUME)) return appvolume_ok(r) ? 0 : -EINVAL;
    if (is_action(r, UPRIV_ACT_SOUNDFORMAT)) return soundformat_ok(r, 0) ? 0 : -EINVAL;
    if (is_action(r, UPRIV_ACT_REMOTEDESKTOP))
        return remote_ok(r) ? 0 : (r->nstrs - 1) / 2 > UPRIV_PROMPT_ROWS ? -E2BIG : -EINVAL;
    if (is_action(r, UPRIV_ACT_DEVICE)) return device_ok(r) ? 0 : -EINVAL;
    if (is_action(r, UPRIV_ACT_NETLINK)) return netlink_ok(r) ? 0 : -EINVAL;
    return 0;
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

// UPRIV_ACT_SETTINGS: name, then "=value" or "-", per setting. Each is
// checked to exist before the prompt (settings_ok()), and made here as
// root in order; the first the registry refuses stops the rest.
static int settings_ok(const struct req *r) {
    if (r->nstrs < 3 || (r->nstrs - 1) % 2) return 0;
    if ((r->nstrs - 1) / 2 > UPRIV_PROMPT_ROWS) return 0;
    for (int i = 1; i + 1 < r->nstrs; i += 2) {
        static struct setting_msg m;   // a kilobyte; privd is one thread
        const char *v = r->strs[i + 1];
        if ((v[0] != '=' && strcmp(v, "-")) || usetting_info(r->strs[i], &m) != 0) return 0;
    }
    return 1;
}

static int32_t apply_settings(const struct req *r) {
    if (!settings_ok(r)) return -EINVAL;
    for (int i = 1; i + 1 < r->nstrs; i += 2) {
        const char *name = r->strs[i], *v = r->strs[i + 1];
        static struct setting_msg m;
        memset(&m, 0, sizeof m);
        m.op = v[0] == '=' ? SETTING_OP_SET : SETTING_OP_UNSET;
        snprintf(m.name, sizeof m.name, "%s", name);
        if (v[0] == '=') snprintf(m.value, sizeof m.value, "%s", v + 1);
        if (usetting_dispatch(&m) != 0) return -EINVAL;
        // UNSET of a value that was never written changed nothing: not an error.
        if (m.op == SETTING_OP_SET && m.result != SETTING_SAVED)
            return m.result == SETTING_UNSAVED ? -EIO : -EINVAL;
    }
    return 0;
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
    else if (!strcmp(a, UPRIV_ACT_AUTOLOGIN) && x)
        return usetting_set(SET_AUTOLOGIN, x) == SETTING_SAVED ? 0 : -EIO;
    else if (!strcmp(a, UPRIV_ACT_ADMINPROMPT) && x) {
        if (admin_prompt_index(x) < 0) return -EINVAL;
        if (usetting_set(SET_ADMIN_PROMPT, x) != SETTING_SAVED) return -EIO;
        if (ADMIN_PROMPT[admin_prompt_index(x)].keep_min <= 0) kept_forget();
        return 0;
    }
    else if (!strcmp(a, UPRIV_ACT_SETTINGS)) return apply_settings(r);
    else if (!strcmp(a, UPRIV_ACT_USERADD) && r->nstrs >= 5)
        return acct_add(x, y, !strcmp(r->strs[3], "1"), r->strs[4]);
    else if (!strcmp(a, UPRIV_ACT_USERDEL) && x && y) {
        if (strcmp(y, "keep") && strcmp(y, "delete")) return -EINVAL;
        return acct_del(x, !strcmp(y, "delete"));
    } else if (!strcmp(a, UPRIV_ACT_USERMOD) && r->nstrs >= 4) return acct_mod(x, y, r->strs[3]);
    else if (!strcmp(a, UPRIV_ACT_NETLINK)) {
        struct net_linkcfg req;
        struct query_netdev d;
        if (!netlink_ok(r) || !netdev_of(x, &d)) return -EINVAL;
        memset(&req, 0, sizeof req);
        snprintf(req.name, sizeof req.name, "%s", x);
        req.which = (uint32_t)strtoul(r->strs[2], 0, 10);
        req.rates = (uint32_t)strtoul(r->strs[3], 0, 10);
        req.eee = (uint32_t)strtoul(r->strs[4], 0, 10);
        req.flow = (uint32_t)strtoul(r->strs[5], 0, 10);
        req.moderation = (uint32_t)strtoul(r->strs[6], 0, 10);
        if (sys_net_link(&req) < 0) return -sys_errno();
        // ...and kept for the next boot: /etc/net.conf is root's (users D3b2a).
        return unetlink_save(&d, &req) < 0 ? -EIO : 0;
    } else if (!strcmp(a, UPRIV_ACT_APPVOLUME)) {
        if (!appvolume_ok(r)) return -EINVAL;
        return uconf_set(SND_CONFIG_FILE, x, y) ? 0 : -EIO;
    } else if (!strcmp(a, UPRIV_ACT_SOUNDFORMAT)) {
        struct usndfmt f;
        if (!soundformat_ok(r, &f)) return -EINVAL;
        return usndfmt_write(x, &f) ? 0 : -EIO;
    } else if (!strcmp(a, UPRIV_ACT_REMOTEDESKTOP)) {
        if (!remote_ok(r)) return -EINVAL;
        for (int i = 1; i + 1 < r->nstrs; i += 2)
            if (uremote_change_apply(r->strs[i], r->strs[i + 1]) < 0) return -EIO;
        return 0;
    } else if (!strcmp(a, UPRIV_ACT_DEVICE)) {
        if (!device_ok(r)) return -EINVAL;
        return do_device(r);
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
    if (path[0] && uappentry_find_exec(path, &e)) { snprintf(out, cap, "%s", e.name); uappentry_free(&e); return; }
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
    // What changes, a row each (UPRIV_T_PROMPT's tail); the settings action's.
    static struct { char key[SETTING_ABI_LABEL_MAX], val[200]; } rows[UPRIV_PROMPT_ROWS];
    int nrows = 0;
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
                 "interrupt moderation, kept for the next boot.", r->strs[1]);
        snprintf(act, sizeof act, "Change a network card");
    } else if (!strcmp(a, UPRIV_ACT_REMOTEDESKTOP)) {
        if (!remote_ok(r)) return 0;
        static struct uremote_conf now;
        uremote_load(&now);
        snprintf(head, sizeof head, "%s wants to change remote desktop", name);
        snprintf(body, sizeof body, "Who may see and control this computer from another one.");
        snprintf(act, sizeof act, "Change remote desktop");
        for (int i = 1; i + 1 < r->nstrs; i += 2, nrows++)
            uremote_change_row(&now, r->strs[i], r->strs[i + 1], rows[nrows].key, sizeof rows[nrows].key,
                               rows[nrows].val, sizeof rows[nrows].val);
    } else if (!strcmp(a, UPRIV_ACT_DEVICE)) {
        if (!device_ok(r)) return 0;
        struct udevice *list;
        // Looked up again: a device unplugged since device_ok() is gone.
        const struct udevice *d = device_of(r->strs[2], &list);
        if (!d) { free(list); return 0; }
        int off = !strcmp(r->strs[1], "disable");
        snprintf(head, sizeof head, "%s wants to %s a device", name, off ? "disable" : "enable");
        if ((size_t)snprintf(body, sizeof body, "%s (%s)%s", d->name, d->location,
                             !off ? " gets its driver back."
                             : r->strs[3][0] == '1' ? " stops working, and stays off after a restart."
                                                    : " stops working until it is enabled or the computer restarts.")
            >= sizeof body) { free(list); return 0; }
        snprintf(act, sizeof act, off ? "Disable a device" : "Enable a device");
        free(list);
    } else if (!strcmp(a, UPRIV_ACT_APPVOLUME) && appvolume_ok(r)) {
        snprintf(head, sizeof head, "%s wants to change a program's volume", name);
        snprintf(body, sizeof body, "%s plays at %s%% from now on.", r->strs[1], r->strs[2]);
        snprintf(act, sizeof act, "Change the volume");
    } else if (!strcmp(a, UPRIV_ACT_SOUNDFORMAT) && soundformat_ok(r, 0)) {
        snprintf(head, sizeof head, "%s wants to change a sound card's format", name);
        snprintf(body, sizeof body, "Card: %s -- the rate it plays at, and its width.", r->strs[1]);
        snprintf(act, sizeof act, "Change a sound card");
    } else if (!strcmp(a, UPRIV_ACT_BOOTCFG)) {
        snprintf(head, sizeof head, "%s wants to change the boot menu", name);
        snprintf(body, sizeof body, "The file %s is rewritten; the old one is kept as .bak.",
                 r->nstrs > 1 ? r->strs[1] : "");
        snprintf(act, sizeof act, "Change the boot menu");
    } else if (!strcmp(a, UPRIV_ACT_ADMINPROMPT) && r->nstrs > 1 && admin_prompt_index(r->strs[1]) >= 0) {
        int keep = ADMIN_PROMPT[admin_prompt_index(r->strs[1])].keep_min;
        snprintf(head, sizeof head, "%s wants to change what administrator prompts ask", name);
        if (keep < 0)
            snprintf(body, sizeof body, "From now on an app that needs administrator rights asks only "
                     "Allow or Cancel, without the password. sudo still asks for it.");
        else
            snprintf(body, sizeof body, "From now on, after the right password, the next %d minutes of "
                     "administrator prompts ask only Allow or Cancel. sudo still asks for the password.", keep);
        snprintf(act, sizeof act, "Change the administrator prompt");
    } else if (!strcmp(a, UPRIV_ACT_AUTOLOGIN) && r->nstrs > 1) {
        snprintf(head, sizeof head, "%s wants to sign %s in automatically", name, r->strs[1]);
        snprintf(body, sizeof body, "At boot the desktop starts without the login screen, and without "
                 "asking for a password.");
        snprintf(act, sizeof act, "Sign in automatically");
    } else if (!strcmp(a, UPRIV_ACT_USERADD) && r->nstrs >= 5) {
        // The password is never in the words: the prompt names the account.
        int admin = !strcmp(r->strs[3], "1");
        snprintf(head, sizeof head, "%s wants to add an account", name);
        // A full name too long to show whole is refused, never shown cut.
        if ((size_t)snprintf(body, sizeof body, "A new %s account%s%s%s named %s, with its home in /home/%s.%s",
                 admin ? "administrator" : "standard", r->strs[2][0] ? " for " : "", r->strs[2],
                 r->strs[2][0] ? "," : "", r->strs[1], r->strs[1],
                 admin ? " An administrator can add people and change the machine." : "") >= sizeof body)
            return 0;
        snprintf(act, sizeof act, "Add an account");
    } else if (!strcmp(a, UPRIV_ACT_USERDEL) && r->nstrs >= 3) {
        struct passwd *pw = getpwnam(r->strs[1]);
        snprintf(head, sizeof head, "%s wants to remove the account %s", name, r->strs[1]);
        snprintf(body, sizeof body, "%s can no longer sign in. Its files in %s are %s.",
                 r->strs[1], pw && pw->pw_dir ? pw->pw_dir : "its home",
                 !strcmp(r->strs[2], "delete") ? "DELETED" : "kept");
        snprintf(act, sizeof act, "Remove an account");
    } else if (!strcmp(a, UPRIV_ACT_USERMOD) && r->nstrs >= 4) {
        const char *who = r->strs[1], *field = r->strs[2], *v = r->strs[3];
        if (!strcmp(field, "admin")) {
            int on = !strcmp(v, "1");
            snprintf(head, sizeof head, "%s wants to make %s %s", name, who,
                     on ? "an administrator" : "a standard account");
            snprintf(body, sizeof body, on ? "%s can then add people and change the machine."
                                           : "%s can then no longer add people or change the machine.", who);
            snprintf(act, sizeof act, "Change an account's type");
        } else if (!strcmp(field, "real")) {
            snprintf(head, sizeof head, "%s wants to rename the account %s", name, who);
            if ((size_t)snprintf(body, sizeof body, "Its full name becomes \"%s\".", v) >= sizeof body) return 0;
            snprintf(act, sizeof act, "Rename an account");
        } else if (!strcmp(field, "password")) {
            snprintf(head, sizeof head, "%s wants to %s %s's password", name, v[0] ? "change" : "remove", who);
            snprintf(body, sizeof body, v[0] ? "The new password is set without asking for the old one."
                                             : "%s then signs in without a password.", who);
            snprintf(act, sizeof act, "Change a password");
        } else {
            return 0;
        }
    } else if (!strcmp(a, UPRIV_ACT_SETTINGS)) {
        if (!settings_ok(r)) return 0;
        snprintf(head, sizeof head, "%s wants to change this computer's settings", name);
        snprintf(body, sizeof body, "They apply to everyone who signs in on this computer.");
        snprintf(act, sizeof act, "Change this computer's settings");
        for (int i = 1; i + 1 < r->nstrs; i += 2) {
            static struct setting_msg m;
            char now[SETTING_ABI_VALUE_MAX], was[96], will[96];
            usetting_info(r->strs[i], &m);
            if (!usetting_get(r->strs[i], now, sizeof now)) now[0] = '\0';
            usetting_value_label(r->strs[i], now, was, sizeof was);
            if (r->strs[i + 1][0] == '=') usetting_value_label(r->strs[i], r->strs[i + 1] + 1, will, sizeof will);
            else snprintf(will, sizeof will, "its default");
            snprintf(rows[nrows].key, sizeof rows[nrows].key, "%s", m.label[0] ? m.label : r->strs[i]);
            snprintf(rows[nrows].val, sizeof rows[nrows].val, "%s -> %s", was[0] ? was : "(empty)", will);
            nrows++;
        }
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
    // Allow alone because a password is remembered: say so, when it fits --
    // the person approves the request, which is all said above.
    if (r->given_at) {
        uint64_t min = (sys_monotonic_ns() - r->given_at) / (60ull * 1000000000ull);
        size_t n = strlen(body);
        char ago[48];
        if (min == 0) snprintf(ago, sizeof ago, "less than a minute ago");
        else snprintf(ago, sizeof ago, "%llu minute%s ago", (unsigned long long)min, min == 1 ? "" : "s");
        int k = snprintf(body + n, sizeof body - n, " You gave your password %s, so this asks only Allow.", ago);
        if (k < 0 || (size_t)k >= sizeof body - n) body[n] = '\0';
    }
    snprintf(prog, sizeof prog, "%s (pid %d)", w->path[0] ? w->path : "?", w->pid);
    const char *f[7 + 2 * UPRIV_PROMPT_ROWS] = { head, body, act, prog, w->user,
                                                 r->tries && r->name ? r->name : "", "" };
    int nf = 7;
    for (int i = 0; i < nrows; i++) {
        f[nf++] = rows[i].key;
        f[nf++] = rows[i].val;
    }
    uint32_t n = 0;
    for (int i = 0; i < nf; i++) {
        size_t k = strlen(f[i]) + 1;
        if (n + k >= cap) return 0;   // the agent's copy keeps one byte for its NUL
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
    r->bloblen = n;
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

// The account's name for `uid`, owned by the caller; NULL for none.
static char *name_of(uint32_t uid) {
    struct passwd *pw = getpwuid(uid);
    return pw ? strdup(pw->pw_name) : NULL;
}

// `acct` names `w`'s own account (by uid, so no name is ever cut short).
static int own(const char *acct, const struct who *w) {
    struct passwd *pw = getpwnam(acct);
    return pw && pw->pw_uid == w->uid;
}

// A wrong password, answered UPRIV_FAIL_DELAY_MS later.
static void wrong(int pid, uint32_t id, uint32_t uid, const char *acct) {
    logf1("privd: a wrong password for %s (pid %d)\n", acct, pid);
    send_later(pid, UPRIV_T_ANSWER, id, -EACCES, uid);
}

// UPRIV_T_AUTH: root may ask about any account, anyone else about their
// own (unix_chkpwd's rule).
static void handle_auth(const struct ubulk_msg *m, uint32_t flags, struct req *r, const struct who *w) {
    if (r->nstrs < 2) { answer(m->pid, m->id, -EINVAL); return; }
    const char *acct = r->strs[0], *pw = r->strs[1];
    if (w->euid != 0 && !own(acct, w)) { answer(m->pid, m->id, -EPERM); return; }
    if (flags & UPRIV_PROBE) { answer(m->pid, m->id, pass_has(acct) ? -EACCES : 0); return; }
    if (penalized(w->uid, m->pid)) { answer(m->pid, m->id, -EAGAIN); return; }
    if (pass_check(acct, pw)) answer(m->pid, m->id, 0);
    else wrong(m->pid, m->id, w->uid, acct);
}

// UPRIV_ACT_PASSWD: account, current, new.
static void handle_passwd(const struct ubulk_msg *m, struct req *r, const struct who *w) {
    if (r->nstrs < 4) { answer(m->pid, m->id, -EINVAL); return; }
    const char *acct = r->strs[1], *cur = r->strs[2], *pw = r->strs[3];
    if (!getpwnam(acct)) { answer(m->pid, m->id, -ENOENT); return; }
    if (w->euid != 0) {
        if (!own(acct, w)) { answer(m->pid, m->id, -EPERM); return; }
        if (penalized(w->uid, m->pid)) { answer(m->pid, m->id, -EAGAIN); return; }
        if (!pass_check(acct, cur)) { wrong(m->pid, m->id, w->uid, acct); return; }
    }
    int32_t rc = pass_set(acct, pw);
    logf1(rc ? "privd: could not change %s's password (pid %d)\n"
             : "privd: %s's password changed (pid %d)\n", acct, m->pid);
    // SETTING ONE TURNS AUTOMATIC SIGN-IN OFF for the account (users
    // D2b): a password nobody is asked for protects nothing.
    if (!rc && pw[0]) {
        // Through the settings layer, as Settings writes it, so every
        // reader's cache sees the change (root: the machine's layer).
        char who[256] = "";
        if (usetting_get(SET_AUTOLOGIN, who, sizeof who) && !strcmp(who, acct) &&
            usetting_set(SET_AUTOLOGIN, "") == SETTING_SAVED)
            logf1("privd: %s signs in at the login screen from now on (pid %d)\n", acct, m->pid);
    }
    answer(m->pid, m->id, rc);
}

// UPRIV_T_LOGIN: the greeter's sign-in. Believed from the compositor
// running as the greeter account at the seat, and from nothing else.
static void handle_login(const struct ubulk_msg *m, struct req *r, const struct who *w) {
    if (r->nstrs < 2) { answer(m->pid, m->id, -EINVAL); return; }
    const char *acct = r->strs[0], *pw = r->strs[1];
    struct passwd *g = getpwnam(GREETER_ACCOUNT);
    if (!g || g->pw_uid != w->uid || !w->seat || strcmp(w->path, AGENT_PATH) != 0) {
        logf1("privd: %s is not the login screen -- no sign-in (pid %d)\n", w->path, m->pid);
        answer(m->pid, m->id, -EPERM);
        return;
    }
    struct passwd *who = getpwnam(acct);
    if (!who || who->pw_uid < 1000) { answer(m->pid, m->id, -EPERM); return; }
    if (penalized(w->uid, m->pid)) { answer(m->pid, m->id, -EAGAIN); return; }
    if (!pass_check(acct, pw)) { wrong(m->pid, m->id, w->uid, acct); return; }
    int rc = uinitctl_session(acct);
    logf1(rc == INITCTL_OK ? "privd: %s signed in (pid %d)\n" : "privd: init started no session for %s (pid %d)\n",
          acct, m->pid);
    // WHO SIGNED IN LAST, for the login screen to put in the middle next
    // time (AccountsService keeps it for GDM). Written here: init stops
    // the greeter the moment the session begins, before it hears this.
    // THE FOLDER IS THE GREETER'S, so the greeter could leave a hard link
    // to /etc/shadow at the name: a new file, made O_EXCL, renamed over it.
    if (rc == INITCTL_OK) {
        unlink(LAST_SIGNIN ".new");
        int fd = open(LAST_SIGNIN ".new", O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            int ok = write(fd, acct, strlen(acct)) == (ssize_t)strlen(acct) && write(fd, "\n", 1) == 1;
            close(fd);
            if (!ok || rename(LAST_SIGNIN ".new", LAST_SIGNIN) < 0) unlink(LAST_SIGNIN ".new");
        }
    }
    answer(m->pid, m->id, rc == INITCTL_OK ? 0 : rc < 0 ? -EIO : -EBUSY);
}

static void handle_request(const struct ubulk_msg *m) {
    uint32_t flags = 0;
    memcpy(&flags, m->data, m->len >= sizeof flags ? sizeof flags : 0);
    struct req *r = parse(m);
    // WITH_PASSWORD: the first string is the asker's password, off the front.
    const char *typed = 0;
    if (r && (flags & UPRIV_WITH_PASSWORD) && r->nstrs > 0) {
        typed = r->strs[0];
        memmove(r->strs, r->strs + 1, (size_t)r->nstrs * sizeof *r->strs);   // its NULL too
        r->nstrs--;
    }
    if (!r || r->nstrs < 1 || (r->type == UPRIV_T_SPAWN && r->nstrs < 2)) {
        if (r) req_free(r);
        answer(m->pid, m->id, -EINVAL);
        return;
    }
    struct who w;
    if (!identify(r->pid, &w)) { req_free(r); return; }   // gone already
    if (r->type == UPRIV_T_AUTH) { handle_auth(m, flags, r, &w); req_free(r); return; }
    if (r->type == UPRIV_T_LOGIN) { handle_login(m, r, &w); req_free(r); return; }
    // A change the registry does not know is refused before anyone is asked.
    if (r->type == UPRIV_T_ACTION && !strcmp(r->strs[0], UPRIV_ACT_SETTINGS) && !settings_ok(r)) {
        answer(r->pid, r->id, (r->nstrs - 1) / 2 > UPRIV_PROMPT_ROWS ? -E2BIG : -EINVAL);
        req_free(r);
        return;
    }
    int32_t bad = args_refused(r);
    if (bad) {
        answer(r->pid, r->id, bad);
        req_free(r);
        return;
    }
    if (r->type == UPRIV_T_ACTION && !strcmp(r->strs[0], UPRIV_ACT_PASSWD)) {
        handle_passwd(m, r, &w);
        req_free(r);
        return;
    }
    // The desktop's own Force Quit of an elevated window: the person chose
    // it there, so it is not asked again.
    if (r->pid == g_agent && r->type == UPRIV_T_ACTION && !strcmp(r->strs[0], UPRIV_ACT_KILL)) {
        carry_out(r);
        req_free(r);
        return;
    }
    // AUTOMATIC SIGN-IN names only the asker's own account; switching it
    // off is the seat's right, switching it on goes through the question.
    if (r->type == UPRIV_T_ACTION && !strcmp(r->strs[0], UPRIV_ACT_AUTOLOGIN) && w.euid != 0) {
        if (r->nstrs < 2) { answer(r->pid, r->id, -EINVAL); req_free(r); return; }
        if (r->strs[1][0] && !own(r->strs[1], &w)) { answer(r->pid, r->id, -EPERM); req_free(r); return; }
        if (!r->strs[1][0] && w.seat) { carry_out(r); req_free(r); return; }
    }
    // WHAT THE PROMPT ASKS is the seat's to make stricter; asking LESS is
    // asked, with the password even under "consent" -- UAC's slider asks
    // too -- or one click would turn the password off for good.
    int force_pw = 0;
    if (r->type == UPRIV_T_ACTION && !strcmp(r->strs[0], UPRIV_ACT_ADMINPROMPT) && w.euid != 0) {
        int want = r->nstrs > 1 ? admin_prompt_index(r->strs[1]) : -1;
        if (want < 0) { answer(r->pid, r->id, -EINVAL); req_free(r); return; }
        if (want <= admin_prompt() && w.seat) { carry_out(r); req_free(r); return; }
        force_pw = 1;
    }
    if (w.euid == 0 || (w.seat && seat_action(r))) { carry_out(r); req_free(r); return; }
    char *me = name_of(w.uid);
    int needs_pw = me && pass_has(me);
    // sudo's shape: the right password from an account in wheel is the
    // consent, on whatever terminal it was typed -- ONLY for an account
    // that has one: "" matching an account with none would be root for
    // the asking, at the machine or not.
    if (typed && needs_pw) {
        if (!in_wheel(w.uid)) answer(r->pid, r->id, -EPERM);
        else if (penalized(w.uid, r->pid)) answer(r->pid, r->id, -EAGAIN);
        else if (pass_check(me, typed)) carry_out(r);
        else wrong(r->pid, r->id, w.uid, me);
        free(me);
        req_free(r);
        return;
    }
    if (!w.seat || !in_wheel(w.uid)) {
        logf1("privd: refused %s -- not an administrator at the machine (pid %d)\n", w.path, r->pid);
        answer(r->pid, r->id, -EPERM);
        free(me);
        req_free(r);
        return;
    }
    if (!g_agent) {
        // A text boot: sudo asked on its own terminal -- with no password
        // set, a y/N; with one, sudo sends it (above). On a graphical boot
        // with the desktop gone there is no one to ask.
        if (!g_agent_ever && !needs_pw && (flags & UPRIV_SELF_CONSENT)) carry_out(r);
        else answer(r->pid, r->id, -ENXIO);
        free(me);
        req_free(r);
        return;
    }
    // At the machine, through the desktop's own prompt -- which no other
    // program can press (only remoted injects input) -- login.admin_prompt
    // may let Allow alone answer.
    if (needs_pw && !force_pw) {
        int choice = admin_prompt();
        if (ADMIN_PROMPT[choice].keep_min < 0) needs_pw = 0;
        else if ((r->given_at = kept_since(w.uid)) != 0) needs_pw = 0;
    }
    if (needs_pw) {
        r->tries = UPRIV_AUTH_TRIES;
        r->name = me;   // the request owns it now
        me = 0;
    }
    free(me);
    static char rec[4096];   // privd is one thread; upriv_prompt's buf
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
        // The password the prompt asked for: after the flags, NUL-ended. An
        // Allow with NONE (an agent told to answer by itself) is a Cancel.
        const char *pw = (const char *)m->data + sizeof allow;
        uint32_t n = m->len - (uint32_t)sizeof allow;
        if (allow && r->tries && (n == 0 || pw[n - 1] != '\0')) allow = 0;
        if (allow && r->tries) {
            if (penalized(r->uid, g_agent) || !pass_check(r->name, pw)) {
                logf1("privd: a wrong password for %s (pid %d)\n", r->name, r->pid);
                if (--r->tries > 0) {
                    send_later(g_agent, UPRIV_T_RETRY, r->prompt, (int32_t)r->tries, r->uid);
                    return;
                }
                req_unlink(r);
                send_later(g_agent, UPRIV_T_CANCEL, r->prompt, 0, r->uid);
                send_later(r->pid, UPRIV_T_ANSWER, r->id, -EACCES, r->uid);
                req_free(r);
                return;
            }
            // Right: the agent's dialog, waiting BUSY, closes -- and a
            // `remember` choice keeps it for the next prompts.
            ubulk_server_send(&g_srv, g_agent, UPRIV_T_CANCEL, r->prompt, "", 0, 1000);
            kept_remember(r->uid);
        }
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
    purge_later(pid);
    if (pid == g_agent) {
        g_agent = 0;
        kept_forget();   // a sign-out, or the desktop gone
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
            if (m.type == UPRIV_T_SPAWN || m.type == UPRIV_T_ACTION || m.type == UPRIV_T_AUTH ||
                m.type == UPRIV_T_LOGIN)
                handle_request(&m);
            else if (m.type == UPRIV_T_AGENT) handle_agent(m.pid);
            else if (m.type == UPRIV_T_VERDICT) handle_verdict(&m);
            else if (m.type == UPRIV_T_FORGET && m.pid == g_agent && g_agent) kept_forget();
            memset(buf, 0, m.len);   // a record may have carried a password
        }
        ubulk_server_wait_change(&g_srv, run_later(1000));
    }
}
