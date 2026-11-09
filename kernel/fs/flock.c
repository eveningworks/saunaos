// POSIX record locks: fcntl()'s F_GETLK, F_SETLK and F_SETLKW
// (kernel/include/kernel/flock.h has the rules; this is the table).
//
// ONE TABLE FOR THE SYSTEM, grown to fit and edited only with the
// preemption guard up -- the heap's own rule (heap_os.c): nothing here
// sleeps or is reached from an interrupt, so the guard is the lock. The
// two things that may sleep, finding a file's identity (fs_stat()) and
// copying the caller's struct, happen before the guard goes up.
#include "flock.h"
#include "syscalls.h"
#include "syscall_abi.h"
#include "scheduler.h"
#include "fs.h"
#include "mount.h"
#include "heap.h"
#include "string.h"
#include "errno.h"
#include "vmm.h"

struct rlock {
    const void *mnt;    // the file: its mount and inode, so two opens of
    uint64_t ino;       // it -- or two names, hard links -- are one file
    uint64_t owner;     // the descriptor table's key (CR3): the PROCESS
    int pid;            // what F_GETLK reports as l_pid
    uint8_t type;       // SYS_LK_RD or SYS_LK_WR
    uint64_t start, end;   // inclusive; end UINT64_MAX is "to the end, and past it"
};

static struct rlock *g_l;
static int g_n, g_cap;
static const char g_chan;   // every waiter parks here; a release wakes them to look again

#define TO_EOF ((uint64_t)-1)

// Room for `need` entries. Under the guard (kmalloc does not sleep).
static int reserve(int need) {
    if (need <= g_cap) return 1;
    int cap = g_cap ? g_cap * 2 : 8;
    while (cap < need) cap *= 2;
    struct rlock *n = kmalloc((size_t)cap * sizeof *n);
    if (!n) return 0;
    if (g_n) k_memcpy(n, g_l, (size_t)g_n * sizeof *n);
    kfree(g_l);
    g_l = n;
    g_cap = cap;
    return 1;
}

static int same_file(const struct rlock *l, const void *mnt, uint64_t ino) {
    return l->mnt == mnt && l->ino == ino;
}

static int overlaps(const struct rlock *l, uint64_t s, uint64_t e) {
    return l->start <= e && s <= l->end;
}

// Another owner's lock that a `type` lock over [s, e] would collide with:
// any writer, or a reader against a writer. Its index, or -1.
static int conflict(const void *mnt, uint64_t ino, uint64_t owner, int type,
                    uint64_t s, uint64_t e) {
    for (int i = 0; i < g_n; i++) {
        const struct rlock *l = &g_l[i];
        if (l->owner == owner || !same_file(l, mnt, ino) || !overlaps(l, s, e)) continue;
        if (type == SYS_LK_WR || l->type == SYS_LK_WR) return i;
    }
    return -1;
}

static void drop(int i) { g_l[i] = g_l[--g_n]; }

// Takes [s, e] out of every lock `owner` holds on the file, splitting a
// lock the range falls inside. Needs one free slot per split, which the
// caller reserved (a range inside ONE lock splits it once; a range
// spanning several only trims them). 1 if anything changed.
static int punch(const void *mnt, uint64_t ino, uint64_t owner, uint64_t s, uint64_t e) {
    int changed = 0;
    for (int i = 0; i < g_n; i++) {
        struct rlock *l = &g_l[i];
        if (l->owner != owner || !same_file(l, mnt, ino) || !overlaps(l, s, e)) continue;
        changed = 1;
        int left = l->start < s, right = l->end > e;
        if (left && right) {                    // the hole is inside: two pieces
            struct rlock tail = *l;
            tail.start = e + 1;
            l->end = s - 1;
            g_l[g_n++] = tail;
        } else if (left) {
            l->end = s - 1;
        } else if (right) {
            l->start = e + 1;
        } else {
            drop(i--);
        }
    }
    return changed;
}

// Every lock matching `owner` (and, when `mnt`, that file) goes.
static int release(uint64_t owner, const void *mnt, uint64_t ino) {
    int n = 0;
    for (int i = 0; i < g_n; i++)
        if (g_l[i].owner == owner && (!mnt || same_file(&g_l[i], mnt, ino))) { drop(i--); n++; }
    return n;
}

static int owner_holds(uint64_t owner) {
    for (int i = 0; i < g_n; i++)
        if (g_l[i].owner == owner) return 1;
    return 0;
}

// The file behind a description: its mount and inode. 0, or -ENOLCK
// when the name no longer resolves (deleted since it was opened -- the
// name is all a description keeps).
static int file_key(const struct open_file *f, const void **mnt, uint64_t *ino,
                    struct fs_stat_info *st) {
    if (!fs_stat(f->file.name, st)) return -ENOLCK;
    const char *sub = 0;
    *mnt = mount_resolve(f->file.name, &sub);
    *ino = st->ino;
    return *mnt ? 0 : -ENOLCK;
}

void flock_close(uint64_t owner, struct open_file *f) {
    if (!f || f->ops != &file_fd_ops) return;
    scheduler_preempt_disable();
    int any = owner_holds(owner);
    scheduler_preempt_enable();
    if (!any) return;   // the common close: no stat, no walk past this
    const void *mnt;
    uint64_t ino;
    struct fs_stat_info st;
    if (file_key(f, &mnt, &ino, &st) < 0) return;
    scheduler_preempt_disable();
    int n = release(owner, mnt, ino);
    scheduler_preempt_enable();
    if (n) scheduler_wake((const void *)&g_chan, SYS_RETRY);
}

void flock_release_owner(uint64_t owner) {
    scheduler_preempt_disable();
    int n = release(owner, 0, 0);
    scheduler_preempt_enable();
    if (n) scheduler_wake((const void *)&g_chan, SYS_RETRY);
}

void flock_rekey(uint64_t old_owner, uint64_t new_owner) {
    scheduler_preempt_disable();
    for (int i = 0; i < g_n; i++)
        if (g_l[i].owner == old_owner) g_l[i].owner = new_owner;
    scheduler_preempt_enable();
}

int flock_count(void) { return g_n; }

// [start, end] from a struct flock's whence/start/len, as POSIX reads
// them: len 0 is "to the end of the file, however far it grows", and a
// NEGATIVE len is the bytes BEFORE start.
static int range_of(const struct sys_flock *fl, const struct open_file *f, uint64_t size,
                    uint64_t *out_s, uint64_t *out_e) {
    int64_t base;
    switch (fl->whence) {
    case SYS_LK_SEEK_SET: base = 0; break;
    case SYS_LK_SEEK_CUR: base = (int64_t)f->file.pos; break;
    case SYS_LK_SEEK_END: base = (int64_t)size; break;
    default: return -EINVAL;
    }
    int64_t s = base + fl->start;
    if ((fl->start > 0 && s < base) || s < 0) return -EINVAL;
    if (fl->len == 0) { *out_s = (uint64_t)s; *out_e = TO_EOF; return 0; }
    if (fl->len > 0) {
        if (s > INT64_MAX - fl->len) return -EOVERFLOW;
        *out_s = (uint64_t)s;
        *out_e = (uint64_t)(s + fl->len - 1);
        return 0;
    }
    if (s + fl->len < 0) return -EINVAL;
    *out_s = (uint64_t)(s + fl->len);
    *out_e = (uint64_t)(s - 1);
    return s == 0 ? -EINVAL : 0;
}

int flock_op(uint64_t pml4, int pid, struct open_file *f, int cmd, struct sys_flock *fl,
             uint64_t *regs, int *parked) {
    *parked = 0;
    if (!f || f->ops != &file_fd_ops) return -EINVAL;   // a pipe, a socket: no locks
    if (cmd != SYS_LK_GET && cmd != SYS_LK_SET && cmd != SYS_LK_SETW) return -EINVAL;
    int type = fl->type;
    if (type != SYS_LK_RD && type != SYS_LK_WR && type != SYS_LK_UN) return -EINVAL;
    if (cmd == SYS_LK_GET && type == SYS_LK_UN) return -EINVAL;
    // POSIX: a read lock needs a descriptor open for reading, a write
    // lock one open for writing.
    if (cmd != SYS_LK_GET) {
        if (type == SYS_LK_RD && !fd_mode_reads(f->file.mode)) return -EBADF;
        if (type == SYS_LK_WR && !fd_mode_writes(f->file.mode)) return -EBADF;
    }
    const void *mnt;
    uint64_t ino, s, e;
    struct fs_stat_info st;
    int rc = file_key(f, &mnt, &ino, &st);
    if (rc < 0) return rc;
    rc = range_of(fl, f, st.size, &s, &e);
    if (rc < 0) return rc;

    scheduler_preempt_disable();
    if (cmd == SYS_LK_GET) {
        int c = conflict(mnt, ino, pml4, type, s, e);
        if (c < 0) {
            fl->type = SYS_LK_UN;
        } else {
            fl->type = g_l[c].type;
            fl->whence = SYS_LK_SEEK_SET;
            fl->start = (int64_t)g_l[c].start;
            fl->len = g_l[c].end == TO_EOF ? 0 : (int64_t)(g_l[c].end - g_l[c].start + 1);
            fl->pid = g_l[c].pid;
        }
        scheduler_preempt_enable();
        return 0;
    }
    if (type != SYS_LK_UN && conflict(mnt, ino, pml4, type, s, e) >= 0) {
        // CHECK AND PARK UNDER ONE GUARD, as a pipe read does: a release
        // between them would wake nobody, and the wait would never end.
        if (cmd == SYS_LK_SETW && scheduler_block_current(regs, (const void *)&g_chan, SCHED_WAIT_LOCK)) {
            *parked = 1;
            scheduler_preempt_enable();
            return 0;
        }
        scheduler_preempt_enable();
        return -EAGAIN;
    }
    // Two slots: a punch splits at most one lock, and the new one.
    if (!reserve(g_n + 2)) { scheduler_preempt_enable(); return -ENOLCK; }
    int changed = punch(mnt, ino, pml4, s, e);
    if (type != SYS_LK_UN)
        g_l[g_n++] = (struct rlock){ mnt, ino, pml4, pid, (uint8_t)type, s, e };
    scheduler_preempt_enable();
    // Anything released or downgraded may be what a waiter wants.
    if (changed) scheduler_wake((const void *)&g_chan, SYS_RETRY);
    return 0;
}

// SYS_FCNTL_LOCK: RDI = fd, RSI = SYS_LK_GET/SET/SETW, RDX = struct sys_flock *.
int sys_fcntl_lock(struct syscall_ctx *c) {
    struct sys_flock fl;
    if (!vmm_copy_from_user(c->pml4, &fl, c->a2, sizeof fl)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    struct open_file *f = fd_get(c->pml4, (int)c->a0);
    if (!f) { c->regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }
    int parked = 0;
    int rc = flock_op(c->pml4, scheduler_current_tgid(), f, (int)c->a1, &fl, c->regs, &parked);
    if (parked) return 0;   // the wake writes SYS_RETRY, and libsys asks again
    if (rc == 0 && c->a1 == SYS_LK_GET && !vmm_copy_to_user(c->pml4, c->a2, &fl, sizeof fl))
        rc = -EFAULT;
    c->regs[14] = (uint64_t)(int64_t)rc;
    return 0;
}
