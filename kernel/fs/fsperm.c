// File permissions -- kernel/fsperm.h has the contract. Owner, group,
// other, as Linux's generic_permission(); root passes but for exec.
#include "fsperm.h"
#include "fs.h"
#include "cred.h"
#include "cred_abi.h"   // CRED_PERM_ENFORCE
#include "errno.h"
#include "klog.h"
#include "kfmt.h"
#include "kpath.h"
#include "kpath_buf.h"   // a path is 4096 bytes and may not be a kernel local
#include "string.h"
#include "multiboot.h"
#include "scheduler.h"

// What a kernel with no `fileperm=` word does: PERMISSIVE until the GUI
// suite's fixtures belong to the desktop's account -- its first run under
// enforcement failed 48 tools, nearly all on files the harness (root) had
// made for an app run as `user` to change (docs/users-design.md, stage C).
#ifndef FSPERM_DEFAULT
#define FSPERM_DEFAULT FSPERM_PERMISSIVE
#endif

static int g_mode = -1;   // -1 until the command line is read

int fsperm_mode(void) {
    int m = __atomic_load_n(&g_mode, __ATOMIC_RELAXED);
    if (m >= 0) return m;
    char v[16];
    m = FSPERM_DEFAULT;
    if (multiboot_cmdline_value("fileperm=", v, sizeof v)) {
        if (!k_strcmp(v, "off")) m = FSPERM_OFF;
        else if (!k_strcmp(v, "permissive")) m = FSPERM_PERMISSIVE;
        else if (!k_strcmp(v, "enforce")) m = FSPERM_ENFORCE;
    }
    __atomic_store_n(&g_mode, m, __ATOMIC_RELAXED);
    klog_printf("fsperm: file permissions %s\n",
                m == FSPERM_OFF ? "off (fileperm=off)"
                : m == FSPERM_PERMISSIVE ? "logged, not enforced (fileperm=permissive)"
                : "enforced");
    return m;
}

// ---- the log ---------------------------------------------------------
//
// A refusal is the first thing to look for when a program run as `user`
// fails, so each DISTINCT one is logged -- the same path and question
// from one process is one line however often it repeats (a program
// retrying a save) -- up to a per-boot budget that says when it runs out.
#define LOG_BUDGET 256
#define LOG_SEEN   64
static uint32_t g_seen[LOG_SEEN];
static unsigned g_seen_next;
static int g_logged;

static int enforced(void);

static void log_refusal(const char *what, const char *path, int rc) {
    struct cred *c = cred_current();
    int pid = scheduler_current_tgid();
    uint32_t h = 2166136261u;
    for (const char *p = path; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (const char *p = what; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h ^= (uint32_t)pid * 2654435761u;
    for (unsigned i = 0; i < LOG_SEEN; i++)
        if (__atomic_load_n(&g_seen[i], __ATOMIC_RELAXED) == h) return;
    unsigned slot = __atomic_fetch_add(&g_seen_next, 1, __ATOMIC_RELAXED) % LOG_SEEN;
    __atomic_store_n(&g_seen[slot], h, __ATOMIC_RELAXED);
    int n = __atomic_add_fetch(&g_logged, 1, __ATOMIC_RELAXED);
    if (n > LOG_BUDGET) return;
    char name[32];
    if (!scheduler_name_of(pid, name, sizeof name)) k_strlcpy(name, "?", sizeof name);
    klog_printf(KLOG_WARN "fsperm: pid %d (%s) uid %u refused %s %s (%s)%s%s\n", pid, name,
                (unsigned)cred_euid(c), what, path, rc == -EPERM ? "not the owner" : "mode",
                enforced() ? "" : " -- allowed, permissive",
                n == LOG_BUDGET ? " -- further refusals not logged" : "");
}

static int enforced(void) {
    return fsperm_mode() == FSPERM_ENFORCE || (cred_flags(cred_current()) & CRED_PERM_ENFORCE);
}

static int verdict(const char *what, const char *path, int rc) {
    if (!rc) return 0;
    log_refusal(what, path, rc);
    return enforced() ? rc : 0;
}

// ---- the rule --------------------------------------------------------

// Which of r/w/x the cred has on a file with this owner and mode: the
// owner's bits, else the group's if any of its groups match, else
// other's -- the FIRST class that matches decides, so an owner denied
// what other may do is denied (POSIX).
static unsigned granted(const struct cred *c, const struct fs_stat_info *st) {
    uint32_t uid = cred_euid(c);
    if (uid == st->uid) return (st->mode >> 6) & 7u;
    if (cred_in_group(c, st->gid)) return (st->mode >> 3) & 7u;
    return st->mode & 7u;
}

static int allowed(const struct cred *c, const struct fs_stat_info *st, unsigned want) {
    if (cred_euid(c) == 0) {
        // Root reads and writes anything, and searches any directory;
        // it EXECUTES a file only if some class may (Linux's rule), so a
        // data file is never run by accident.
        if ((want & FSP_X) && !st->is_dir && !(st->mode & 0111)) return 0;
        return 1;
    }
    return (granted(c, st) & want) == want;
}

// Search on every directory above `path`: "/", "/a", "/a/b" for
// "/a/b/c". A component that is missing or not a directory answers 0 --
// the operation fails on its own terms.
static int search_above(const struct cred *c, const char *path) {
    if (cred_euid(c) == 0) return 0;
    // ONLY WHEN IT CAN REFUSE: the walk is a stat per folder above the
    // path, each under its volume's lock, and a permissive boot that is
    // only taking an inventory logs the path's own refusals without it.
    if (!enforced()) return 0;
    char *dir = kpath_get();
    if (!dir) return -ENOMEM;
    int rc = 0;
    size_t n = k_strlen(path);
    for (size_t i = 0; i < n && !rc; i++) {
        if (path[i] != '/' ) continue;
        size_t len = i ? i : 1;   // "/" itself, then each prefix up to a slash
        if (len >= FS_PATH_MAX) break;
        k_memcpy(dir, path, len);
        dir[len] = '\0';
        struct fs_stat_info st;
        if (!fs_attrs(dir, &st) || !st.is_dir) break;
        if (!allowed(c, &st, FSP_X)) rc = -EACCES;
    }
    kpath_put(dir);
    return rc;
}

int fsperm_may_search(const char *path) {
    if (fsperm_mode() == FSPERM_OFF) return 0;
    return verdict("search above", path, search_above(cred_current(), path));
}

int fsperm_may(const char *path, unsigned want) {
    if (fsperm_mode() == FSPERM_OFF) return 0;
    struct cred *c = cred_current();
    int rc = search_above(c, path);
    struct fs_stat_info st;
    if (!rc && fs_attrs(path, &st) && !allowed(c, &st, want)) rc = -EACCES;
    return verdict(want & FSP_W ? "write" : want & FSP_X ? "exec" : "read", path, rc);
}

// The parent of `path` into a kpath buffer the caller puts back.
static char *parent_of(const char *path) {
    char *dir = kpath_get();
    if (dir && !k_path_dirname(path, dir, FS_PATH_MAX)) { kpath_put(dir); dir = 0; }
    return dir;
}

static int may_in_parent(const char *path, int deleting, const char *what) {
    if (fsperm_mode() == FSPERM_OFF) return 0;
    struct cred *c = cred_current();
    int rc = search_above(c, path);
    char *dir = rc ? 0 : parent_of(path);
    if (!rc && !dir) rc = -ENOMEM;
    struct fs_stat_info pst;
    if (!rc && fs_attrs(dir, &pst)) {
        if (!allowed(c, &pst, FSP_W | FSP_X)) {
            rc = -EACCES;
        } else if (deleting && (pst.mode & 01000) && cred_euid(c) != 0) {
            // THE STICKY BIT (/tmp's 1777): in such a folder only the
            // file's owner or the folder's may remove or rename it.
            struct fs_stat_info st;
            uint32_t uid = cred_euid(c);
            if (fs_attrs(path, &st) && uid != st.uid && uid != pst.uid) rc = -EPERM;
        }
    }
    if (dir) kpath_put(dir);
    return verdict(what, path, rc);
}

// A name that is already there is the operation's EEXIST, not a mode's
// EACCES: Linux's filename_create() finds it before may_create() asks --
// and every `mkdir -p` and startup mkdir("/home") relies on that.
int fsperm_may_create(const char *path) {
    if (fsperm_mode() == FSPERM_OFF) return 0;
    struct fs_stat_info st;
    if (fs_attrs(path, &st)) return fsperm_may_search(path);
    return may_in_parent(path, 0, "create");
}
int fsperm_may_delete(const char *path) { return may_in_parent(path, 1, "delete"); }

// "Now" as a file's time: its owner, or anyone who may write it --
// utimensat(2)'s UTIME_NOW rule.
int fsperm_may_touch(const char *path) {
    if (fsperm_mode() == FSPERM_OFF) return 0;
    struct cred *c = cred_current();
    int rc = search_above(c, path);
    struct fs_stat_info st;
    if (!rc && cred_euid(c) != 0 && fs_attrs(path, &st) && st.uid != cred_euid(c) &&
        !allowed(c, &st, FSP_W))
        rc = -EACCES;
    return verdict("touch", path, rc);
}

int fsperm_may_own(const char *path) {
    if (fsperm_mode() == FSPERM_OFF) return 0;
    struct cred *c = cred_current();
    int rc = search_above(c, path);
    struct fs_stat_info st;
    if (!rc && cred_euid(c) != 0 && fs_attrs(path, &st) && st.uid != cred_euid(c)) rc = -EPERM;
    return verdict("change", path, rc);
}

int fsperm_may_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (fsperm_mode() == FSPERM_OFF) return 0;
    struct cred *c = cred_current();
    int rc = search_above(c, path);
    struct fs_stat_info st;
    if (!rc && cred_euid(c) != 0 && fs_attrs(path, &st)) {
        // Linux's chown_ok()/chgrp_ok(): giving a file away is root's,
        // and an owner may move it only into a group it is in.
        if (st.uid != cred_euid(c)) rc = -EPERM;
        else if (uid != FS_OWNER_KEEP && uid != st.uid) rc = -EPERM;
        else if (gid != FS_OWNER_KEEP && gid != st.gid && !cred_in_group(c, gid)) rc = -EPERM;
    }
    return verdict("chown", path, rc);
}

int fsperm_ask(const char *path, enum fsp_ask ask) {
    switch (ask) {
    case FSP_ASK_NONE:   return 0;
    case FSP_ASK_SEARCH: return fsperm_may_search(path);
    case FSP_ASK_READ:   return fsperm_may(path, FSP_R);
    case FSP_ASK_WRITE:  return fsperm_may(path, FSP_W);
    case FSP_ASK_EXEC:   return fsperm_may(path, FSP_X);
    case FSP_ASK_CREATE: return fsperm_may_create(path);
    case FSP_ASK_DELETE: return fsperm_may_delete(path);
    case FSP_ASK_OWN:    return fsperm_may_own(path);
    case FSP_ASK_TOUCH:  return fsperm_may_touch(path);
    }
    return -EINVAL;
}
