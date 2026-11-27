// See net/uremote.h.
#include "base/ugrow.h"
#include "base/upriv.h"   // a remote session from an account that is not root
#include "net/uremote.h"
#include "base/uconf.h"
#include "rt/sys.h"
#include "signal_abi.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void uremote_fmt_ip(uint32_t ip, char *out, size_t cap) {
    snprintf(out, cap, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

int uremote_parse_net(const char *s, struct uremote_net *out) {
    const char *start = s;
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v > 255) return 0;
        ip = (ip << 8) | (uint32_t)v;
        s = end;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    int bits = 32;
    if (*s == '/') {
        char *end;
        long v = strtol(s + 1, &end, 10);
        if (end == s + 1 || v < 0 || v > 32) return 0;
        bits = (int)v;
        s = end;
    }
    if (*s) return 0;
    memset(out, 0, sizeof *out);
    out->mask = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
    out->ip = ip & out->mask;
    snprintf(out->text, sizeof out->text, "%s", start);
    return 1;
}

static int yes(const char *v) {
    return !strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true");
}

static void proto(const struct etc_config_buf *b, const char *sect, struct uremote_proto *p,
                  int port) {
    char v[UREMOTE_PASSWORD_MAX];
    p->enabled = etc_config_buf_get_in(b, sect, "enabled", v, sizeof v) && yes(v);
    p->port = port;
    if (etc_config_buf_get_in(b, sect, "port", v, sizeof v)) {
        int n = atoi(v);
        if (n > 0 && n < 65536) p->port = n;
    }
    p->password_set = etc_config_buf_get_in(b, sect, "password_set", v, sizeof v) && yes(v);
    p->encryption = UREMOTE_ENC_PREFER;
    if (etc_config_buf_get_in(b, sect, "encryption", v, sizeof v)) {
        if (!strcmp(v, "require")) p->encryption = UREMOTE_ENC_REQUIRE;
        else if (!strcmp(v, "off")) p->encryption = UREMOTE_ENC_OFF;
    }
    p->fingerprint[0] = 0;
    etc_config_buf_get_in(b, sect, "fingerprint", p->fingerprint, sizeof p->fingerprint);
    snprintf(p->user, sizeof p->user, "toy");
    etc_config_buf_get_in(b, sect, "user", p->user, sizeof p->user);
}

void uremote_load(struct uremote_conf *c) {
    memset(c, 0, sizeof *c);
    c->when = UREMOTE_ASK_UNLESS_TRUSTED;
    c->from = UREMOTE_FROM_NETWORK;
    c->vnc.port = 5900;
    c->rdp.port = 3389;
    struct etc_config_buf b;
    if (!uconf_load(UREMOTE_CONF, &b)) return;
    proto(&b, "vnc", &c->vnc, 5900);
    proto(&b, "rdp", &c->rdp, 3389);
    // The passwords: root's file, so anyone else reads none.
    struct etc_config_buf sec;
    if (geteuid() == 0 && uconf_load(UREMOTE_SECRET, &sec)) {
        etc_config_buf_get_in(&sec, "vnc", "password", c->vnc.password, sizeof c->vnc.password);
        etc_config_buf_get_in(&sec, "rdp", "password", c->rdp.password, sizeof c->rdp.password);
        etc_config_buf_free(&sec);
    }

    char v[256];
    if (etc_config_buf_get(&b, "when", v, sizeof v)) {
        if (!strcmp(v, "ask")) c->when = UREMOTE_ASK;
        else if (!strcmp(v, "always")) c->when = UREMOTE_ALWAYS;
    }
    if (etc_config_buf_get(&b, "from", v, sizeof v) && !strcmp(v, "anywhere"))
        c->from = UREMOTE_FROM_ANYWHERE;
    c->view_only = etc_config_buf_get(&b, "view_only", v, sizeof v) && yes(v);

    // `trusted = 192.168.1.20 192.168.1.0/24`, words separated by spaces
    // or commas, and an optional label per address under [labels]. A word
    // that does not parse is skipped -- the rest still means what it says.
    if (etc_config_buf_get(&b, "trusted", v, sizeof v)) {
        char *save = 0;
        for (char *w = strtok_r(v, " ,", &save); w && c->ntrusted < UREMOTE_TRUSTED_MAX;
             w = strtok_r(0, " ,", &save)) {
            struct uremote_net *n = &c->trusted[c->ntrusted];
            if (!uremote_parse_net(w, n)) continue;
            etc_config_buf_get_in(&b, "labels", w, n->label, sizeof n->label);
            c->ntrusted++;
        }
    }
    etc_config_buf_free(&b);
}

// --- writing ---------------------------------------------------------------

static int ok(int r) { return r ? 0 : -1; }

int uremote_set_enabled(const char *p, int on) {
    return ok(uconf_set_in(UREMOTE_CONF, p, "enabled", on ? "yes" : "no"));
}

int uremote_set_port(const char *p, int port) {
    if (port <= 0 || port > 65535) return -1;
    char v[8];
    snprintf(v, sizeof v, "%d", port);
    return ok(uconf_set_in(UREMOTE_CONF, p, "port", v));
}

// THE SECRET IS WRITTEN 0600 FROM ITS FIRST BYTE: a new file's mode is
// the umask's (open()'s mode never reaches the kernel), so the umask is
// narrowed around the write, and the chmod covers a file made before.
int uremote_set_password(const char *p, const char *pw) {
    mode_t was = umask(077);
    int r = pw[0] ? uconf_set_in(UREMOTE_SECRET, p, "password", pw)
                  : (uconf_set_in(UREMOTE_SECRET, p, "password", NULL), 1);
    umask(was);
    chmod(UREMOTE_SECRET, 0600);
    if (!r) return -1;
    uconf_set_in(UREMOTE_CONF, p, "password", NULL);   // where one lived before D3b2a
    return ok(uconf_set_in(UREMOTE_CONF, p, "password_set", pw[0] ? "yes" : "no"));
}

int uremote_set_encryption(const char *p, enum uremote_enc e) {
    static const char *const E[] = { "prefer", "require", "off" };
    return ok(uconf_set_in(UREMOTE_CONF, p, "encryption", E[e]));
}

int uremote_set_when(enum uremote_when w) {
    static const char *const W[] = { "ask", "ask-unless-trusted", "always" };
    return ok(uconf_set(UREMOTE_CONF, "when", W[w]));
}

int uremote_set_from(enum uremote_from f) {
    return ok(uconf_set(UREMOTE_CONF, "from", f == UREMOTE_FROM_ANYWHERE ? "anywhere" : "network"));
}

static int write_list(const struct uremote_conf *c) {
    char v[UREMOTE_TRUSTED_MAX * 24];
    v[0] = 0;
    for (int i = 0; i < c->ntrusted; i++) {
        if (i) strlcat(v, " ", sizeof v);
        strlcat(v, c->trusted[i].text, sizeof v);
    }
    if (!c->ntrusted) return ok(uconf_unset(UREMOTE_CONF, "trusted"));
    return ok(uconf_set(UREMOTE_CONF, "trusted", v));
}

int uremote_trust(const char *addr, const char *label) {
    struct uremote_net n;
    if (!uremote_parse_net(addr, &n)) return -1;
    struct uremote_conf c;
    uremote_load(&c);
    int have = 0;
    for (int i = 0; i < c.ntrusted; i++) if (!strcmp(c.trusted[i].text, n.text)) have = 1;
    if (!have) {
        if (c.ntrusted >= UREMOTE_TRUSTED_MAX) return -1;
        c.trusted[c.ntrusted++] = n;
        if (write_list(&c)) return -1;
    }
    if (label && label[0]) return ok(uconf_set_in(UREMOTE_CONF, "labels", n.text, label));
    return 0;
}

int uremote_untrust(const char *addr) {
    struct uremote_conf c;
    uremote_load(&c);
    int k = 0;
    for (int i = 0; i < c.ntrusted; i++)
        if (strcmp(c.trusted[i].text, addr)) c.trusted[k++] = c.trusted[i];
    if (k == c.ntrusted) return 0;
    c.ntrusted = k;
    uconf_set_in(UREMOTE_CONF, "labels", addr, NULL);   // a removal (uconf.h)
    return write_list(&c);
}

// --- a change, through privd -------------------------------------------------

// One line the file can hold, under `cap`: no control character, no '#'.
static int line_ok(const char *v, size_t cap) {
    if (strlen(v) >= cap) return 0;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++)
        if (*p < ' ' || *p == 0x7f || *p == '#') return 0;
    return 1;
}

static const char *const ENC_WORD[] = { "prefer", "require", "off" };
static const char *const ENC_LABEL[] = { "When the viewer offers it", "Required", "Off" };
static const char *const WHEN_WORD[] = { "ask", "ask-unless-trusted", "always" };
static const char *const WHEN_LABEL[] = { "Ask every time", "Ask unless trusted", "Always allow" };
static const char *const FROM_WORD[] = { "network", "anywhere" };
static const char *const FROM_LABEL[] = { "This network only", "Anywhere" };

static int word_index(const char *v, const char *const *w, int n) {
    for (int i = 0; i < n; i++) if (!strcmp(v, w[i])) return i;
    return -1;
}

static int port_of(const char *v) {
    char *end;
    long n = strtol(v, &end, 10);
    return end != v && !*end && n > 0 && n < 65536 ? (int)n : -1;
}

int uremote_change_ok(const char *key, const char *value) {
    if (!key || !value) return 0;
    int set = value[0] == '=';
    const char *v = value + 1;
    if (!set && strcmp(value, "-")) return 0;
    if (!strncmp(key, UREMOTE_CHANGE_TRUST, strlen(UREMOTE_CHANGE_TRUST))) {
        struct uremote_net n;
        const char *addr = key + strlen(UREMOTE_CHANGE_TRUST);
        return uremote_parse_net(addr, &n) && !strcmp(n.text, addr) &&
               (!set || line_ok(v, UREMOTE_LABEL_MAX));
    }
    if (!strcmp(key, UREMOTE_CHANGE_PASSWORD)) return !set || (v[0] && line_ok(v, UREMOTE_PASSWORD_MAX));
    if (!set) return 0;   // only a password and a trust are taken away
    if (!strcmp(key, "vnc.enabled")) return !strcmp(v, "yes") || !strcmp(v, "no");
    if (!strcmp(key, "vnc.port")) return port_of(v) > 0;
    if (!strcmp(key, "vnc.encryption")) return word_index(v, ENC_WORD, 3) >= 0;
    if (!strcmp(key, "when")) return word_index(v, WHEN_WORD, 3) >= 0;
    if (!strcmp(key, "from")) return word_index(v, FROM_WORD, 2) >= 0;
    return 0;
}

void uremote_change_row(const struct uremote_conf *c, const char *key, const char *value,
                        char *rk, size_t kcap, char *rv, size_t vcap) {
    const char *v = value[0] == '=' ? value + 1 : "";
    rk[0] = rv[0] = 0;
    if (!strncmp(key, UREMOTE_CHANGE_TRUST, strlen(UREMOTE_CHANGE_TRUST))) {
        const char *addr = key + strlen(UREMOTE_CHANGE_TRUST);
        snprintf(rk, kcap, "Trusted");
        if (value[0] != '=') snprintf(rv, vcap, "- %s", addr);
        else if (v[0]) snprintf(rv, vcap, "+ %s (%s)", addr, v);
        else snprintf(rv, vcap, "+ %s", addr);
    } else if (!strcmp(key, UREMOTE_CHANGE_PASSWORD)) {
        snprintf(rk, kcap, "Password");
        snprintf(rv, vcap, value[0] == '=' ? (c->vnc.password_set ? "changed" : "set") : "removed");
    } else if (!strcmp(key, "vnc.enabled")) {
        snprintf(rk, kcap, "VNC");
        snprintf(rv, vcap, "%s -> %s", c->vnc.enabled ? "On" : "Off", !strcmp(v, "yes") ? "On" : "Off");
    } else if (!strcmp(key, "vnc.port")) {
        snprintf(rk, kcap, "Port");
        snprintf(rv, vcap, "%d -> %s", c->vnc.port, v);
    } else if (!strcmp(key, "vnc.encryption")) {
        int i = word_index(v, ENC_WORD, 3);
        snprintf(rk, kcap, "Encryption");
        snprintf(rv, vcap, "%s -> %s", ENC_LABEL[c->vnc.encryption], i >= 0 ? ENC_LABEL[i] : v);
    } else if (!strcmp(key, "when")) {
        int i = word_index(v, WHEN_WORD, 3);
        snprintf(rk, kcap, "When someone connects");
        snprintf(rv, vcap, "%s -> %s", WHEN_LABEL[c->when], i >= 0 ? WHEN_LABEL[i] : v);
    } else if (!strcmp(key, "from")) {
        int i = word_index(v, FROM_WORD, 2);
        snprintf(rk, kcap, "Allow connections from");
        snprintf(rv, vcap, "%s -> %s", FROM_LABEL[c->from], i >= 0 ? FROM_LABEL[i] : v);
    }
}

int uremote_change_apply(const char *key, const char *value) {
    if (!uremote_change_ok(key, value)) return -1;
    int set = value[0] == '=';
    const char *v = value + 1;
    if (!strncmp(key, UREMOTE_CHANGE_TRUST, strlen(UREMOTE_CHANGE_TRUST))) {
        const char *addr = key + strlen(UREMOTE_CHANGE_TRUST);
        if (!set) return uremote_untrust(addr);
        if (uremote_trust(addr, v)) return -1;
        // An empty label takes a former one away (uremote_trust() keeps it).
        if (!v[0]) uconf_set_in(UREMOTE_CONF, "labels", addr, NULL);
        return 0;
    }
    if (!strcmp(key, UREMOTE_CHANGE_PASSWORD)) return uremote_set_password("vnc", set ? v : "");
    if (!strcmp(key, "vnc.enabled")) return uremote_set_enabled("vnc", !strcmp(v, "yes"));
    if (!strcmp(key, "vnc.port")) return uremote_set_port("vnc", port_of(v));
    if (!strcmp(key, "vnc.encryption")) return uremote_set_encryption("vnc", (enum uremote_enc)word_index(v, ENC_WORD, 3));
    if (!strcmp(key, "when")) return uremote_set_when((enum uremote_when)word_index(v, WHEN_WORD, 3));
    if (!strcmp(key, "from")) return uremote_set_from((enum uremote_from)word_index(v, FROM_WORD, 2));
    return -1;
}

int uremote_request(const char *const *kv, int n) {
    if (!kv || n <= 0) { errno = EINVAL; return -1; }
    for (int i = 0; i < n; i++)
        if (!uremote_change_ok(kv[2 * i], kv[2 * i + 1])) { errno = EINVAL; return -1; }
    if (geteuid() == 0) {
        for (int i = 0; i < n; i++)
            if (uremote_change_apply(kv[2 * i], kv[2 * i + 1])) { errno = EIO; return -1; }
        return 0;
    }
    // ONE prompt, all or nothing: what one dialog shows whole.
    if (n > UPRIV_PROMPT_ROWS) { errno = E2BIG; return -1; }
    const char *args[2 * UPRIV_PROMPT_ROWS + 1];
    for (int i = 0; i < 2 * n; i++) args[i] = kv[i];
    args[2 * n] = 0;
    return upriv_action(UPRIV_ACT_REMOTEDESKTOP, args, 0);
}

// --- who may connect -------------------------------------------------------

int uremote_trusted(const struct uremote_conf *c, uint32_t ip) {
    for (int i = 0; i < c->ntrusted; i++)
        if ((ip & c->trusted[i].mask) == c->trusted[i].ip) return 1;
    return 0;
}

int uremote_local(uint32_t ip) {
    struct query_netdev d;
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        if (!d.ip || !d.netmask) continue;
        if ((ip & (uint32_t)d.netmask) == ((uint32_t)d.ip & (uint32_t)d.netmask)) return 1;
    }
    return 0;
}

// --- sessions --------------------------------------------------------------

int uremote_sessions(struct query_remotesess **out, int *cap) {
    int n = 0;
    struct query_remotesess q;
    QUERY_FOREACH(QUERY_REMOTESESS, q, i) {
        if (!UGROW(*out, *cap, n + 1)) break;
        (*out)[n++] = q;
    }
    return n;
}

int uremote_is_desktop(const struct query_remotesess *s) { return !strcmp(s->comm, "remoted"); }

void uremote_describe(const struct query_remotesess *s, char *out, size_t cap) {
    if (s->status[0]) snprintf(out, cap, "%s", s->status);
    else if (uremote_is_desktop(s)) snprintf(out, cap, "Remote desktop");
    else if (!strcmp(s->comm, "telnetd")) snprintf(out, cap, "Telnet shell");
    else snprintf(out, cap, "%s", s->comm);
}

// A remote session is root's process: from another account the seat asks
// privd, which grants it without a prompt (base/upriv.h).
static int session_signal(int pid, int sig) {
    if (sys_kill(pid, sig) == 0) return 0;
    if (sys_errno() != EPERM) return -1;
    char p[16], s[8];
    snprintf(p, sizeof p, "%d", pid);
    snprintf(s, sizeof s, "%d", sig);
    const char *args[] = { p, s, 0 };
    return upriv_action(UPRIV_ACT_REMOTE, args, 0);
}

int uremote_disconnect(int pid) { return session_signal(pid, SIGTERM); }

int uremote_set_view_only(int pid, int on) { return session_signal(pid, on ? SIGUSR1 : SIGUSR2); }
