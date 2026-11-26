// privd's client and agent halves -- base/upriv.h has the contract.
#include "base/upriv.h"
#include "base/ubulk.h"
#include "base/uself.h"
#include "base/uaskpass.h"
#include "query_abi.h"   // QUERY_PROCPATH_MAX
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // environ

static uint32_t g_next_id = 1;

// The request's bytes: u32 flags, then each string with its NUL.
static char *pack(unsigned flags, const char *first, const char *const *rest, uint32_t *len) {
    size_t n = sizeof(uint32_t) + strlen(first) + 1;
    for (int i = 0; rest && rest[i]; i++) n += strlen(rest[i]) + 1;
    if (n > UBULK_REC_MAX) { errno = E2BIG; return 0; }
    char *b = malloc(n);
    if (!b) { errno = ENOMEM; return 0; }
    uint32_t f = flags;
    memcpy(b, &f, sizeof f);
    size_t at = sizeof f;
    size_t k = strlen(first) + 1;
    memcpy(b + at, first, k);
    at += k;
    for (int i = 0; rest && rest[i]; i++) {
        k = strlen(rest[i]) + 1;
        memcpy(b + at, rest[i], k);
        at += k;
    }
    *len = (uint32_t)n;
    return b;
}

// Sends one request and waits for its answer: 0, or -1 with errno.
static int ask(uint32_t type, char *rec, uint32_t len) {
    struct ubulk_client c;
    if (ubulk_client_open(&c, UPRIV_SERVICE) < 0) {
        memset(rec, 0, len);
        free(rec);
        errno = ENOENT;
        return -1;
    }
    uint32_t id = __atomic_fetch_add(&g_next_id, 1, __ATOMIC_RELAXED);
    int rc = ubulk_send(&c, type, id, rec, len, 5000);
    memset(rec, 0, len);   // it may carry a password
    free(rec);
    if (rc < 0) { ubulk_client_close(&c); errno = EIO; return -1; }
    char *buf = malloc(UBULK_REC_MAX);
    if (!buf) { ubulk_client_close(&c); errno = ENOMEM; return -1; }
    int answer = -ETIMEDOUT;
    for (int waited = 0; waited < UPRIV_TIMEOUT_MS; ) {
        struct ubulk_msg m;
        if (ubulk_client_recv(&c, &m, buf, UBULK_REC_MAX) == 1) {
            if (m.type == UPRIV_T_ANSWER && m.id == id && m.len >= sizeof(int32_t)) {
                int32_t v;
                memcpy(&v, m.data, sizeof v);
                answer = v;
                break;
            }
            continue;
        }
        if (!ubulk_client_server_alive(&c)) { answer = -EIO; break; }
        ubulk_client_wait(&c, 1000);
        waited += 1000;
    }
    free(buf);
    ubulk_client_close(&c);
    if (answer < 0) { errno = -answer; return -1; }
    return 0;
}

int upriv_spawn(const char *path, char *const *argv, const struct sys_spawn_opts *o,
                unsigned flags) {
    if (!path || !argv || !argv[0]) { errno = EINVAL; return -1; }
    uint32_t len;
    char *rec = pack(flags, path, (const char *const *)argv, &len);
    if (!rec || ask(UPRIV_T_SPAWN, rec, len) < 0) return -1;
    struct sys_spawn_opts so;
    if (o) so = *o;
    else { sys_spawn_opts_init(&so); so.env = environ; }
    so.argv = argv;
    so.flags |= SPAWN_ARGV | SPAWN_ELEVATED;
    return sys_spawn_opts(path, &so);
}

int upriv_spawn_password(const char *path, char *const *argv, const struct sys_spawn_opts *o,
                         const char *password) {
    if (!path || !argv || !argv[0] || !password) { errno = EINVAL; return -1; }
    int n = 0;
    while (argv[n]) n++;
    // password, path, argv... -- privd takes the first string off.
    const char **rest = calloc((size_t)n + 2, sizeof *rest);
    if (!rest) { errno = ENOMEM; return -1; }
    rest[0] = path;
    for (int i = 0; i < n; i++) rest[i + 1] = argv[i];
    uint32_t len;
    char *rec = pack(UPRIV_WITH_PASSWORD, password, rest, &len);
    free(rest);
    if (!rec) return -1;
    int rc = ask(UPRIV_T_SPAWN, rec, len);   // ask() wipes and frees rec
    if (rc < 0) return -1;
    struct sys_spawn_opts so;
    if (o) so = *o;
    else { sys_spawn_opts_init(&so); so.env = environ; }
    so.argv = argv;
    so.flags |= SPAWN_ARGV | SPAWN_ELEVATED;
    return sys_spawn_opts(path, &so);
}

int upriv_auth(const char *account, const char *password) {
    if (!account || !password) { errno = EINVAL; return -1; }
    const char *rest[] = { password, 0 };
    uint32_t len;
    char *rec = pack(0, account, rest, &len);
    return rec ? ask(UPRIV_T_AUTH, rec, len) : -1;
}

int upriv_login(const char *account, const char *password) {
    if (!account || !password) { errno = EINVAL; return -1; }
    const char *rest[] = { password, 0 };
    uint32_t len;
    char *rec = pack(0, account, rest, &len);
    return rec ? ask(UPRIV_T_LOGIN, rec, len) : -1;
}

int upriv_has_password(const char *account) {
    if (!account) { errno = EINVAL; return -1; }
    const char *rest[] = { "", 0 };
    uint32_t len;
    char *rec = pack(UPRIV_PROBE, account, rest, &len);
    if (!rec) return -1;
    if (ask(UPRIV_T_AUTH, rec, len) == 0) return 0;
    return errno == EACCES ? 1 : -1;
}

int upriv_passwd(const char *account, const char *current, const char *password) {
    if (!account || !password) { errno = EINVAL; return -1; }
    const char *args[] = { account, current ? current : "", password, 0 };
    return upriv_action(UPRIV_ACT_PASSWD, args, 0);
}

int upriv_action(const char *action, const char *const *args, unsigned flags) {
    if (!action) { errno = EINVAL; return -1; }
    uint32_t len;
    char *rec = pack(flags, action, args, &len);
    return rec ? ask(UPRIV_T_ACTION, rec, len) : -1;
}

int upriv_action_password(const char *action, const char *const *args, const char *password) {
    if (!action || !password) { errno = EINVAL; return -1; }
    int n = 0;
    while (args && args[n]) n++;
    // password, action, args... -- privd takes the first string off.
    const char **rest = calloc((size_t)n + 2, sizeof *rest);
    if (!rest) { errno = ENOMEM; return -1; }
    rest[0] = action;
    for (int i = 0; i < n; i++) rest[i + 1] = args[i];
    uint32_t len;
    char *rec = pack(UPRIV_WITH_PASSWORD, password, rest, &len);
    free(rest);
    return rec ? ask(UPRIV_T_ACTION, rec, len) : -1;
}

static int ask_here(const char *what) {
    if (!isatty(0)) return 0;
    fprintf(stderr, "%s as administrator? [y/N] ", what);
    fflush(stderr);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) return 0;
    return line[0] == 'y' || line[0] == 'Y';
}

int upriv_action_tty(const char *what, const char *action, const char *const *args) {
    if (geteuid() == 0) return upriv_action(action, args, 0);
    struct passwd *pw = getpwuid(getuid());
    char me[64] = "";
    if (pw) snprintf(me, sizeof me, "%s", pw->pw_name);
    int has = me[0] ? upriv_has_password(me) : 0;
    if (has != 1) {
        int rc = upriv_action(action, args, 0);
        if (rc < 0 && errno == ENXIO && ask_here(what)) rc = upriv_action(action, args, UPRIV_SELF_CONSENT);
        return rc;
    }
    if (!isatty(0)) { errno = ENOTTY; return -1; }
    char prompt[96];
    char *typed = malloc(256);
    if (!typed) { errno = ENOMEM; return -1; }
    snprintf(prompt, sizeof prompt, "[%s] password for %s: ", what, me);
    int rc = -1;
    for (int i = 0; i < UPRIV_AUTH_TRIES; i++) {
        if (uaskpass(prompt, typed, 256) < 0) { errno = EINTR; break; }
        rc = upriv_action_password(action, args, typed);
        memset(typed, 0, 256);
        if (rc == 0 || errno != EACCES) break;
        if (i + 1 < UPRIV_AUTH_TRIES) fprintf(stderr, "Sorry, try again.\n");
    }
    int e = errno;
    free(typed);
    errno = e;
    return rc;
}

const char *upriv_strerror(int e) {
    return e == EACCES    ? "the password was wrong"
         : e == EPERM     ? "this account may not do that as administrator"
         : e == ECANCELED ? "not allowed"
         : e == EINTR     ? "cancelled"
         : e == ENXIO     ? "no one to ask: the desktop is not running"
         : e == ENOENT    ? "privd is not running"
         : e == ETIMEDOUT ? "no answer"
         : e == ENOTTY    ? "a password is needed, and there is no terminal to ask on"
         : strerror(e);
}

int upriv_elevate_self(char **argv) {
    if (geteuid() == 0) return 0;
    char self[QUERY_PROCPATH_MAX];
    if (uself_path(self, sizeof self) < 0) { errno = ENOENT; return -1; }
    int pid = upriv_spawn(self, argv, 0, 0);
    if (pid < 0) return -1;
    // Waits, so whoever started this pid -- the desktop's launch list, a
    // test -- sees it live as long as the program is.
    int code = 1;
    sys_waitpid(pid, &code);
    exit(code);
}

// --- the agent --------------------------------------------------------------

struct ubulk_client *upriv_agent_open(void) {
    struct ubulk_client *c = malloc(sizeof *c);
    if (!c) return 0;
    if (ubulk_client_open(c, UPRIV_SERVICE) < 0 ||
        ubulk_send(c, UPRIV_T_AGENT, 0, "", 0, 1000) < 0) {
        free(c);
        return 0;
    }
    return c;
}

// The next field of [*at, end). Always a pointer INTO the buffer, whose
// byte at `end` is a NUL: past the end it is `end` itself, an empty
// string a caller may re-base like any other field.
static const char *field(const char **at, const char *end) {
    const char *s = *at;
    if (s >= end) return end;
    const char *nul = memchr(s, '\0', (size_t)(end - s));
    *at = nul ? nul + 1 : end;
    return s;
}

int upriv_agent_next(struct ubulk_client *a, struct upriv_prompt *p) {
    static char buf[UBULK_REC_MAX];   // the agent is one thread
    struct ubulk_msg m;
    while (ubulk_client_recv(a, &m, buf, sizeof buf) == 1) {
        if (m.type == UPRIV_T_CANCEL) { p->id = m.id; return -1; }
        if (m.type == UPRIV_T_RETRY) {
            p->id = m.id;
            p->tries_left = 0;
            if (m.len >= sizeof(uint32_t)) memcpy(&p->tries_left, m.data, sizeof(uint32_t));
            return 2;
        }
        if (m.type != UPRIV_T_PROMPT) continue;
        uint32_t n = m.len < sizeof p->buf ? m.len : (uint32_t)sizeof p->buf - 1;
        memcpy(p->buf, m.data, n);
        p->buf[n] = '\0';
        const char *at = p->buf, *end = p->buf + n;
        p->id = m.id;
        p->headline = field(&at, end);
        p->body = field(&at, end);
        p->action = field(&at, end);
        p->program = field(&at, end);
        p->asker = field(&at, end);
        p->password_for = field(&at, end);
        p->admins = field(&at, end);
        p->nrows = 0;
        while (at < end && p->nrows < UPRIV_PROMPT_ROWS) {
            p->row_key[p->nrows] = field(&at, end);
            p->row_val[p->nrows] = field(&at, end);
            p->nrows++;
        }
        p->tries_left = UPRIV_AUTH_TRIES;
        return 1;
    }
    return 0;
}

void upriv_agent_answer(struct ubulk_client *a, uint32_t id, int allow) {
    uint32_t v = allow ? 1 : 0;
    ubulk_send(a, UPRIV_T_VERDICT, id, &v, sizeof v, 1000);
}

void upriv_agent_answer_password(struct ubulk_client *a, uint32_t id, const char *password) {
    size_t n = strlen(password) + 1;
    if (n > UBULK_REC_MAX - sizeof(uint32_t)) { upriv_agent_answer(a, id, 0); return; }
    char *rec = malloc(sizeof(uint32_t) + n);
    if (!rec) { upriv_agent_answer(a, id, 0); return; }
    uint32_t v = 1;
    memcpy(rec, &v, sizeof v);
    memcpy(rec + sizeof v, password, n);
    ubulk_send(a, UPRIV_T_VERDICT, id, rec, (uint32_t)(sizeof v + n), 1000);
    memset(rec, 0, sizeof v + n);
    free(rec);
}

void upriv_agent_forget(struct ubulk_client *a) {
    if (a) ubulk_send(a, UPRIV_T_FORGET, 0, "", 0, 1000);
}

void upriv_agent_close(struct ubulk_client *a) {
    if (!a) return;
    ubulk_client_close(a);
    free(a);
}
