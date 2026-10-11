// smbfsd -- serve network shares as ordinary files under /net.
//
// TWO ROLES IN ONE BINARY:
//
//   smbfsd                    the SUPERVISOR (the `smbfsd` service). Keeps
//                             one worker per file in /etc/mounts.d,
//                             starting, stopping and restarting them as
//                             the files come and go.
//   smbfsd --serve NAME       a WORKER: mounts /net/NAME (userfs) and
//                             answers the kernel's requests for it from
//                             the share in /etc/mounts.d/NAME.
//   smbfsd --url URL POINT    a worker for a share nothing saved -- what
//                             `mount -t smb URL POINT` starts.
//
// ONE PROCESS PER SHARE, as FUSE runs one per filesystem: a share whose
// server hangs or a worker that crashes takes nothing else with it, and
// a worker is single-threaded because it serves one mount.
//
// LAZY: a worker mounts at once and connects on the FIRST request, so a
// boot never waits for a NAS -- Windows' "reconnect at sign-in" and
// systemd's automount behave the same. The mount's state (QUERY_FSINFO's
// remote_state) says what it is doing, for the File Manager.
//
// A worker MUST NOT touch its own mount (abi/userfs_abi.h); everything
// it reads is under /etc and /run.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rt/sys.h"
#include "userfs_abi.h"
#include "signal_abi.h"
#include "lib/unetmount.h"
#include "usmb.h"

#define RUN_DIR       "/run/smbfsd"
#define CONNECT_MS    5000u
#define ANSWER_MS     10000u
#define RETRY_AFTER_MS 5000u    // a failed connect answers this long without retrying
#define STAT_TTL_MS   2000u
#define LIST_TTL_MS   3000u
#define FILE_IDLE_MS  15000u

static uint64_t now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

// ======================================================================
// The worker
// ======================================================================

struct cfg {
    char name[UNETMOUNT_NAME_MAX];
    char point[256];
    char url[160];
    char user[64], domain[64];
    uint8_t hash[16];
    int have_hash;              // a password is known (saved, or given this boot)
    int saved;                  // from /etc/mounts.d (so Remember can write it)
    int signed_in;              // credentials given THIS boot win over the file's
};

static struct cfg g_cfg;
static int g_handle;
static char g_server[128], g_share[128], g_base[512];
static uint16_t g_port;
static struct usmb *g_s;
static int g_state = USERFS_ST_IDLE;
static uint64_t g_failed_at;    // when a connect last failed; 0 = not recently
static int g_fail_err;

static void publish(int state, const char *note, uint64_t total, uint64_t avail) {
    struct userfs_state st;
    memset(&st, 0, sizeof st);
    st.handle = (uint32_t)g_handle;
    st.state = (uint32_t)state;
    st.total = total;
    st.avail = avail;
    snprintf(st.note, sizeof st.note, "%s", note ? note : "");
    sys_userfs(USERFS_STATE, (uint64_t)(uintptr_t)&st, 0);
    g_state = state;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int unhex(const char *s, uint8_t out[16]) {
    if (strlen(s) != 32) return 0;
    for (int i = 0; i < 16; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 1;
}

static int needs_password(void) {
    return g_cfg.user[0] && strcmp(g_cfg.user, "guest") && !g_cfg.have_hash;
}

// ---- caches ------------------------------------------------------------

struct stat_ent { char path[512]; struct usmb_stat st; int found; uint64_t at; };
static struct stat_ent g_stats[64];
static int g_stat_next;

struct list_ent { char *name; struct usmb_stat st; };
static struct { char path[512]; struct list_ent *e; int n, cap; uint64_t at; int valid; } g_list;

struct open_ent { char path[512]; struct usmb_file f; uint64_t used; int open; };
static struct open_ent g_files[4];

static void stat_put(const char *path, const struct usmb_stat *st, int found) {
    struct stat_ent *e = &g_stats[g_stat_next++ % 64];
    snprintf(e->path, sizeof e->path, "%s", path);
    if (st) e->st = *st; else memset(&e->st, 0, sizeof e->st);
    e->found = found;
    e->at = now_ms();
}

static struct stat_ent *stat_get(const char *path) {
    uint64_t t = now_ms();
    for (int i = 0; i < 64; i++)
        if (g_stats[i].at && t - g_stats[i].at < STAT_TTL_MS && !strcmp(g_stats[i].path, path))
            return &g_stats[i];
    return NULL;
}

static void list_clear(void) {
    for (int i = 0; i < g_list.n; i++) free(g_list.e[i].name);
    g_list.n = 0;
    g_list.valid = 0;
}

static void drop_session(void) {
    if (g_s) usmb_close(g_s);       // its files close with it
    g_s = NULL;
    for (int i = 0; i < 4; i++) g_files[i].open = 0;
    memset(g_stats, 0, sizeof g_stats);
    list_clear();
}

// ---- connecting ---------------------------------------------------------

static int to_errno(int rc) {
    switch (rc) {
    case USMB_E_NOENT:   return -ENOENT;
    case USMB_E_NOTDIR:  return -ENOTDIR;
    case USMB_E_ISDIR:   return -EISDIR;
    case USMB_E_ACCESS: case USMB_E_LOGON: case USMB_E_SHAREDENIED: case USMB_E_NEEDSIGN:
        return -EACCES;
    case USMB_E_NOMEM:   return -ENOMEM;
    case USMB_E_NET: case USMB_E_TIMEOUT: case USMB_E_RESOLVE:
        return -ETIMEDOUT;
    default:             return -EIO;
    }
}

static void note_connected(void) {
    struct usmb_info in;
    usmb_get_info(g_s, &in);
    uint64_t total = 0, avail = 0;
    usmb_fs_size(g_s, &total, &avail);
    char note[96];
    snprintf(note, sizeof note, "SMB %u.%u%s%s%s", in.dialect >> 8, (in.dialect >> 4) & 0xf,
             in.dialect == 0x0311 ? ".1" : (in.dialect & 0xf) ? ".2" : "",
             in.signing ? ", signed" : "", in.guest ? ", as guest" : "");
    publish(USERFS_ST_CONNECTED, note, total, avail);
}

// THE CREDENTIALS ARE THE FILE'S AT EACH CONNECT, so an edit to User or
// a saved Hash needs no restart (smbfsd's supervisor restarts a worker
// only when its Source changes) -- unless someone signed in this boot,
// whose answer is newer than anything on disk.
static void reload_credentials(void) {
    struct unetmount m;
    if (!g_cfg.saved || g_cfg.signed_in || !unetmount_load(g_cfg.name, &m)) return;
    snprintf(g_cfg.user, sizeof g_cfg.user, "%s", m.user);
    snprintf(g_cfg.domain, sizeof g_cfg.domain, "%s", m.domain);
    g_cfg.have_hash = unhex(m.hash, g_cfg.hash);
}

// A session, making one if there is none. 0, or a USMB_E_* error.
static int ensure(void) {
    if (g_s) return 0;
    reload_credentials();
    if (needs_password()) {
        publish(USERFS_ST_NEED_LOGIN, "Sign in to open this share", 0, 0);
        return USMB_E_LOGON;
    }
    // A server that just failed answers from memory for a few seconds:
    // one folder listing is several requests, and each must not wait out
    // its own connect timeout.
    if (g_failed_at && now_ms() - g_failed_at < RETRY_AFTER_MS) return g_fail_err;
    publish(USERFS_ST_CONNECTING, "Connecting", 0, 0);
    struct usmb_auth auth;
    memset(&auth, 0, sizeof auth);
    auth.user = g_cfg.user;
    auth.domain = g_cfg.domain;
    if (g_cfg.have_hash) memcpy(auth.nt_hash, g_cfg.hash, 16);
    else usmb_nt_hash("", auth.nt_hash);
    struct usmb_io io;
    int rc = usmb_dial(g_server, g_port, CONNECT_MS, &io);
    if (!rc) rc = usmb_open(&g_s, &io, g_server, g_share, &auth, ANSWER_MS);
    memset(&auth, 0, sizeof auth);
    if (!rc) {
        g_failed_at = 0;
        note_connected();
        return 0;
    }
    g_s = NULL;
    g_failed_at = now_ms();
    g_fail_err = rc;
    char note[96];
    // An ANONYMOUS try a server refuses is a share that wants a user --
    // what a share added without one hits first. Ask, not "failed".
    int anon_refused = !g_cfg.user[0] &&
        (rc == USMB_E_ACCESS || rc == USMB_E_SHAREDENIED || rc == USMB_E_NEEDSIGN);
    if (rc == USMB_E_LOGON || anon_refused) {
        // A saved password that stopped working (changed on the server)
        // is forgotten for this boot, so the user is asked again.
        g_cfg.have_hash = 0;
        g_cfg.signed_in = 1;      // ...and the file's is not re-read until they answer
        g_failed_at = 0;
        publish(USERFS_ST_NEED_LOGIN, "Wrong user name or password", 0, 0);
    } else if (rc == USMB_E_NET || rc == USMB_E_TIMEOUT) {
        snprintf(note, sizeof note, "No answer from %s", g_server);
        publish(USERFS_ST_UNREACHABLE, note, 0, 0);
    } else if (rc == USMB_E_RESOLVE) {
        snprintf(note, sizeof note, "%s was not found on the network", g_server);
        publish(USERFS_ST_UNREACHABLE, note, 0, 0);
    } else {
        snprintf(note, sizeof note, "%s", usmb_strerror(rc));
        publish(USERFS_ST_FAILED, note, 0, 0);
    }
    return rc;
}

// Whether an operation's error means the session is dead, so one more
// try on a fresh one is worth it.
static int session_broke(int rc) {
    return rc == USMB_E_NET || rc == USMB_E_TIMEOUT || rc == USMB_E_SIGN || rc == USMB_E_PROTO;
}

// The share-relative path for a mount-relative one. ONE buffer: a
// worker is single-threaded and each request uses it once, and four
// 1 KiB locals inlined into serve() overran the 2 KiB frame budget.
static const char *remote(const char *path) {
    static char rp[1024];
    snprintf(rp, sizeof rp, "%s%s", g_base, strcmp(path, "/") ? path : "");
    return rp;
}

// ---- the requests ---------------------------------------------------------

// A path in the folder just listed, answered from that listing: the
// kernel stats every entry a listing returns (for its date), and a
// round trip per entry would make a big folder crawl.
static int from_list(const char *path, struct usmb_stat *st) {
    const char *slash = strrchr(path, '/');
    if (!slash || !g_list.valid || now_ms() - g_list.at >= LIST_TTL_MS) return 0;
    size_t dl = (size_t)(slash - path);
    const char *dir = dl ? path : "/";
    if (dl ? (strncmp(g_list.path, path, dl) || g_list.path[dl]) : strcmp(g_list.path, dir)) return 0;
    for (int i = 0; i < g_list.n; i++)
        if (!strcmp(g_list.e[i].name, slash + 1)) { *st = g_list.e[i].st; return 1; }
    return 0;
}

static int do_stat(const char *path, struct userfs_stat *out) {
    struct stat_ent *c = stat_get(path);
    struct usmb_stat st;
    if (!c && from_list(path, &st)) {
        // answered by the listing
    } else if (c) {
        if (!c->found) return -ENOENT;
        st = c->st;
    } else {
        const char *rp = remote(path);
        int rc = 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            rc = ensure();
            if (!rc) rc = usmb_stat(g_s, rp, &st);
            if (!session_broke(rc) || attempt) break;
            drop_session();
        }
        if (rc == USMB_E_NOENT || rc == USMB_E_NOTDIR) { stat_put(path, NULL, 0); return -ENOENT; }
        if (rc) { if (session_broke(rc)) drop_session(); return to_errno(rc); }
        stat_put(path, &st, 1);
    }
    memset(out, 0, sizeof *out);
    out->size = st.size;
    out->created = st.created;
    out->modified = st.modified;
    out->is_dir = (uint32_t)st.is_dir;
    return 0;
}

static int collect(void *ctx, const char *name, const struct usmb_stat *st) {
    (void)ctx;
    if (g_list.n == g_list.cap) {
        int cap = g_list.cap ? g_list.cap * 2 : 256;
        struct list_ent *e = realloc(g_list.e, sizeof *e * (size_t)cap);
        if (!e) return 1;
        g_list.e = e;
        g_list.cap = cap;
    }
    size_t nl = strlen(name);
    if (!nl || nl > 255) return 0;
    char *copy = malloc(nl + 1);
    if (!copy) return 1;
    memcpy(copy, name, nl + 1);
    g_list.e[g_list.n].name = copy;
    g_list.e[g_list.n].st = *st;
    g_list.n++;
    return 0;
}

static int load_list(const char *path) {
    if (g_list.valid && !strcmp(g_list.path, path) && now_ms() - g_list.at < LIST_TTL_MS) return 0;
    const char *rp = remote(path);
    int rc = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        list_clear();
        rc = ensure();
        if (!rc) rc = usmb_list(g_s, rp, collect, NULL);
        if (!session_broke(rc) || attempt) break;
        drop_session();
    }
    if (rc) { list_clear(); if (session_broke(rc)) drop_session(); return to_errno(rc); }
    snprintf(g_list.path, sizeof g_list.path, "%s", path);
    g_list.at = now_ms();
    g_list.valid = 1;
    return 0;   // stats of what it holds are answered from it (from_list())
}

// Packs entries from `from` into `buf`; returns how many, *more if some remain.
static int pack_list(uint64_t from, uint8_t *buf, uint32_t cap, uint32_t *used, int *more) {
    uint32_t off = 0;
    int n = 0;
    *more = 0;
    for (uint64_t i = from; i < (uint64_t)g_list.n; i++) {
        const struct list_ent *e = &g_list.e[i];
        uint16_t nl = (uint16_t)strlen(e->name);
        uint32_t sz = (uint32_t)((sizeof(struct userfs_dirent) + nl + 7) & ~7u);
        if (off + sz > cap) { *more = 1; break; }
        struct userfs_dirent d;
        memset(&d, 0, sizeof d);
        d.size = e->st.is_dir ? 0 : e->st.size;
        d.modified = e->st.modified;
        d.name_len = nl;
        d.is_dir = (uint8_t)e->st.is_dir;
        memcpy(buf + off, &d, sizeof d);
        memcpy(buf + off + sizeof d, e->name, nl);
        off += sz;
        n++;
    }
    *used = off;
    return n;
}

static void close_idle_files(void) {
    uint64_t t = now_ms();
    for (int i = 0; i < 4; i++)
        if (g_files[i].open && g_s && t - g_files[i].used > FILE_IDLE_MS) {
            usmb_close_file(g_s, &g_files[i].f);
            g_files[i].open = 0;
        }
}

static struct open_ent *file_for(const char *path, int *err) {
    struct open_ent *lru = &g_files[0];
    for (int i = 0; i < 4; i++) {
        if (g_files[i].open && !strcmp(g_files[i].path, path)) { g_files[i].used = now_ms(); return &g_files[i]; }
        if (!g_files[i].open) lru = &g_files[i];
        else if (lru->open && g_files[i].used < lru->used) lru = &g_files[i];
    }
    if (lru->open) { usmb_close_file(g_s, &lru->f); lru->open = 0; }
    *err = usmb_open_file(g_s, remote(path), &lru->f);
    if (*err) return NULL;
    snprintf(lru->path, sizeof lru->path, "%s", path);
    lru->open = 1;
    lru->used = now_ms();
    return lru;
}

static int64_t do_read(const char *path, uint64_t off, uint8_t *buf, uint32_t len) {
    int rc = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        rc = ensure();
        if (!rc) {
            struct open_ent *f = file_for(path, &rc);
            if (f) {
                int64_t n = usmb_read(g_s, &f->f, off, buf, len);
                if (n >= 0) return n;
                rc = (int)n;
            }
        }
        if (!session_broke(rc) || attempt) break;
        drop_session();
    }
    if (session_broke(rc)) drop_session();
    return to_errno(rc);
}

// USERFS_CTL_LOGIN: "user\0domain\0password\0remember".
static int do_login(const uint8_t *data, uint32_t len) {
    const char *f[4] = { "", "", "", "0" };
    uint32_t at = 0;
    for (int i = 0; i < 4 && at < len; i++) {
        f[i] = (const char *)data + at;
        uint32_t n = 0;
        while (at + n < len && data[at + n]) n++;
        if (at + n == len) return -EINVAL;      // every field ends in its NUL
        at += n + 1;
    }
    if (!f[0][0]) return -EINVAL;
    struct cfg old = g_cfg;
    snprintf(g_cfg.user, sizeof g_cfg.user, "%s", f[0]);
    snprintf(g_cfg.domain, sizeof g_cfg.domain, "%s", f[1]);
    usmb_nt_hash(f[2], g_cfg.hash);
    g_cfg.have_hash = 1;
    g_cfg.signed_in = 1;
    drop_session();
    g_failed_at = 0;
    int rc = ensure();
    if (rc) {
        int e = rc == USMB_E_LOGON ? -EACCES : to_errno(rc);
        if (rc == USMB_E_LOGON) { g_cfg = old; g_cfg.have_hash = 0; publish(USERFS_ST_NEED_LOGIN, "Wrong user name or password", 0, 0); }
        return e;
    }
    if (f[3][0] == '1' && g_cfg.saved) {
        struct unetmount m;
        if (unetmount_load(g_cfg.name, &m)) {
            snprintf(m.user, sizeof m.user, "%s", g_cfg.user);
            snprintf(m.domain, sizeof m.domain, "%s", g_cfg.domain);
            for (int i = 0; i < 16; i++) snprintf(m.hash + 2 * i, 3, "%02x", g_cfg.hash[i]);
            unetmount_save(&m);
        }
    }
    return 0;
}

static void serve(void) {
    struct userfs_req *req = malloc(sizeof *req);
    uint8_t *buf = malloc(64 * 1024);
    if (!req || !buf) return;
    for (;;) {
        int64_t t = sys_userfs(USERFS_TAKE, (uint64_t)g_handle, (uint64_t)(uintptr_t)req);
        if (t < 0) {
            if (sys_errno() == ENODEV) break;      // unmounted: done
            sys_sleep_ms(100);
            continue;
        }
        if (t == 0) continue;
        close_idle_files();
        req->path[sizeof req->path - 1] = '\0';
        struct userfs_reply rp;
        memset(&rp, 0, sizeof rp);
        rp.id = req->id;
        rp.buf = (uint64_t)(uintptr_t)buf;
        uint32_t cap = req->len < 64 * 1024 ? req->len : 64 * 1024;
        if (req->op == USERFS_OP_STAT) {
            struct userfs_stat st;
            rp.result = do_stat(req->path, &st);
            if (!rp.result) { memcpy(buf, &st, sizeof st); rp.len = sizeof st; }
        } else if (req->op == USERFS_OP_LIST) {
            // From the cache for LIST_TTL_MS: the kernel's paged listing
            // (SYS_LISTDIR_AT) starts every page at entry 0.
            int rc = load_list(req->path);
            if (rc) rp.result = rc;
            else {
                int more;
                rp.result = pack_list(req->offset, buf, cap, &rp.len, &more);
                rp.flags = more ? USERFS_LIST_MORE : 0;
            }
        } else if (req->op == USERFS_OP_READ) {
            int64_t n = do_read(req->path, req->offset, buf, cap);
            rp.result = (int32_t)n;
            rp.len = n > 0 ? (uint32_t)n : 0;
        } else if (req->op == USERFS_OP_CTL) {
            if (req->ctl == USERFS_CTL_LOGIN) rp.result = do_login(req->data, req->data_len);
            else if (req->ctl == USERFS_CTL_RECONNECT) {
                drop_session();
                g_failed_at = 0;
                publish(needs_password() ? USERFS_ST_NEED_LOGIN : USERFS_ST_IDLE, "", 0, 0);
                rp.result = 0;
            } else rp.result = -EINVAL;
            memset(req->data, 0, sizeof req->data);
        } else {
            rp.result = -EINVAL;
        }
        sys_userfs(USERFS_REPLY, (uint64_t)(uintptr_t)&rp, 0);
    }
    drop_session();
    free(req);
    free(buf);
}

static int mkdirs(const char *path) {
    char p[256];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = '\0'; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int worker(void) {
    char path[160];
    if (usmb_parse_url(g_cfg.url, g_server, sizeof g_server, &g_port, g_share, sizeof g_share,
                       path, sizeof path)) {
        fprintf(stderr, "smbfsd: %s: '%s' is not //server/share\n", g_cfg.name, g_cfg.url);
        return 1;
    }
    // A share given with a folder (//nas/media/Films) is served from there.
    snprintf(g_base, sizeof g_base, "%s%s", path[0] && path[0] != '/' ? "/" : "", path);
    size_t bl = strlen(g_base);
    while (bl && g_base[bl - 1] == '/') g_base[--bl] = '\0';

    if (!mkdirs(g_cfg.point)) {
        fprintf(stderr, "smbfsd: cannot make %s\n", g_cfg.point);
        return 1;
    }
    struct userfs_mount m;
    memset(&m, 0, sizeof m);
    snprintf(m.point, sizeof m.point, "%s", g_cfg.point);
    snprintf(m.type, sizeof m.type, "smb");
    snprintf(m.source, sizeof m.source, "%s", g_cfg.url);
    int64_t h = sys_userfs(USERFS_MOUNT, (uint64_t)(uintptr_t)&m, 0);
    if (h <= 0) {
        int e = sys_errno();
        fprintf(stderr, "smbfsd: cannot mount %s: %s\n", g_cfg.point, strerror(e));
        return e == EBUSY ? 3 : 1;
    }
    g_handle = (int)h;
    if (g_cfg.saved) {
        mkdirs(RUN_DIR);
        char pidf[96], pid[16];
        snprintf(pidf, sizeof pidf, "%s/%s.pid", RUN_DIR, g_cfg.name);
        int fd = open(pidf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            int n = snprintf(pid, sizeof pid, "%d\n", sys_getpid());
            if (write(fd, pid, (size_t)n) != n) { /* the supervisor finds us by its child pid anyway */ }
            close(fd);
        }
    }
    publish(needs_password() ? USERFS_ST_NEED_LOGIN : USERFS_ST_IDLE,
            needs_password() ? "Sign in to open this share" : "", 0, 0);
    serve();
    return 0;
}

static int serve_saved(const char *name) {
    struct unetmount m;
    if (!unetmount_load(name, &m)) {
        fprintf(stderr, "smbfsd: no share named '%s' in %s\n", name, UNETMOUNT_DIR);
        return 1;
    }
    snprintf(g_cfg.name, sizeof g_cfg.name, "%s", m.name);
    unetmount_point(m.name, g_cfg.point, sizeof g_cfg.point);
    snprintf(g_cfg.url, sizeof g_cfg.url, "%s", m.source);
    snprintf(g_cfg.user, sizeof g_cfg.user, "%s", m.user);
    snprintf(g_cfg.domain, sizeof g_cfg.domain, "%s", m.domain);
    g_cfg.have_hash = unhex(m.hash, g_cfg.hash);
    g_cfg.saved = 1;
    return worker();
}

// ======================================================================
// The supervisor
// ======================================================================

#define MAX_SHARES 16

struct child {
    char name[UNETMOUNT_NAME_MAX];
    struct unetmount cfg;       // what it was started with (hash aside)
    int pid;
    int gone;                   // its file was removed: stop, do not restart
    int busy;                   // someone else serves its point: wait for a change
    uint64_t restart_at;
    unsigned backoff_ms;
};

static struct child g_kids[MAX_SHARES];
static int g_nkids;

// Only the SHARE makes a worker a different one: its credentials are
// re-read at every connect (reload_credentials()), so the worker's own
// "Remember password" -- which writes User and Hash -- restarts nothing.
static int same_share(const struct unetmount *a, const struct unetmount *b) {
    return !strcmp(a->source, b->source);
}

static void start(struct child *k) {
    char args[64];
    snprintf(args, sizeof args, "--serve %s", k->name);
    k->pid = sys_spawn("/bin/smbfsd", args, -1);
    if (k->pid < 0) {
        k->pid = 0;
        k->restart_at = now_ms() + 5000;
    }
}

static void stop(struct child *k) {
    char point[128];
    if (k->pid > 0) {
        sys_kill(k->pid, SIGTERM);
        int code;
        for (int i = 0; i < 20 && sys_waitpid_nohang(k->pid, &code) == SYS_RETRY; i++) sys_sleep_ms(50);
        k->pid = 0;
    }
    unetmount_point(k->name, point, sizeof point);
    // Fails harmlessly while a file there is open: the mount then stays,
    // serverless, until it is closed and unmounted by hand.
    if (sys_umount(point) == 0 && k->gone) rmdir(point);   // empty once unmounted; refused otherwise
}

static void reconcile(void) {
    char names[MAX_SHARES][UNETMOUNT_NAME_MAX];
    int n = unetmount_list(names, MAX_SHARES);
    for (int i = 0; i < g_nkids; i++) g_kids[i].gone = 1;
    for (int j = 0; j < n; j++) {
        struct unetmount m;
        if (!unetmount_load(names[j], &m)) continue;
        struct child *k = NULL;
        for (int i = 0; i < g_nkids; i++) if (!strcmp(g_kids[i].name, names[j])) k = &g_kids[i];
        if (k) {
            k->gone = 0;
            if (!same_share(&k->cfg, &m)) {      // edited: serve it afresh
                stop(k);
                k->cfg = m;
                k->busy = 0;
                k->backoff_ms = 0;
                start(k);
            }
            continue;
        }
        if (g_nkids == MAX_SHARES) break;
        k = &g_kids[g_nkids++];
        memset(k, 0, sizeof *k);
        snprintf(k->name, sizeof k->name, "%s", names[j]);
        k->cfg = m;
        start(k);
    }
    for (int i = 0; i < g_nkids; i++) {
        if (!g_kids[i].gone) continue;
        stop(&g_kids[i]);
        g_kids[i--] = g_kids[--g_nkids];
    }
}

// Workers a previous supervisor left behind keep their mounts until they
// are told to stop; the new workers then adopt the mounts.
static void stop_strays(void) {
    struct sys_dirent *ents = malloc(sizeof *ents * 32);
    if (!ents) return;
    int n = sys_listdir(RUN_DIR, ents, 32);
    for (int i = 0; i < n; i++) {
        char path[96], buf[16];
        snprintf(path, sizeof path, "%s/%s", RUN_DIR, ents[i].name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int got = (int)read(fd, buf, sizeof buf - 1);
        close(fd);
        unlink(path);
        if (got <= 0) continue;
        buf[got] = '\0';
        int pid = atoi(buf);
        if (pid > 1 && pid != sys_getpid()) sys_kill(pid, SIGTERM);
    }
    free(ents);
}

static __attribute__((noreturn)) void supervisor(void) {
    stop_strays();
    long long seen = -2;
    for (;;) {
        long long gen = sys_fs_generation_of(UNETMOUNT_DIR);
        if (gen != seen) {
            seen = gen;
            reconcile();
        }
        uint64_t t = now_ms();
        for (int i = 0; i < g_nkids; i++) {
            struct child *k = &g_kids[i];
            int code = 0;
            if (k->pid > 0 && sys_waitpid_nohang(k->pid, &code) == k->pid) {
                k->pid = 0;
                if (code == 3) { k->busy = 1; continue; }     // another process serves it
                k->backoff_ms = k->backoff_ms ? (k->backoff_ms * 2 > 30000 ? 30000 : k->backoff_ms * 2) : 1000;
                k->restart_at = t + k->backoff_ms;
            }
            if (!k->pid && !k->busy && k->restart_at && t >= k->restart_at) {
                k->restart_at = 0;
                start(k);
            }
        }
        sys_sleep_ms(1000);
    }
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--serve")) return serve_saved(argv[2]);
    if ((argc == 4 || argc == 6) && !strcmp(argv[1], "--url")) {
        snprintf(g_cfg.name, sizeof g_cfg.name, "url");
        snprintf(g_cfg.url, sizeof g_cfg.url, "%s", argv[2]);
        snprintf(g_cfg.point, sizeof g_cfg.point, "%s", argv[3]);
        if (argc == 6 && !strcmp(argv[4], "--user")) snprintf(g_cfg.user, sizeof g_cfg.user, "%s", argv[5]);
        return worker();
    }
    if (argc == 1) supervisor();
    fprintf(stderr, "usage: smbfsd [--serve NAME | --url URL POINT [--user USER]]\n");
    return 2;
}
