// userfs -- a filesystem whose answers come from a process
// (abi/userfs_abi.h). Each backend call on the mount becomes a request
// on the mount's queue; the daemon that mounted it takes the request
// with SYS_USERFS, answers it, and the caller returns with the answer.
// /bin/smbfsd is the first server: a Windows or Samba share under /net.
//
// READ-ONLY, and mounted MNT_RDONLY, so vfs.c refuses every write before
// it reaches the stubs below.
//
// THE THREE RULES that make a process-backed filesystem safe to hang a
// mount table off -- the traps FUSE documents:
//
//  1. A CALLER NEVER WAITS HOLDING THE MOUNT'S LOCK when it can drop it
//     (mount_wait_until()). A network wait is seconds; holding the lock
//     across it would stall fs_exclusive_begin() -- `sync`, a mount, a
//     partition write -- behind a NAS that has gone to sleep.
//  2. EVERY WAIT IS BOUNDED (USERFS_WAIT_MS), and re-checks the daemon
//     is alive each slice: a daemon that crashed mid-request costs its
//     callers half a second, not the whole timeout.
//  3. THE DAEMON MAY NOT CALL INTO ITS OWN MOUNT -- it would wait on a
//     request only it can answer. Refused at once.
//
// A REQUEST BELONGS TO ITS CALLER, never to the mount: the caller
// allocates it, waits on it and frees it. Whoever finishes one -- the
// daemon's REPLY, the unmount, a dead daemon noticed -- does so under
// g_lock and wakes it, and a caller that finds its request still
// unfinished after a timeout unlinks it under the same lock. That is
// what lets an unmount free the mount's state while callers still hold
// requests that were queued on it.
#include "fs_ops.h"
#include "fs.h"
#include "mount.h"
#include "userfs.h"
#include "userfs_abi.h"
#include "heap.h"
#include "kmutex.h"
#include "scheduler.h"
#include "clocksource.h"
#include "syscalls.h"
#include "syscall_table.h"
#include "vmm.h"
#include "klog.h"
#include "kfmt.h"
#include "barrier.h"
#include "ktime.h"
#include "caltime.h"
#include "string.h"
#include "errno.h"

#define USERFS_WAIT_MS  30000u  // one call: connect, sign in, answer
#define USERFS_SLICE_MS 500u    // how often a waiter checks the daemon lives
#define USERFS_CHUNK    (1024u * 1024u) // the most one request carries back
// READ-AHEAD: a small read fetches a WINDOW and the reads after it come
// from memory. The window starts small (a header read must not pull a
// megabyte) and doubles while the reads stay sequential -- Linux's
// readahead in miniature. Without it every 4 KiB read() was a round
// trip through the daemon and the network: ~1 ms each, so a copy ran
// at a few MB/s on a link that carries twenty.
#define USERFS_RA_MIN   (128u * 1024u)
#define USERFS_RA_MAX   USERFS_CHUNK
#define USERFS_RA_TTL_MS 2000u           // a remote file can change under us

struct ufs_req {
    struct ufs_req *next;
    struct ufs_mount *um;       // valid while !done
    uint32_t id;
    int taken, done;
    int32_t result;
    uint32_t flags;
    uint32_t got;               // bytes the reply put in `buf`
    void *buf;                  // where the reply goes (kernel memory)
    uint32_t cap;
    struct userfs_req msg;      // what TAKE copies out
};

struct ufs_mount {
    int handle;                 // 1-based index into g_tab
    int owner;                  // the serving process (tgid)
    char point[256];
    char type[16];
    char source[128];
    struct userfs_state st;
    struct ufs_req *head;
    uint32_t next_id;
    char take_chan;             // an address the daemon parks on
    uint64_t mounted_at;        // epoch: the root's times, answered without the server
    // The read-ahead window, touched only under the MOUNT's lock (a
    // backend op holds it whenever it is not waiting).
    struct {
        uint8_t *buf;           // kmalloc'd, `len` bytes of `path` from `off`
        uint64_t off, next;     // `next`: where a sequential read would go on
        uint32_t len, window;
        uint64_t at_ns;
        char path[USERFS_PATH_MAX];
    } ra;
};

static struct kmutex g_lock;
static struct ufs_mount *g_tab[MOUNT_MAX];

static int owner_alive(const struct ufs_mount *um) {
    return um->owner > 0 && scheduler_pid_alive(um->owner);
}

// Finish `r` (holding g_lock): unlink it, record the result, wake its
// caller.
static void finish(struct ufs_req *r, int32_t result) {
    struct ufs_mount *um = r->um;
    for (struct ufs_req **pp = &um->head; *pp; pp = &(*pp)->next)
        if (*pp == r) { *pp = r->next; break; }
    r->result = result;
    r->done = 1;
    scheduler_wake(r, 0);
}

// Queue `r` on `um` (holding g_lock). 0, or -errno and nothing queued.
static int32_t enqueue_locked(struct ufs_mount *um, struct ufs_req *r) {
    if (um->owner == scheduler_current_tgid()) return -EDEADLK;   // rule 3
    if (!owner_alive(um)) {
        um->st.state = USERFS_ST_NO_SERVER;
        return -ENOTCONN;
    }
    r->um = um;
    r->id = r->msg.id = ++um->next_id;
    r->next = NULL;
    struct ufs_req **pp = &um->head;
    while (*pp) pp = &(*pp)->next;
    *pp = r;
    scheduler_wake(&um->take_chan, 0);
    return 0;
}

// Wait for a queued `r`. `st` is the mount state when the caller is a
// backend op holding the mount's lock (so it can be dropped), else NULL.
// Returns the result (>= 0) or -errno; `st` MUST NOT be used after
// this returns -- the mount may have gone while we waited.
static int32_t wait_for(void *st, struct ufs_req *r, int *gone) {
    if (gone) *gone = 0;
    uint64_t deadline = clocksource_now_ns() + (uint64_t)USERFS_WAIT_MS * 1000000ull;
    for (;;) {
        uint64_t now = clocksource_now_ns();
        uint64_t slice = now + (uint64_t)USERFS_SLICE_MS * 1000000ull;
        if (slice > deadline) slice = deadline;
        // Rule 1: drop the mount lock while waiting, if this call holds
        // it once. A nested call (or one under exclusion) cannot, and
        // waits holding it -- still bounded.
        int w = st ? mount_wait_until(st, r, slice) : 0;
        if (w == 0) {
            scheduler_wait_arm(r);
            if (__atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) scheduler_wait_disarm();
            else if (!scheduler_block_kernel_until(r, SCHED_WAIT_NET, slice)) {
                scheduler_wait_disarm();
                cpu_relax();
            }
        }
        kmutex_lock(&g_lock);
        if (r->done) {
            // -ENODEV is an unmount finishing it: the state is freed.
            if (gone && (r->result == -ENODEV || !st)) *gone = 1;
            kmutex_unlock(&g_lock);
            return r->result;
        }
        // Not done means the mount is still there (an unmount finishes
        // every request), so r->um is good.
        int dead = !owner_alive(r->um);
        if (dead || clocksource_now_ns() >= deadline) {
            if (dead) r->um->st.state = USERFS_ST_NO_SERVER;
            finish(r, dead ? -ENOTCONN : -ETIMEDOUT);
            if (gone && !st) *gone = 1;
            kmutex_unlock(&g_lock);
            return r->result;
        }
        kmutex_unlock(&g_lock);
        if (w < 0) st = NULL;    // the mount went away; the request finishes regardless
    }
}

// `*gone` (if asked) says the mount went away while we waited: the
// caller must not touch its state then.
static int32_t call(void *st, struct ufs_req *r, int *gone) {
    if (gone) *gone = 0;
    kmutex_lock(&g_lock);
    int32_t rc = enqueue_locked(st, r);
    kmutex_unlock(&g_lock);
    return rc ? rc : wait_for(st, r, gone);
}

static struct ufs_req *req_new(uint32_t op, const char *path) {
    struct ufs_req *r = kzalloc(sizeof *r);
    if (!r) return NULL;
    r->msg.op = op;
    if (k_strlen(path) >= USERFS_PATH_MAX) { kfree(r); return NULL; }
    k_strlcpy(r->msg.path, path, sizeof r->msg.path);
    return r;
}

// The mount's root is answered here: asking the daemon whether "/" is a
// directory would make every `ls /net` connect to every server.
static int is_root(const char *path) { return path[0] == '/' && path[1] == '\0'; }

static int do_stat(void *st, const char *path, struct userfs_stat *out) {
    k_memset(out, 0, sizeof *out);
    if (is_root(path)) {
        out->is_dir = 1;
        out->created = out->modified = ((struct ufs_mount *)st)->mounted_at;
        return 1;
    }
    struct ufs_req *r = req_new(USERFS_OP_STAT, path);
    if (!r) return 0;
    r->buf = out;
    r->cap = r->msg.len = sizeof *out;
    int32_t rc = call(st, r, NULL);
    int ok = rc >= 0 && r->got == sizeof *out;
    kfree(r);
    return ok;
}

// ---- the backend ----------------------------------------------------

static uint64_t u_size(void *st, const char *path) {
    struct userfs_stat s;
    return do_stat(st, path, &s) && !s.is_dir ? s.size : 0;
}

static int u_is_dir(void *st, const char *path) {
    struct userfs_stat s;
    return do_stat(st, path, &s) && s.is_dir;
}

static int u_exists(void *st, const char *path) {
    struct userfs_stat s;
    return do_stat(st, path, &s);
}

// A stable-for-this-boot inode number from the path: the server's file
// ids are per open, and a name is all a path-based call has.
static uint64_t path_ino(const char *path) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = path; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    return h | 1;
}

static int u_stat(void *st, const char *path, struct fs_stat_info *out) {
    struct userfs_stat s;
    if (!do_stat(st, path, &s)) return 0;
    k_memset(out, 0, sizeof *out);
    out->ino = path_ino(path);
    out->created = s.created;
    out->modified = s.modified;
    out->mode = s.is_dir ? 0555 : 0444;
    out->nlink = 1;
    out->is_dir = (uint8_t)s.is_dir;
    out->size = s.is_dir ? 0 : s.size;
    return 1;
}

// One READ request for [off, off + want) into `dst`. Bytes got, or -1.
static int64_t fetch(void *st, const char *path, uint64_t off, void *dst, uint32_t want, int *gone) {
    struct ufs_req *r = req_new(USERFS_OP_READ, path);
    if (!r) { *gone = 0; return -1; }
    r->msg.offset = off;
    r->msg.len = r->cap = want;
    r->buf = dst;
    int32_t rc = call(st, r, gone);
    int64_t got = rc >= 0 ? (int64_t)r->got : -1;
    kfree(r);
    return got;
}

static uint32_t u_read_range(void *st, const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct ufs_mount *um = st;
    uint32_t done = 0;
    while (done < len) {
        uint64_t off = offset + done;
        uint32_t want = len - done;
        uint64_t now = clocksource_now_ns();
        int same = !k_strcmp(um->ra.path, path);
        // From the window, when it holds this offset and is fresh.
        if (um->ra.buf && same && off >= um->ra.off && off < um->ra.off + um->ra.len &&
            now - um->ra.at_ns < (uint64_t)USERFS_RA_TTL_MS * 1000000ull) {
            uint32_t n = (uint32_t)(um->ra.off + um->ra.len - off);
            if (n > want) n = want;
            k_memcpy((uint8_t *)buf + done, um->ra.buf + (off - um->ra.off), n);
            done += n;
            um->ra.next = off + n;
            continue;
        }
        int gone;
        if (want >= USERFS_RA_MAX) {
            // A read this big gains nothing from the window: straight in.
            int64_t got = fetch(st, path, off, (uint8_t *)buf + done, USERFS_RA_MAX, &gone);
            if (got <= 0) break;
            done += (uint32_t)got;
            if (gone || got < USERFS_RA_MAX) break;   // the end of the file, or no mount left
            continue;
        }
        // A small read: fetch a window, growing while reads stay sequential.
        uint32_t window = same && off == um->ra.next && um->ra.window ? um->ra.window * 2 : USERFS_RA_MIN;
        if (window > USERFS_RA_MAX) window = USERFS_RA_MAX;
        if (window < want) window = want;
        uint8_t *fresh = kmalloc(window);
        if (!fresh) break;
        int64_t got = fetch(st, path, off, fresh, window, &gone);
        if (got <= 0 || gone) {
            if (got > 0) {          // the mount went: hand over what came, keep nothing
                uint32_t n = got < want ? (uint32_t)got : want;
                k_memcpy((uint8_t *)buf + done, fresh, n);
                done += n;
            }
            kfree(fresh);
            break;
        }
        // The lock is ours again (call() retook it): install the window.
        kfree(um->ra.buf);
        um->ra.buf = fresh;
        um->ra.off = off;
        um->ra.len = (uint32_t)got;
        um->ra.window = window;
        um->ra.at_ns = clocksource_now_ns();
        k_strlcpy(um->ra.path, path, sizeof um->ra.path);
        uint32_t n = got < want ? (uint32_t)got : want;
        k_memcpy((uint8_t *)buf + done, fresh, n);
        done += n;
        um->ra.next = off + n;
        if ((uint32_t)got < window && n < want) break;   // the end of the file
    }
    return done;
}

static void *u_read_range_begin(void *st, const char *path, uint64_t offset, void *buf, uint32_t len) {
    uint32_t *total = kmalloc(sizeof *total);
    if (total) *total = u_read_range(st, path, offset, buf, len);
    return total;
}

static int u_read_range_step(void *st, void *handle, uint32_t *out_total) {
    (void)st;
    if (!handle) return FS_STEP_FAILED;
    if (out_total) *out_total = *(uint32_t *)handle;
    kfree(handle);
    return FS_STEP_DONE;
}

#define LIST_BUF (16u * 1024u)

static void u_list(void *st, const char *dir, fs_list_cb cb, void *ctx) {
    uint8_t *buf = kmalloc(LIST_BUF);
    char *name = kmalloc(FS_NAME_MAX + 1);
    if (!buf || !name) { kfree(buf); kfree(name); return; }
    uint64_t next = 0;
    for (;;) {
        struct ufs_req *r = req_new(USERFS_OP_LIST, dir);
        if (!r) break;
        r->msg.offset = next;
        r->msg.len = r->cap = LIST_BUF;
        r->buf = buf;
        int32_t n = call(st, r, NULL);
        uint32_t got = r->got, more = r->flags & USERFS_LIST_MORE;
        kfree(r);
        if (n <= 0) break;
        // The daemon's bytes: every length is checked before it is used.
        uint32_t off = 0;
        for (int32_t i = 0; i < n; i++) {
            if (off + sizeof(struct userfs_dirent) > got) { n = 0; break; }
            struct userfs_dirent d;
            k_memcpy(&d, buf + off, sizeof d);
            uint32_t nl = d.name_len;
            if (nl == 0 || nl > FS_NAME_MAX || off + sizeof d + nl > got) { n = 0; break; }
            k_memcpy(name, buf + off + sizeof d, nl);
            name[nl] = '\0';
            cb(ctx, name, d.size > 0xffffffffull ? 0xffffffffu : (uint32_t)d.size, d.is_dir);
            off += (uint32_t)((sizeof d + nl + 7) & ~7u);
            next++;
        }
        if (!more || n == 0) break;
    }
    kfree(buf);
    kfree(name);
}

// From what the daemon last reported: QUERY_FSINFO polls this, and a
// sidebar refreshing its meters must never be what connects to a server.
static int u_disk_usage(void *st, uint64_t *used, uint64_t *total) {
    struct ufs_mount *um = st;
    kmutex_lock(&g_lock);
    uint64_t t = um->st.total, a = um->st.avail;
    kmutex_unlock(&g_lock);
    if (used) *used = t > a ? t - a : 0;
    if (total) *total = t;
    return 1;
}

static void u_state_free(void *st) {
    struct ufs_mount *um = st;
    kmutex_lock(&g_lock);
    while (um->head) finish(um->head, -ENODEV);
    if (um->handle > 0 && g_tab[um->handle - 1] == um) g_tab[um->handle - 1] = NULL;
    scheduler_wake(&um->take_chan, 0);      // TAKE answers -ENODEV now
    kmutex_unlock(&g_lock);
    kfree(um->ra.buf);
    kfree(um);
}

// Writes never get here (MNT_RDONLY), and every probe-time hook is for a
// backend with a volume; these refuse rather than leave a NULL to trip on.
static int u_refuse_dev(void *st, const struct block_device *dev) { (void)st; (void)dev; return -1; }
static int u_init(void *st, const struct block_device *dev, uint64_t size) { (void)st; (void)dev; (void)size; return -1; }
static int u_refuse_path(void *st, const char *p) { (void)st; (void)p; return 0; }
static int u_write(void *st, const char *p, const char *d, int a) { (void)st; (void)p; (void)d; (void)a; return 0; }
static int u_write_range(void *st, const char *p, uint64_t o, const void *b, uint32_t l) { (void)st; (void)p; (void)o; (void)b; (void)l; return 0; }
static void *u_write_range_begin(void *st, const char *p, uint64_t o, const void *b, uint32_t l) { (void)st; (void)p; (void)o; (void)b; (void)l; return 0; }
static int u_write_range_step(void *st, void *h) { (void)st; (void)h; return FS_STEP_FAILED; }
static int u_rename(void *st, const char *a, const char *b) { (void)st; (void)a; (void)b; return 0; }
static int u_truncate(void *st, const char *p, uint64_t s) { (void)st; (void)p; (void)s; return 0; }

const struct fs_ops userfs_ops = {
    .name = "userfs",
    .caps = FS_CAP_EPOCH_TIME,
    .volume_relative = 0,
    .max_mounts = MOUNT_MAX,
    .state_free = u_state_free,
    .probe = u_refuse_dev,
    .wipe = u_refuse_dev,
    .format = u_refuse_dev,
    .init = u_init,
    .touch = u_refuse_path,
    .write = u_write,
    .mkdir = u_refuse_path,
    .del = u_refuse_path,
    .size = u_size,
    .read_range = u_read_range,
    .write_range = u_write_range,
    .write_range_begin = u_write_range_begin,
    .write_range_step = u_write_range_step,
    .read_range_begin = u_read_range_begin,
    .read_range_step = u_read_range_step,
    .rename = u_rename,
    .truncate = u_truncate,
    .is_dir = u_is_dir,
    .exists = u_exists,
    .list = u_list,
    .stat = u_stat,
    .disk_usage = u_disk_usage,
};

int userfs_describe(const void *st, struct userfs_describe *out) {
    const struct ufs_mount *um = st;
    kmutex_lock(&g_lock);
    k_strlcpy(out->type, um->type, sizeof out->type);
    k_strlcpy(out->source, um->source, sizeof out->source);
    k_strlcpy(out->note, um->st.note, sizeof out->note);
    out->state = owner_alive(um) ? um->st.state : USERFS_ST_NO_SERVER;
    kmutex_unlock(&g_lock);
    return 1;
}

// ---- SYS_USERFS -------------------------------------------------------

// The live mount behind `handle`, if the caller serves it.
static struct ufs_mount *served(uint64_t handle) {
    if (handle < 1 || handle > MOUNT_MAX) return NULL;
    struct ufs_mount *um = g_tab[handle - 1];
    return um && um->owner == scheduler_current_tgid() ? um : NULL;
}

static int64_t op_mount(struct syscall_ctx *c) {
    struct userfs_mount m;
    if (!vmm_copy_from_user(c->pml4, &m, c->a1, sizeof m)) return -EFAULT;
    m.point[sizeof m.point - 1] = m.type[sizeof m.type - 1] = m.source[sizeof m.source - 1] = '\0';
    if (m.point[0] != '/' || !m.type[0]) return -EINVAL;
    int me = scheduler_current_tgid();

    // A point this backend already serves: adopt it if its server is gone.
    kmutex_lock(&g_lock);
    for (int i = 0; i < MOUNT_MAX; i++) {
        struct ufs_mount *um = g_tab[i];
        if (!um || k_strcmp(um->point, m.point)) continue;
        if (owner_alive(um) && um->owner != me) { kmutex_unlock(&g_lock); return -EBUSY; }
        um->owner = me;
        k_strlcpy(um->type, m.type, sizeof um->type);
        k_strlcpy(um->source, m.source, sizeof um->source);
        um->st.state = USERFS_ST_IDLE;
        // Whatever the old server took and never answered goes to the new one.
        for (struct ufs_req *r = um->head; r; r = r->next) r->taken = 0;
        kmutex_unlock(&g_lock);
        klog_printf("userfs: %s adopted by pid %d\n", m.point, me);
        return um->handle;
    }
    int slot = -1;
    for (int i = 0; i < MOUNT_MAX && slot < 0; i++) if (!g_tab[i]) slot = i;
    kmutex_unlock(&g_lock);
    if (slot < 0) return -ENOSPC;

    struct ufs_mount *um = kzalloc(sizeof *um);
    if (!um) return -ENOMEM;
    um->owner = me;
    um->handle = slot + 1;
    k_strlcpy(um->point, m.point, sizeof um->point);
    k_strlcpy(um->type, m.type, sizeof um->type);
    k_strlcpy(um->source, m.source, sizeof um->source);
    um->st.state = USERFS_ST_IDLE;
    struct rtc_time now;
    ktime_read(&now);
    um->mounted_at = cal_rtc_to_epoch(&now);
    kmutex_lock(&g_lock);
    if (g_tab[slot]) { kmutex_unlock(&g_lock); kfree(um); return -EAGAIN; }
    g_tab[slot] = um;           // reserved before the mount, so TAKE can find it
    kmutex_unlock(&g_lock);
    const char *why = "";
    if (!mount_add_nodev(&userfs_ops, um, m.point, MNT_RDONLY, &why)) {
        kmutex_lock(&g_lock);
        g_tab[slot] = NULL;
        kmutex_unlock(&g_lock);
        kfree(um);
        klog_printf(KLOG_ERR "userfs: cannot mount %s: %s\n", m.point, why);
        return -EBUSY;
    }
    return um->handle;
}

static int op_take(struct syscall_ctx *c) {
    kmutex_lock(&g_lock);
    struct ufs_mount *um = served(c->a1);
    if (!um) {
        kmutex_unlock(&g_lock);
        c->regs[14] = (uint64_t)(int64_t)-ENODEV;
        return 0;
    }
    struct ufs_req *r = um->head;
    while (r && r->taken) r = r->next;
    if (r) {
        int ok = vmm_copy_to_user(c->pml4, c->a2, &r->msg, sizeof r->msg);
        if (ok) r->taken = 1;
        kmutex_unlock(&g_lock);
        c->regs[14] = ok ? 1 : (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    // ARMED UNDER THE LOCK a queuer takes before it wakes us, so the
    // wake cannot fall between this test and the park.
    scheduler_wait_arm(&um->take_chan);
    kmutex_unlock(&g_lock);
    c->regs[14] = 0;
    scheduler_block_current(c->regs, &um->take_chan, SCHED_WAIT_EVENT);
    return 1;
}

static int64_t op_reply(struct syscall_ctx *c) {
    struct userfs_reply rp;
    if (!vmm_copy_from_user(c->pml4, &rp, c->a1, sizeof rp)) return -EFAULT;
    int me = scheduler_current_tgid();
    int64_t rc = 0;
    kmutex_lock(&g_lock);
    struct ufs_req *found = NULL;
    for (int i = 0; i < MOUNT_MAX && !found; i++) {
        struct ufs_mount *um = g_tab[i];
        if (!um || um->owner != me) continue;
        for (struct ufs_req *r = um->head; r; r = r->next)
            if (r->id == rp.id && r->taken) { found = r; break; }
    }
    if (found) {
        uint32_t n = rp.len < found->cap ? rp.len : found->cap;
        if (rp.result >= 0 && n && !vmm_copy_from_user(c->pml4, found->buf, rp.buf, n)) {
            rc = -EFAULT;
            finish(found, -EIO);
        } else {
            found->got = rp.result >= 0 ? n : 0;
            found->flags = rp.flags;
            finish(found, rp.result);
        }
    }
    kmutex_unlock(&g_lock);
    return rc;   // a request nobody waits for any more is not an error
}

static int64_t op_state(struct syscall_ctx *c) {
    struct userfs_state s;
    if (!vmm_copy_from_user(c->pml4, &s, c->a1, sizeof s)) return -EFAULT;
    s.note[sizeof s.note - 1] = '\0';
    kmutex_lock(&g_lock);
    struct ufs_mount *um = served(s.handle);
    if (um) um->st = s;
    kmutex_unlock(&g_lock);
    return um ? 0 : -ENODEV;
}

static int64_t op_ctl(struct syscall_ctx *c) {
    struct userfs_ctl *u = kmalloc(sizeof *u);
    struct ufs_req *r = req_new(USERFS_OP_CTL, "/");
    if (!u || !r) { kfree(u); kfree(r); return -ENOMEM; }
    int64_t rc = -EFAULT;
    if (!vmm_copy_from_user(c->pml4, u, c->a1, sizeof *u)) goto out;
    u->point[sizeof u->point - 1] = '\0';
    rc = -EINVAL;
    if (u->len > USERFS_CTL_MAX) goto out;
    r->msg.ctl = u->code;
    r->msg.data_len = u->len;
    k_memcpy(r->msg.data, u->data, u->len);

    // Looked up and queued under ONE hold of g_lock, which an unmount
    // also takes: the mount cannot be freed in between.
    kmutex_lock(&g_lock);
    struct ufs_mount *um = NULL;
    for (int i = 0; i < MOUNT_MAX && !um; i++)
        if (g_tab[i] && !k_strcmp(g_tab[i]->point, u->point)) um = g_tab[i];
    rc = um ? enqueue_locked(um, r) : -ENOENT;
    kmutex_unlock(&g_lock);
    if (!rc) rc = wait_for(NULL, r, NULL);
out:
    k_memset(u, 0, sizeof *u);      // a password, often
    k_memset(&r->msg.data, 0, sizeof r->msg.data);
    kfree(u);
    kfree(r);
    return rc;
}

int sys_userfs(struct syscall_ctx *c) {
    int64_t rc;
    switch (c->a0) {
    case USERFS_MOUNT: rc = op_mount(c); break;
    case USERFS_TAKE:  return op_take(c);
    case USERFS_REPLY: rc = op_reply(c); break;
    case USERFS_STATE: rc = op_state(c); break;
    case USERFS_CTL:   rc = op_ctl(c); break;
    default:           rc = -EINVAL; break;
    }
    c->regs[14] = (uint64_t)rc;
    return 0;
}
