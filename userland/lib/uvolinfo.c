// A mounted volume's or share's facts -- see uvolinfo.h.
#include "lib/uvolinfo.h"
#include "lib/unetmount.h"
#include "lib/human.h"
#include "rt/sys.h"
#include "query_abi.h"
#include "userfs_abi.h"
#include "kpath.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

void uvolinfo_name(const char *point, char *out, int cap) {
    if (!strcmp(point, "/"))     { strlcpy(out, "System", (size_t)cap); return; }
    if (!strcmp(point, "/boot")) { strlcpy(out, "Boot", (size_t)cap); return; }
    if (!strcmp(point, "/tmp"))  { strlcpy(out, "Temporary", (size_t)cap); return; }
    const char *base = point;
    for (const char *s = point; *s; s++) if (*s == '/' && s[1]) base = s + 1;
    strlcpy(out, base, (size_t)cap);
}

static void fill(struct uvolinfo *v, const struct query_fsinfo *fs) {
    memset(v, 0, sizeof *v);
    strlcpy(v->point, fs->point, sizeof v->point);
    uvolinfo_name(fs->point, v->label, sizeof v->label);
    strlcpy(v->fs, fs->name, sizeof v->fs);
    strlcpy(v->device, fs->device, sizeof v->device);
    v->remote = (fs->flags & QUERY_FS_REMOTE) != 0;
    v->readonly = (fs->flags & QUERY_FS_RDONLY) != 0;
    v->persistent = (fs->flags & QUERY_FS_PERSISTENT) != 0;
    v->root = (fs->flags & QUERY_FS_ROOT) != 0;
    v->disks_mounted = !strncmp(fs->point, "/mnt/", 5);
    v->total = fs->total_bytes;
    v->used = fs->used_bytes;
    if (!v->remote) return;
    strlcpy(v->source, fs->source, sizeof v->source);
    v->state = (int)fs->remote_state;
    strlcpy(v->note, fs->note, sizeof v->note);
    // "//nas:445/media" -> "nas"
    const char *h = fs->source[0] == '/' && fs->source[1] == '/' ? fs->source + 2 : fs->source;
    int n = 0;
    while (h[n] && h[n] != '/' && h[n] != ':' && n < (int)sizeof v->server - 1) n++;
    memcpy(v->server, h, (size_t)n);
    v->server[n] = '\0';
    // Saved is a file in /etc/mounts.d (unetmount_is_session(), without
    // its second walk of the mounts); one read gives the sign-in too.
    struct unetmount m;
    int saved = !strncmp(fs->point, UNETMOUNT_ROOT "/", strlen(UNETMOUNT_ROOT) + 1) &&
                unetmount_load(k_path_basename(fs->point), &m);
    v->session = !saved;
    if (saved) {
        strlcpy(v->user, m.user, sizeof v->user);
        strlcpy(v->domain, m.domain, sizeof v->domain);
        v->remembered = m.hash[0] != '\0';
    }
}

// The mount whose point is `path` (exact) or holds it (longest prefix).
static int find(const char *path, int exact, struct uvolinfo *out) {
    struct query_fsinfo fs, best;
    int best_len = -1, len = (int)strlen(path);
    for (int i = 0;; i++) {
        int n = sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs);
        if (n <= 0) break;
        if ((unsigned)n < sizeof fs || !(fs.flags & QUERY_FS_MOUNTED)) continue;
        int ml = (int)strlen(fs.point);
        int hit = exact ? !strcmp(fs.point, path)
                        : ml == 1 || (!strncmp(path, fs.point, (size_t)ml) && (ml == len || path[ml] == '/'));
        if (hit && ml > best_len) { best = fs; best_len = ml; }
    }
    if (best_len < 0) return 0;
    fill(out, &best);
    return 1;
}

int uvolinfo_load(const char *point, struct uvolinfo *out) { return point && find(point, 1, out); }
int uvolinfo_of(const char *path, struct uvolinfo *out)   { return path && path[0] == '/' && find(path, 0, out); }

const char *uvolinfo_state_text(int state) {
    switch (state) {
    case USERFS_ST_IDLE:        return "Not connected -- the next open connects";
    case USERFS_ST_CONNECTING:  return "Connecting";
    case USERFS_ST_CONNECTED:   return "Connected";
    case USERFS_ST_NEED_LOGIN:  return "Needs a password";
    case USERFS_ST_UNREACHABLE: return "No answer";
    case USERFS_ST_FAILED:      return "Failed";
    case USERFS_ST_NO_SERVER:   return "Stopped -- its server program has exited";
    default:                    return "Unknown";
    }
}

void uvolinfo_address(const struct uvolinfo *v, char *out, int cap) {
    if (v->remote) snprintf(out, (size_t)cap, "smb:%s", v->source);
    else strlcpy(out, v->point, (size_t)cap);
}

void uvolinfo_kind_text(const struct uvolinfo *v, char *out, int cap) {
    if (v->remote)
        snprintf(out, (size_t)cap, "Network share%s", v->readonly ? " - read-only" : "");
    else if (!v->persistent)
        snprintf(out, (size_t)cap, "Volume in memory - %s", v->fs);
    else
        snprintf(out, (size_t)cap, "Volume - %s%s%s", v->fs, v->device[0] ? " on " : "", v->device);
}

void uvolinfo_space_text(const struct uvolinfo *v, char *out, int cap) {
    if (!v->total) { out[0] = '\0'; return; }
    char f[16], t[16];
    human_size(f, sizeof f, v->total > v->used ? v->total - v->used : 0);
    human_size(t, sizeof t, v->total);
    snprintf(out, (size_t)cap, "%s free of %s", f, t);
}

uint64_t uvolinfo_free(const struct uvolinfo *v) {
    return v->total > v->used ? v->total - v->used : 0;
}

uint64_t uvolinfo_room(const struct uvolinfo *v) {
    uint64_t f = uvolinfo_free(v);
    if (!v->total || f <= (1ull << 20)) return 0;
    return (f - (1ull << 20)) / 257 * 256;
}

int uvolinfo_fits(const struct uvolinfo *v, uint64_t bytes) {
    return v->total && bytes <= uvolinfo_room(v);
}

int uvolinfo_used_pm(const struct uvolinfo *v) {
    if (!v->total) return -1;
    uint64_t u = v->used > v->total ? v->total : v->used;
    return (int)(u * 1000 / v->total);
}

static int row(struct uvolinfo_row *out, int n, int cap, const char *k, const char *val) {
    if (n >= cap) return n;
    strlcpy(out[n].key, k, sizeof out[n].key);
    strlcpy(out[n].value, val, sizeof out[n].value);
    return n + 1;
}

int uvolinfo_rows(const struct uvolinfo *v, struct uvolinfo_row *out, int cap) {
    int n = 0;
    char b[96];
    if (v->remote) {
        uvolinfo_address(v, b, sizeof b);
        n = row(out, n, cap, "Address", b);
        n = row(out, n, cap, "Opens at", v->point);
        n = row(out, n, cap, "Kept", v->session ? "this session only" : "saved - opens at every start");
        // The server's own word ("SMB 3.1.1, signed") when connected;
        // its reason ("No answer from nas") when not.
        const char *st = uvolinfo_state_text(v->state);
        if (v->note[0]) snprintf(b, sizeof b, "%s - %s", st, v->note);
        else strlcpy(b, st, sizeof b);
        n = row(out, n, cap, "Status", b);
        if (!v->session) {
            if (!v->user[0]) strlcpy(b, "nobody (anonymous)", sizeof b);
            else if (v->domain[0]) snprintf(b, sizeof b, "%s\\%s", v->domain, v->user);
            else strlcpy(b, v->user, sizeof b);
            n = row(out, n, cap, "Signed in as", b);
            if (v->user[0] && strcmp(v->user, "guest"))
                n = row(out, n, cap, "Password", v->remembered ? "remembered" : "asked for each time");
        }
    } else {
        n = row(out, n, cap, "Mounted at", v->point);
        snprintf(b, sizeof b, "%s, %s", v->fs, v->readonly ? "read-only" : "read-write");
        n = row(out, n, cap, "Filesystem", b);
        if (v->device[0]) n = row(out, n, cap, "Device", v->device);
        n = row(out, n, cap, "Kept", v->persistent ? "on disk" : "in memory - gone at shutdown");
    }
    if (v->total) {
        char u[16], f[16], t[16];
        human_size(u, sizeof u, v->used > v->total ? v->total : v->used);
        human_size(f, sizeof f, v->total > v->used ? v->total - v->used : 0);
        human_size(t, sizeof t, v->total);
        snprintf(b, sizeof b, "%s used, %s free of %s", u, f, t);
        n = row(out, n, cap, "Space", b);
    } else if (v->remote) {
        n = row(out, n, cap, "Space", "known once it connects");
    }
    return n;
}

int uvolinfo_disconnect(const char *point) {
    struct userfs_ctl *c = calloc(1, sizeof *c);   // a 512-byte payload: not on a ring-3 frame
    if (!c) return -ENOMEM;
    strlcpy(c->point, point, sizeof c->point);
    c->code = USERFS_CTL_RECONNECT;
    int64_t rc = sys_userfs(USERFS_CTL, (uint64_t)(uintptr_t)c, 0);
    free(c);
    return rc < 0 ? -sys_errno() : 0;
}
