#ifndef ULIB_UREMOTE_H
#define ULIB_UREMOTE_H

#include <stdint.h>
#include <stddef.h>
#include "query_abi.h"   // struct query_remotesess

#ifdef __cplusplus
extern "C" {
#endif

// The remote desktop, from any program: /etc/remote.conf (what
// /bin/remoted serves and who may connect), the sessions open now, and
// the two things anyone may do to one -- end it, or make it view only.
// /bin/remoted, System Settings' Remote Desktop page and the tray flyout
// share it, so the three cannot disagree about what the file says.
//
// BOTH FILES ARE ROOT'S (users D3b2a): remote.conf 0644, and the
// passwords in /etc/remote.secret, 0600 -- remote.conf says only whether
// one is set (`password_set`). Anyone else changes them through privd's
// UPRIV_ACT_REMOTEDESKTOP, an administrator's right, asked once per
// request (uremote_request()).
//
// A SESSION IS ACTED ON BY SIGNAL, to its leader's pid: SIGTERM ends it
// (the kernel releases what its viewer held), SIGUSR1 makes it view only,
// SIGUSR2 gives control back. Signals rather than a channel because they
// need nothing open, they reach a telnet shell just as well (SIGTERM),
// and a session that is wedged still dies.

#define UREMOTE_CONF "/etc/remote.conf"
#define UREMOTE_SECRET "/etc/remote.secret"   // [vnc] password=, root's and 0600
#define UREMOTE_PASSWORD_MAX 64
#define UREMOTE_TRUSTED_MAX 16
#define UREMOTE_LABEL_MAX 32

enum uremote_when { UREMOTE_ASK, UREMOTE_ASK_UNLESS_TRUSTED, UREMOTE_ALWAYS };
enum uremote_from { UREMOTE_FROM_NETWORK, UREMOTE_FROM_ANYWHERE };

struct uremote_net {                 // one trusted address or subnet
    uint32_t ip, mask;
    char text[24];                   // as written: "192.168.1.0/24"
    char label[UREMOTE_LABEL_MAX];   // "" when it has none
};

// VNC's encryption (VeNCrypt): offered first and the plain password still
// accepted; required; or not offered.
enum uremote_enc { UREMOTE_ENC_PREFER, UREMOTE_ENC_REQUIRE, UREMOTE_ENC_OFF };

struct uremote_proto {
    int enabled;
    enum uremote_enc encryption;
    char fingerprint[100];           // the certificate's, as remoted last recorded it
    int port;
    char user[32];                   // RDP's; VNC has no user name
    char password[UREMOTE_PASSWORD_MAX];   // "" unless the reader is root
    int password_set;                // remote.conf says one is set: anyone may read it
};

struct uremote_conf {
    struct uremote_proto vnc, rdp;
    enum uremote_when when;
    enum uremote_from from;
    struct uremote_net trusted[UREMOTE_TRUSTED_MAX];
    int ntrusted;
    int view_only;
};

// Reads the file; anything missing is its default (everything off). The
// passwords come from UREMOTE_SECRET alone, which only root can read.
void uremote_load(struct uremote_conf *c);

// Writing, ROOT'S, one key at a time -- the rest of the file is kept. Each
// returns 0 on success, -1 if the write failed. `proto` is "vnc" or "rdp".
// A password "" removes it.
int uremote_set_enabled(const char *proto, int on);
int uremote_set_port(const char *proto, int port);
int uremote_set_password(const char *proto, const char *pw);
int uremote_set_encryption(const char *proto, enum uremote_enc e);
int uremote_set_when(enum uremote_when w);
int uremote_set_from(enum uremote_from f);
// Adds `addr` ("a.b.c.d" or "a.b.c.d/bits") with an optional label, or
// relabels it if it is there. -1 for an address that does not parse.
int uremote_trust(const char *addr, const char *label);
int uremote_untrust(const char *addr);

// ---- a CHANGE, as privd's UPRIV_ACT_REMOTEDESKTOP carries it ------------
//
// A key from a FIXED set and its value -- "=..." to set, "-" to take away:
//
//   vnc.enabled     =yes | =no
//   vnc.port        =1..65535
//   vnc.encryption  =prefer | =require | =off
//   vnc.password    =<text>, or "-" for none
//   when            =ask | =ask-unless-trusted | =always
//   from            =network | =anywhere
//   trust:<addr>    =<label, may be empty> trusts it; "-" stops trusting it
//
// A password or a label is one line the file can hold: no control
// character, no '#' (a comment there), at most its _MAX less one.
#define UREMOTE_CHANGE_PASSWORD "vnc.password"
#define UREMOTE_CHANGE_TRUST    "trust:"
// 1 when `key` and `value` are a change above -- what privd checks before
// anyone is asked.
int uremote_change_ok(const char *key, const char *value);
// The prompt's row for it, against `now`: "VNC" / "Off -> On", "Port" /
// "5900 -> 5901", "Password" / "changed" -- never a password's value --
// and "Trusted" / "+ 192.168.1.20".
void uremote_change_row(const struct uremote_conf *now, const char *key, const char *value,
                        char *rkey, size_t kcap, char *rval, size_t vcap);
// Makes one change, as root. 0, or -1 (refused, or the write failed).
int uremote_change_apply(const char *key, const char *value);
// Makes `n` changes (`kv`: key, value, key, value...): root at once,
// anyone else through privd in ONE prompt -- more than UPRIV_PROMPT_ROWS
// is refused (E2BIG) before anyone is asked. BLOCKS while the prompt is
// up. 0, or -1 with errno (base/upriv.h's).
int uremote_request(const char *const *kv, int n);

// "a.b.c.d" or "a.b.c.d/bits"; 1 on success. Refuses rather than guesses.
int uremote_parse_net(const char *s, struct uremote_net *out);
void uremote_fmt_ip(uint32_t ip, char *out, size_t cap);

int uremote_trusted(const struct uremote_conf *c, uint32_t ip);
// Is `ip` on a network one of this machine's cards is on?
int uremote_local(uint32_t ip);

// The sessions open now (QUERY_REMOTESESS), into `*out` (grown to them,
// kept by the caller and reused: free() it when done). Returns how many;
// out of memory, the ones that fit.
int uremote_sessions(struct query_remotesess **out, int *cap);
// A remote DESKTOP session, as opposed to a telnet shell.
int uremote_is_desktop(const struct query_remotesess *s);
// What a session's row says: "VNC, full control" / "Telnet shell".
void uremote_describe(const struct query_remotesess *s, char *out, size_t cap);
int uremote_disconnect(int pid);
int uremote_set_view_only(int pid, int on);

#ifdef __cplusplus
}
#endif

#endif
