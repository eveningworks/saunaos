// What an identity may do -- docs/users-design.md, stage B. The rules
// are in kernel/privilege.h; this file is the gate the dispatcher asks,
// the seat's mount rule, the kill rule, and SYS_CRED_GRANT's grants.
#include "privilege.h"
#include "cred.h"
#include "cred_abi.h"
#include "syscall_abi.h"
#include "syscalls.h"
#include "scheduler.h"
#include "block.h"
#include "spinlock.h"
#include "heap.h"
#include "kpath_buf.h"
#include "timer.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "errno.h"
#include "vmm.h"
#include "setting.h"

int priv_is_root(void) { return cred_euid(cred_current()) == 0; }

int priv_has_seat(void) {
    const struct cred *c = cred_current();
    return cred_euid(c) == 0 || (cred_flags(c) & CRED_SEAT);
}

// The PROMISE_ADMIN rows an ordinary account may still reach. Everything
// else in the group is root's.
static const uint16_t SEAT_ROWS[] = {
    SYS_POWEROFF, SYS_DISPLAY_POWER, SYS_SHUTDOWN_STATUS,
    SYS_EJECT, SYS_MOUNT, SYS_UMOUNT, SYS_USERFS,   // their handlers check the point
};
// ...and the ones anybody may: a remote session's report of itself,
// which tftpd and a telnet shell make as `user`.
static const uint16_t ANY_ROWS[] = { SYS_REMOTE_LOG };

static int listed(const uint16_t *rows, unsigned n, uint64_t nr) {
    for (unsigned i = 0; i < n; i++)
        if (rows[i] == nr) return 1;
    return 0;
}

int priv_admin_check(uint64_t nr) {
    if (priv_is_root()) return 0;
    if (listed(ANY_ROWS, sizeof ANY_ROWS / sizeof ANY_ROWS[0], nr)) return 0;
    if (priv_has_seat() && listed(SEAT_ROWS, sizeof SEAT_ROWS / sizeof SEAT_ROWS[0], nr)) return 0;
    return -EPERM;
}

int priv_setting_check(const char *name) {
    const struct setting *s = setting_find(name);
    if (!s || priv_is_root()) return 0;
    if (s->who == SETTING_WHO_SEAT && priv_has_seat()) return 0;
    return -EACCES;
}

int priv_setting_apply_check(const char *name) {
    const struct setting *s = setting_find(name);
    if (!s || priv_is_root()) return 0;
    if (s->who == SETTING_WHO_PERSON && priv_has_seat()) return 0;
    return -EACCES;
}

// --- the seat's mounts ------------------------------------------------------

// Under /media/ or /net/, with a name, and no "." or ".." component --
// the point is compared as TEXT here, so "/media/../bin" must not pass
// as being under /media.
int priv_seat_point(const char *point) {
    if (priv_is_root()) return 0;
    if (!priv_has_seat() || !point) return -EPERM;
    const char *rest;
    if (!k_strncmp(point, "/media/", 7)) rest = point + 7;
    else if (!k_strncmp(point, "/net/", 5)) rest = point + 5;
    else return -EPERM;
    if (!*rest) return -EPERM;
    for (const char *s = rest; *s; ) {
        const char *e = s;
        while (*e && *e != '/') e++;
        long n = e - s;
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return -EPERM;
        s = *e ? e + 1 : e;
    }
    return 0;
}

int priv_seat_mount(const struct block_device *dev, const char *point) {
    if (priv_is_root()) return 0;
    int rc = priv_seat_point(point);
    if (rc) return rc;
    return (!dev || dev->removable) ? 0 : -EPERM;
}

// --- signals ----------------------------------------------------------------

int priv_may_signal(int tgid) {
    const struct cred *me = cred_current();
    if (cred_euid(me) == 0) return 1;
    uint32_t target;
    if (!scheduler_pid_ruid(tgid, &target)) return 1;   // gone: the send finds nothing
    return target == cred_ruid(me) || target == cred_euid(me);
}

// --- SYS_CRED_GRANT ---------------------------------------------------------

// A grant is privd's word that process `tgid` may spawn `path` with
// exactly these arguments as root, ONCE, within GRANT_TICKS.
struct grant {
    struct grant *next;
    int           tgid;
    uint64_t      deadline;   // coarse_ticks()
    uint64_t      args_len;
    char         *path;
    char         *env;        // the child's environment, `env_len` bytes
    uint64_t      env_len;
    char          args[];
};

#define GRANT_TICKS (30 * COARSE_HZ)

static struct spinlock g_lock;
static struct grant *g_grants;

// Unlinks every grant `drop` says, under the lock; the caller frees the
// returned chain (kfree takes the heap lock, never under this one).
static struct grant *unlink_locked(int tgid, uint64_t now) {
    struct grant *out = 0, **pp = &g_grants;
    while (*pp) {
        struct grant *g = *pp;
        if (g->tgid == tgid || now >= g->deadline) {
            *pp = g->next;
            g->next = out;
            out = g;
        } else {
            pp = &g->next;
        }
    }
    return out;
}

static void free_chain(struct grant *g) {
    while (g) {
        struct grant *n = g->next;
        kfree(g->path);
        kfree(g->env);
        kfree(g);
        g = n;
    }
}

void priv_grant_drop(int tgid) {
    if (tgid <= 0) return;
    spin_lock(&g_lock);
    struct grant *dead = unlink_locked(tgid, coarse_ticks());
    spin_unlock(&g_lock);
    free_chain(dead);
}

int priv_grant_take(const char *path, const char *args, uint64_t len, char **env,
                    uint64_t *env_len) {
    int me = scheduler_current_tgid();
    uint64_t now = coarse_ticks();
    struct grant *hit = 0, *dead = 0;
    spin_lock(&g_lock);
    for (struct grant **pp = &g_grants; *pp; pp = &(*pp)->next) {
        struct grant *g = *pp;
        if (g->tgid == me && now < g->deadline && g->args_len == len &&
            !k_strcmp(g->path, path) && (!len || !k_memcmp(g->args, args, len))) {
            *pp = g->next;
            hit = g;
            break;
        }
    }
    dead = unlink_locked(-1, now);   // the expired, while we are here
    spin_unlock(&g_lock);
    int ok = hit != 0;
    if (hit) {
        *env = hit->env;          // the spawn's now
        *env_len = hit->env_len;
        hit->env = 0;
        hit->next = dead;
        dead = hit;
    }
    free_chain(dead);
    if (!ok) klog_printf(KLOG_WARN "privilege: pid %d spawned %s elevated with no grant for it -- refused\n",
                         me, path);
    return ok ? 0 : -EPERM;
}

int sys_cred_grant(struct syscall_ctx *c) {
    struct cred_grant req;
    int64_t rc = 0;
    char *path = 0;
    struct grant *g = 0;
    if (!priv_is_root()) { rc = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof req)) { rc = -EFAULT; goto out; }
    if (req.pid <= 0 || req.reserved || !req.env || req.env_len < 1 ||
        (uint64_t)req.args_len + req.env_len > SPAWN_ARGS_MAX) { rc = -EINVAL; goto out; }
    path = kpath_get();
    if (!path) { rc = -ENOMEM; goto out; }
    rc = resolve_user_path(c->pml4, (uint64_t)(uintptr_t)req.path, path, 0, FSP_ASK_NONE);
    if (rc) goto out;
    g = kmalloc(sizeof *g + req.args_len);
    char *keep = g ? kmalloc(k_strlen(path) + 1) : 0;
    if (!g || !keep) { kfree(g); g = 0; rc = -ENOMEM; goto out; }
    k_strlcpy(keep, path, k_strlen(path) + 1);
    if (req.args_len && !vmm_copy_from_user(c->pml4, g->args, (uint64_t)(uintptr_t)req.args, req.args_len)) {
        kfree(keep); kfree(g); g = 0; rc = -EFAULT; goto out;
    }
    if (req.args_len && g->args[req.args_len - 1] != '\0') {   // malformed, not a bad address
        kfree(keep); kfree(g); g = 0; rc = -EINVAL; goto out;
    }
    g->env = kmalloc(req.env_len);
    if (!g->env || !vmm_copy_from_user(c->pml4, g->env, (uint64_t)(uintptr_t)req.env, req.env_len)) {
        rc = g->env ? -EFAULT : -ENOMEM;
        kfree(g->env); kfree(keep); kfree(g); g = 0; goto out;
    }
    // Strings ending in an empty one: the last byte, and the one before
    // it unless the environment is empty, are NULs.
    if (g->env[req.env_len - 1] || (req.env_len > 1 && g->env[req.env_len - 2])) {
        kfree(g->env); kfree(keep); kfree(g); g = 0; rc = -EINVAL; goto out;
    }
    g->env_len = req.env_len;
    g->tgid = req.pid;
    g->deadline = coarse_ticks() + GRANT_TICKS;
    g->args_len = req.args_len;
    g->path = keep;
    spin_lock(&g_lock);
    g->next = g_grants;
    g_grants = g;
    spin_unlock(&g_lock);
    klog_printf("privilege: pid %d may spawn %s elevated once\n", req.pid, keep);
out:
    if (path) kpath_put(path);
    c->regs[14] = (uint64_t)rc;
    return 0;
}
