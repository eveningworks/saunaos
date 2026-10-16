// The sandbox -- SYS_PLEDGE and SYS_UNVEIL, OpenBSD's pledge(2) and
// unveil(2) in shape and in name (abi/sandbox_abi.h has the contract;
// docs/decisions/kernel.md, "A sandbox is a promise set and a path list",
// has why this shape and not seccomp's or Capsicum's).
//
// A process's sandbox lives in its LEADER's slot (sched_slot_ext), read
// through scheduler_current_sandbox() like the cwd, so every thread of a
// program shares one. It is SHARED between a process and the children
// that inherited it and COPIED before a pledge or unveil changes it --
// a child's later pledge must not narrow its parent.
#include "sandbox.h"
#include "sandbox_abi.h"
#include "syscall_abi.h"
#include "signal_abi.h"
#include "syscalls.h"
#include "scheduler.h"
#include "crash_report.h"
#include "fs.h"            // FS_PATH_MAX
#include "kpath_buf.h"     // kpath_get(): a path may not be a kernel local
#include "heap.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "errno.h"
#include "vmm.h"

struct unveil_entry {
    char    *path;     // kmalloc'd; absolute, or "shm:<name>"
    uint16_t len;
    uint8_t  rights;   // UNVEIL_*
};

struct sandbox {
    int      refs;
    uint32_t promises;
    uint8_t  unveiled;   // anything unveiled: what is not, is hidden
    uint8_t  locked;     // SYS_UNVEIL(NULL): the list is final
    uint16_t logged;     // unveil refusals logged so far, for the cap below
    int      n;
    struct unveil_entry v[UNVEIL_MAX];
};

// The first few refusals of each sandbox are logged and the rest are not:
// an engine probing for a file it does not need asks again on every page,
// and a log full of those buries the one that matters (CLAUDE.md, a probe
// that outruns the log).
#define LOG_REFUSALS_MAX 8

struct sandbox *sandbox_get(struct sandbox *sb) {
    if (sb) sb->refs++;
    return sb;
}

void sandbox_put(struct sandbox *sb) {
    if (!sb || --sb->refs > 0) return;
    for (int i = 0; i < sb->n; i++) kfree(sb->v[i].path);
    kfree(sb);
}

uint32_t sandbox_promises(const struct sandbox *sb) {
    return sb ? sb->promises : PROMISE_ALL;
}

static struct sandbox *current(void) {
    struct sandbox **slot = scheduler_current_sandbox();
    return slot ? *slot : 0;
}

int sandbox_active(void) { return current() != 0; }

// The current process's sandbox, made private to it so it can be
// changed: created unrestricted if there is none, copied if a child
// shares it. NULL only when the heap is out.
static struct sandbox *writable(void) {
    struct sandbox **slot = scheduler_current_sandbox();
    if (!slot) return 0;
    struct sandbox *sb = *slot;
    if (sb && sb->refs == 1) return sb;
    struct sandbox *c = kmalloc(sizeof *c);
    if (!c) return 0;
    k_memset(c, 0, sizeof *c);
    c->refs = 1;
    c->promises = PROMISE_ALL;
    if (sb) {
        c->promises = sb->promises;
        c->unveiled = sb->unveiled;
        c->locked = sb->locked;
        for (int i = 0; i < sb->n; i++) {
            c->v[i] = sb->v[i];
            c->v[i].path = kmalloc((size_t)sb->v[i].len + 1);
            if (!c->v[i].path) { c->n = i; sandbox_put(c); return 0; }
            k_memcpy(c->v[i].path, sb->v[i].path, (size_t)sb->v[i].len + 1);
        }
        c->n = sb->n;
        sandbox_put(sb);
    }
    *slot = c;
    return c;
}

// --- SPAWN_SANDBOX's arm ---------------------------------------------------

#define ARMS 8
static struct { int pid; uint32_t promises; } g_arm[ARMS];

int sandbox_arm_spawn(uint32_t promises) {
    int pid = scheduler_current_tgid();
    for (int i = 0; i < ARMS; i++) {
        if (g_arm[i].pid) continue;
        g_arm[i].pid = pid;
        g_arm[i].promises = promises;
        return 0;
    }
    return -EBUSY;
}

void sandbox_disarm(void) {
    int pid = scheduler_current_tgid();
    for (int i = 0; i < ARMS; i++)
        if (g_arm[i].pid == pid) g_arm[i].pid = 0;
}

int sandbox_for_child(struct sandbox **out) {
    struct sandbox *parent = current();
    int pid = scheduler_current_tgid();
    int arm = -1;
    for (int i = 0; pid && i < ARMS; i++)
        if (g_arm[i].pid == pid) arm = i;
    *out = 0;
    if (arm < 0) { *out = sandbox_get(parent); return 0; }

    // A NEW sandbox for the child alone -- narrowing the parent's shared
    // one would narrow the parent.
    struct sandbox *c = kmalloc(sizeof *c);
    if (!c) return -ENOMEM;
    k_memset(c, 0, sizeof *c);
    c->refs = 1;
    c->promises = sandbox_promises(parent) & g_arm[arm].promises;
    if (parent) {
        c->unveiled = parent->unveiled;
        c->locked = parent->locked;
        for (int i = 0; i < parent->n; i++) {
            c->v[i] = parent->v[i];
            c->v[i].path = kmalloc((size_t)parent->v[i].len + 1);
            if (!c->v[i].path) { c->n = i; sandbox_put(c); return -ENOMEM; }
            k_memcpy(c->v[i].path, parent->v[i].path, (size_t)parent->v[i].len + 1);
        }
        c->n = parent->n;
    }
    *out = c;
    return 0;
}

// Never freed: its count starts too high for every process on the
// machine to drop it to zero.
static struct sandbox g_lockout = { .refs = 1 << 30, .unveiled = 1, .locked = 1 };

void sandbox_lockout(struct sandbox **slot) {
    *slot = sandbox_get(&g_lockout);
}

// --- promises ---------------------------------------------------------------

int sandbox_allows(uint32_t promise) {
    struct sandbox *sb = current();
    return !sb || (sb->promises & promise) == promise;
}

static const char *promise_name(uint32_t p) {
    static const char *const names[] = PROMISE_WORDS;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (p & (1u << i)) return names[i];
    return "?";
}

void sandbox_violation(const char *call, uint32_t promise, uint64_t *regs) {
    int pid = scheduler_current_tgid();
    char exec[64];
    if (!scheduler_exec_path(pid, exec, sizeof exec)) k_strlcpy(exec, "?", sizeof exec);
    klog_printf(KLOG_WARN "sandbox: pid %d (%s) called %s without the '%s' promise -- killed\n",
                pid, exec, call, promise_name(promise));
    char what[40];   // QUERY_CRASH_FAULT_MAX
    k_snprintf(what, sizeof what, "Sandbox: %s needs '%s'", call, promise_name(promise));
    crash_report_write(what, regs, 0, 0);
    scheduler_exit_group(SIGNAL_EXIT_BASE + SIGSYS);
}

int sandbox_require(uint32_t promise, const char *call, uint64_t *regs) {
    if (sandbox_allows(promise)) return 0;
    sandbox_violation(call, promise, regs);
    return 1;
}

int sys_pledge(struct syscall_ctx *c) {
    uint32_t keep = (uint32_t)c->a0;
    int64_t rc = 0;
    if (keep & ~PROMISE_ALL) {
        rc = -EINVAL;
    } else if (!sandbox_allows(keep)) {
        // Asking for a bit already given up: refused LOUDLY, so a program
        // that believes it holds something finds out at the pledge, not
        // as a SIGSYS three calls later.
        rc = -EPERM;
    } else if (keep != PROMISE_ALL || current()) {
        struct sandbox *sb = writable();
        if (!sb) rc = -ENOMEM;
        else sb->promises &= keep;
    }
    c->regs[14] = (uint64_t)rc;
    return 0;
}

// --- unveil -----------------------------------------------------------------

// Does entry `e` cover `p`? The entry itself, or anything beneath it --
// "/usr/share" covers "/usr/share/fonts/a.ttf" and not "/usr/shared".
static int covers(const struct unveil_entry *e, const char *p) {
    if (k_strncmp(e->path, p, e->len) != 0) return 0;
    char next = p[e->len];
    if (!next) return 1;
    if (e->path[e->len - 1] == '/' || e->path[e->len - 1] == ':') return 1;  // "/" and "shm:"
    return next == '/';
}

// The most specific entry covering `p` -- a deeper unveil overrides a
// shallower one, as OpenBSD's does -- or NULL.
static const struct unveil_entry *match(const struct sandbox *sb, const char *p) {
    const struct unveil_entry *best = 0;
    for (int i = 0; i < sb->n; i++)
        if (covers(&sb->v[i], p) && (!best || sb->v[i].len > best->len)) best = &sb->v[i];
    return best;
}

static int check(const char *p, unsigned need, const char *kind) {
    struct sandbox *sb = current();
    if (!sb || !sb->unveiled) return 0;
    const struct unveil_entry *e = match(sb, p);
    int rc = !e ? -ENOENT : (e->rights & need) != need ? -EACCES : 0;
    if (rc && sb->logged < LOG_REFUSALS_MAX) {
        sb->logged++;
        klog_printf("sandbox: pid %d refused %s %s (%s)%s\n", scheduler_current_tgid(), kind, p,
                    rc == -ENOENT ? "not unveiled" : "not with that right",
                    sb->logged == LOG_REFUSALS_MAX ? " -- further refusals not logged" : "");
    }
    return rc;
}

int sandbox_path(const char *abs, unsigned need) { return check(abs, need, "path"); }

int sandbox_shm_public(const char *name) {
    struct sandbox *sb = current();
    if (!sb || !sb->unveiled) return 1;
    char full[sizeof UNVEIL_SHM_PREFIX + SHM_NAME_MAX];
    k_snprintf(full, sizeof full, UNVEIL_SHM_PREFIX "%s", name);
    return check(full, UNVEIL_R, "shm") == 0;
}

int sys_unveil(struct syscall_ctx *c) {
    int64_t rc = 0;
    char *path = 0;
    unsigned rights = (unsigned)c->a1;
    struct sandbox *cur = current();

    if (cur && cur->locked) { rc = -EPERM; goto out; }
    if (!c->a0) {
        // LOCK. With nothing unveiled this also hides everything, which
        // is what unveil(NULL, NULL) as the first call means on OpenBSD.
        struct sandbox *sb = writable();
        if (!sb) { rc = -ENOMEM; goto out; }
        sb->unveiled = 1;
        sb->locked = 1;
        goto out;
    }
    if (rights & ~(unsigned)(UNVEIL_R | UNVEIL_W | UNVEIL_C | UNVEIL_X)) { rc = -EINVAL; goto out; }

    path = kpath_get();
    if (!path) { rc = -ENOMEM; goto out; }
    if (!vmm_copy_string_from_user(c->pml4, path, c->a0, FS_PATH_MAX)) { rc = -EFAULT; goto out; }
    if (!k_strncmp(path, UNVEIL_SHM_PREFIX, sizeof UNVEIL_SHM_PREFIX - 1)) {
        if (k_strlen(path) >= sizeof UNVEIL_SHM_PREFIX - 1 + SHM_NAME_MAX) {
            rc = -ENAMETOOLONG; goto out;
        }
    } else if ((rc = resolve_user_path(c->pml4, c->a0, path, 0)) < 0) {
        goto out;   // 0 = no unveil check: this call is what defines them
    }

    // **OPEN UNTIL LOCKED, as OpenBSD's is**: a program unveils the
    // handful of paths it needs, one call each, then locks. Before the
    // lock any call may add or re-right a path -- which widens nothing
    // the process could not have kept by not unveiling at all; it is the
    // LOCK that makes the list a promise. A process that never locks has
    // only hidden things from its own bugs, not from an exploit.
    struct sandbox *sb = writable();
    if (!sb) { rc = -ENOMEM; goto out; }
    size_t len = k_strlen(path);
    if (len > 1 && path[len - 1] == '/' && path[0] == '/') path[--len] = '\0';
    for (int i = 0; i < sb->n; i++) {
        if (sb->v[i].len == len && !k_strcmp(sb->v[i].path, path)) {
            sb->v[i].rights = (uint8_t)rights;
            sb->unveiled = 1;
            goto out;
        }
    }
    if (sb->n >= UNVEIL_MAX) { rc = -E2BIG; goto out; }
    char *copy = kmalloc(len + 1);
    if (!copy) { rc = -ENOMEM; goto out; }
    k_memcpy(copy, path, len + 1);
    sb->v[sb->n].path = copy;
    sb->v[sb->n].len = (uint16_t)len;
    sb->v[sb->n].rights = (uint8_t)rights;
    sb->n++;
    sb->unveiled = 1;
out:
    if (path) kpath_put(path);
    c->regs[14] = (uint64_t)rc;
    return 0;
}
