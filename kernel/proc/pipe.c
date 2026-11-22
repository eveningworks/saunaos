// See pipe.h for the design and what this deliberately isn't.
#include "pipe.h"
#include "scheduler.h"
#include "syscalls.h"
#include "errno.h"
#include "klog.h"
#include "heap.h"
#include "vmm.h"
#include "signal.h"   // signal_send -- SIGPIPE on a dead pipe
#include "syscall_abi.h" // SYS_RETRY -- the wake value blocked readers see
#include "kslots.h"
#include "spinlock.h"
#include "syscall_table.h" // SYSCALL_DECLINE
#include <stddef.h>

// **EVERY FIELD UNDER `lock`**, from any core, with or without the kernel
// lock (FreeBSD's pipe mutex). A waiter ARMS on the pipe's channel before
// it lets the lock go, and a waker wakes after it lets it go: a wake then
// either finds the arm or the BLOCKED process (scheduler_wake_n()).
// Nothing is taken under it but the scheduler lock (the arm).
struct pipe {
    char buf[PIPE_BUF_SIZE];
    int head;    // next byte to read
    int count;   // bytes buffered
    int readers;
    int writers;
    struct spinlock lock;
};

// GROWN ON DEMAND (kslots.h): created from syscall context, where an
// allocation failure is an ordinary -1, and released without freeing,
// so process teardown never allocates and never frees.
static struct kslots g_pipes = KSLOTS_INIT(sizeof(struct pipe));

static struct pipe *at(int idx) { return kslots_at(&g_pipes, idx); }

int pipe_valid(int idx) { return at(idx) != NULL; }

// This pipe's wait channel -- see scheduler.h. The struct's own address,
// so a waiter and a waker name the same thing without a second table,
// and a write to one pipe cannot wake the readers of another.
const void *pipe_wait_chan(int idx) { return at(idx); }

int pipe_create(void) {
    int i = kslots_alloc(&g_pipes);   // zeroed: empty, head 0
    if (i < 0) return -1;
    struct pipe *p = at(i);
    spin_lock_name(&p->lock, "pipe");
    p->readers = 1;
    p->writers = 1;
    return i;
}

void pipe_add_reader(int idx) {
    struct pipe *p = at(idx);
    if (!p) return;
    spin_lock(&p->lock);
    p->readers++;
    spin_unlock(&p->lock);
}
void pipe_add_writer(int idx) {
    struct pipe *p = at(idx);
    if (!p) return;
    spin_lock(&p->lock);
    p->writers++;
    spin_unlock(&p->lock);
}

// Drops one end; 1 when that end's count reached 0 (a wake is owed), and
// `*orphaned` when neither end is left -- decided under the lock, so of
// two last closes on two cores exactly one frees the slot. A close runs
// under the kernel lock (syscall_fd.c's put_lockless() takes it for a last put), as
// kslots' alloc and free need.
static int drop_end(struct pipe *p, int reader, int *orphaned) {
    spin_lock(&p->lock);
    int *n = reader ? &p->readers : &p->writers;
    if (*n > 0) (*n)--;
    int last = *n == 0;
    *orphaned = p->readers <= 0 && p->writers <= 0;
    spin_unlock(&p->lock);
    return last;
}

void pipe_close_reader(int idx) {
    struct pipe *p = at(idx);
    if (!p) return;
    int orphaned;
    if (drop_end(p, 1, &orphaned)) {
        // The mirror of the writer case below, and load-bearing now
        // that a full pipe PARKS its writer: the last reader going away
        // is what turns that block into "discarded, nobody is
        // listening". Without this wake, a writer parked on a full pipe
        // whose reader then died would wait forever for room that can
        // never be made -- a hang rather than the dropped write pipe.h
        // documents.
        scheduler_wake(p, SYS_RETRY);
    }
    if (orphaned) kslots_free(&g_pipes, idx);
}

void pipe_close_writer(int idx) {
    struct pipe *p = at(idx);
    if (!p) return;
    int orphaned;
    if (drop_end(p, 0, &orphaned)) {
        // The last writer going away is what turns a blocking read into
        // EOF. A reader parked right now would otherwise wait forever
        // for data that can no longer arrive.
        scheduler_wake(p, SYS_RETRY);
    }
    if (orphaned) kslots_free(&g_pipes, idx);
}

// pipe_write() under the pipe's lock, and without its wake.
static int64_t write_locked(struct pipe *p, const char *src, uint32_t len) {
    if (p->readers <= 0) return 0; // nobody will ever read it -- see pipe.h

    // ALL OR NOTHING. A write that does not fit takes NONE of the bytes
    // and reports "would block", so the caller can park and retry the
    // whole thing -- the mirror of pipe_read() returning -1 on an empty
    // pipe with a live writer.
    //
    // This used to take what fitted and report a short count, which is
    // a correct-looking answer that nothing in ring 3 acts on: no
    // program here loops on a short write, so a producer faster than
    // its reader silently LOST the remainder. A pipeline makes that
    // unavoidable rather than unlucky, since the reader is another
    // process that may not have been scheduled yet.
    //
    // Atomicity is affordable because pipe_fd_write() CLAMPS a pipe
    // write to PIPE_BUF_SIZE, so any one write fits once the pipe drains
    // and this cannot deadlock on a request too big to ever satisfy.
    // POSIX guarantees exactly this for writes up to PIPE_BUF, and for
    // the same reason. The clamp used to be free -- SYS_WRITE_MAX was
    // under PIPE_BUF_SIZE -- and stopped being when the cap was raised;
    // see api/pipe.h.
    if (len > (uint32_t)(PIPE_BUF_SIZE - p->count)) return -1; // would block

    int64_t written = 0;
    while ((uint32_t)written < len) {
        p->buf[(p->head + p->count) % PIPE_BUF_SIZE] = src[written];
        p->count++;
        written++;
    }
    return written;
}

int64_t pipe_write(int idx, const char *src, uint32_t len) {
    struct pipe *p = at(idx);
    if (!p || !src) return 0;
    spin_lock(&p->lock);
    int64_t n = write_locked(p, src, len);
    spin_unlock(&p->lock);
    // Wake any parked reader. Harmless when none is: scheduler_wake()
    // returns 0 and does nothing.
    if (n > 0) scheduler_wake(p, SYS_RETRY);
    return n;
}

// pipe_read() under the pipe's lock, and without its wake.
static int64_t read_locked(struct pipe *p, char *dst, uint32_t len) {
    if (p->count == 0) {
        // Empty. The answer depends entirely on whether more can ever
        // arrive -- see pipe.h on why conflating these two is the bug
        // that makes a terminal think a running program has exited.
        if (p->writers > 0) return -1; // would block
        return 0;                      // EOF
    }

    int64_t n = 0;
    while ((uint32_t)n < len && p->count > 0) {
        dst[n] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count--;
        n++;
    }
    return n;
}

int64_t pipe_read(int idx, char *dst, uint32_t len) {
    struct pipe *p = at(idx);
    if (!p || !dst) return 0;
    spin_lock(&p->lock);
    int64_t n = read_locked(p, dst, len);
    spin_unlock(&p->lock);
    // Draining makes room, so a WRITER parked on a full pipe can now
    // proceed. Readers and writers share THIS PIPE's channel and this
    // wakes both -- which is correct rather than merely tolerable:
    // every waiter re-runs its syscall and re-parks if it is still not
    // ready, so a spurious wake costs a syscall and never a wrong
    // answer. What it no longer does is disturb the readers of OTHER
    // pipes, which is what a shared category channel did.
    if (n > 0) scheduler_wake(p, SYS_RETRY);
    return n;
}

// --- a pipe as a pair of descriptors ---------------------------------
//
// Returns 1 if the caller was PARKED (its syscall has no return value
// yet -- the wake writes it), 0 otherwise, or SYSCALL_DECLINE from the
// lock-free path (c->lockless), having consumed nothing.
//
// **CHECK AND PARK MUST BE ATOMIC AGAINST THE OTHER END**: "empty -> park"
// interrupted by the writer loses its wake, and with both ends able to
// sleep that is a deadlock (reader parked on an empty check made before
// the pipe filled, writer parked on a full one). So the test is made under
// the pipe's lock and the waiter ARMS before letting it go (struct pipe).

// Parks on `p` having armed under its lock, by whichever path is running.
static int park(struct syscall_ctx *c, struct pipe *p) {
    if (c->lockless) {
        if (scheduler_block_current_lockless(c->regs, p, SCHED_WAIT_PIPE)) return 1;
        scheduler_wait_disarm();
        return SYSCALL_DECLINE;
    }
    return scheduler_block_current(c->regs, p, SCHED_WAIT_PIPE);
}

static int pipe_fd_read(struct syscall_ctx *c, struct open_file *f,
                        uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    struct pipe *p = at(f->pipe.idx);
    if (!p) { regs[14] = 0; return 0; }
    // Filled from the pipe, then handed out: it cannot write the user
    // pointer itself once SMAP is on (vmm.h). Heap rather than stack: see
    // fd_fd_bounce_alloc(). Before the lock -- the heap's is a spinlock.
    char *kbuf = fd_bounce_alloc(&len);
    if (!kbuf) {
        if (c->lockless) return SYSCALL_DECLINE;
        regs[14] = (uint64_t)(int64_t)-ENOMEM;
        return 0;
    }
    int arm = !f->nonblock;
    spin_lock(&p->lock);
    uint32_t avail = p->count < (int)len ? (uint32_t)p->count : (uint32_t)len;
    // LOCK-FREE, THE DESTINATION IS CHECKED BEFORE A BYTE LEAVES THE PIPE,
    // and copied before the lock goes: no lock between the two
    // (vmm_copy_to_user_nofault()'s window).
    if (c->lockless && !vmm_user_range_nofault(pml4, buf_ptr, avail, 1)) {
        spin_unlock(&p->lock);
        fd_bounce_free(kbuf);
        return SYSCALL_DECLINE;
    }
    int64_t n = read_locked(p, kbuf, (uint32_t)len);
    int copied = n > 0 && c->lockless ? vmm_copy_to_user_nofault(pml4, buf_ptr, kbuf, (uint64_t)n) : 1;
    if (n < 0 && arm) scheduler_wait_arm(p);
    spin_unlock(&p->lock);
    // Draining makes room, so a WRITER parked on a full pipe can go on --
    // both ends share this pipe's channel, and every waiter re-tests.
    if (n > 0) scheduler_wake(p, SYS_RETRY);
    if (n > 0 && !c->lockless) copied = vmm_copy_to_user(pml4, buf_ptr, kbuf, (uint64_t)n);
    fd_bounce_free(kbuf);

    if (!copied) {
        klog_write(KLOG_ERR "syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    if (n >= 0) { regs[14] = (uint64_t)n; return 0; }   // bytes, or 0 for EOF
    if (!arm) { regs[14] = (uint64_t)(int64_t)-EAGAIN; return 0; }   // O_NONBLOCK says so, not parks
    int r = park(c, p);
    // Nowhere to park (kernel code or the legacy path): EOF rather than
    // spinning -- a caller that cannot block must not be told "try again
    // forever".
    if (r == 0) { scheduler_wait_disarm(); regs[14] = 0; }
    return r;
}

// A write to a pipe WRITE end -- which is simply what fd 1 refers to in a
// process whose parent redirected it: the redirection lives in the
// descriptor table, where dup2 can also put it. Returns as
// pipe_fd_read() does -- a full pipe parks its writer rather than taking
// what fits.
static int pipe_fd_write(struct syscall_ctx *c, struct open_file *f,
                         uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    struct pipe *p = at(f->pipe.idx);
    if (!p) {
        // No pipe behind the end is no reader: EPIPE, never a 0 that
        // libsys's sys_write() would loop on (below).
        if (c->lockless) return SYSCALL_DECLINE;
        signal_send(scheduler_current_pid(), SIGPIPE);
        regs[14] = (uint64_t)(int64_t)-EPIPE;
        return 0;
    }
    // CLAMPED TO THE PIPE BUFFER, and load-bearing: pipe_write() is
    // ALL-OR-NOTHING (api/pipe.h), which is only safe while one write can
    // always eventually fit -- a request bigger than the pipe would park
    // forever. It also keeps POSIX's atomic-up-to-PIPE_BUF, and costs the
    // caller nothing: libsys's sys_write() loops on a short count.
    if (len > PIPE_BUF_SIZE) len = PIPE_BUF_SIZE;

    char *kbuf = fd_bounce_alloc(&len);
    if (!kbuf) {
        if (c->lockless) return SYSCALL_DECLINE;
        regs[14] = (uint64_t)(int64_t)-ENOMEM;
        return 0;
    }
    int ok = c->lockless ? vmm_copy_from_user_nofault(pml4, kbuf, buf_ptr, len)
                         : vmm_copy_from_user(pml4, kbuf, buf_ptr, len);
    if (!ok) {
        fd_bounce_free(kbuf);
        if (c->lockless) return SYSCALL_DECLINE;
        klog_write(KLOG_ERR "syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    int arm = !f->nonblock;
    spin_lock(&p->lock);
    // No reader: SIGPIPE, which is the locked path's to send.
    if (c->lockless && p->readers <= 0) {
        spin_unlock(&p->lock);
        fd_bounce_free(kbuf);
        return SYSCALL_DECLINE;
    }
    int64_t n = write_locked(p, kbuf, (uint32_t)len);
    if (n < 0 && arm) scheduler_wait_arm(p);
    spin_unlock(&p->lock);
    fd_bounce_free(kbuf);
    if (n > 0) scheduler_wake(p, SYS_RETRY);

    // **A DEAD PIPE IS A FAILURE, NOT A SHORT WRITE**, and reporting it as
    // one WEDGED THE MACHINE: libsys's sys_write() reads a zero as "wrote
    // nothing, try the rest" and loops, so `echo abc | no_such_command`
    // spun at 100% CPU forever. POSIX: raise SIGPIPE and fail EPIPE; a
    // process that has not caught or ignored it dies here. `len > 0` keeps
    // an honest zero-length write what it is.
    if (n == 0 && len > 0) {
        signal_send(scheduler_current_pid(), SIGPIPE);
        regs[14] = (uint64_t)(int64_t)-EPIPE;
        return 0;
    }
    if (n >= 0) { regs[14] = (uint64_t)n; return 0; }   // all of it, or an honest zero
    if (!arm) { regs[14] = (uint64_t)(int64_t)-EAGAIN; return 0; }   // full: nothing taken
    // Parked: libsys's sys_write() loops on SYS_RETRY and re-sends the
    // WHOLE buffer -- correct only because nothing was taken.
    int r = park(c, p);
    if (r == 0) { scheduler_wait_disarm(); regs[14] = 0; }   // nowhere to park, as a read's
    return r;
}

static void pipe_fd_open(struct open_file *f, int idx) { f->pipe.idx = idx; }
// Closing the LAST reader or writer is what turns the other end's wait
// into EOF or EPIPE -- pipe_close_reader()/_writer() wake it.
static void pipe_fd_release_r(struct open_file *f) { pipe_close_reader(f->pipe.idx); }
static void pipe_fd_release_w(struct open_file *f) { pipe_close_writer(f->pipe.idx); }

const struct fd_ops pipe_read_fd_ops = {
    .name = "pipe", .read = pipe_fd_read,
    .open = pipe_fd_open, .release = pipe_fd_release_r,
    .flags = FD_OPS_MPSAFE,
};
const struct fd_ops pipe_write_fd_ops = {
    .name = "pipe", .write = pipe_fd_write,
    .open = pipe_fd_open, .release = pipe_fd_release_w,
    .flags = FD_OPS_MPSAFE,
};

int sys_pipe(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    if (!vmm_validate_user_range(pml4, c->a0, sizeof(int) * 2)) {
        klog_write(KLOG_ERR "syscall: pipe() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        int idx = pipe_create();
        int rd = idx >= 0 ? fd_desc_alloc(&pipe_read_fd_ops, idx) : -1;
        int wd = rd  >= 0 ? fd_desc_alloc(&pipe_write_fd_ops, idx) : -1;
        int rfd = wd >= 0 ? fd_install(pml4, rd) : -1;
        int wfd = rfd >= 0 ? fd_install(pml4, wd) : -1;
        if (idx < 0 || rd < 0 || wd < 0 || rfd < 0 || wfd < 0) {
            // Unwind rather than leak. A half-made pipe with only
            // one end is worse than none: the caller cannot tell,
            // and would block forever on the end that is missing.
            if (rfd >= 0) fd_close(pml4, rfd); else if (rd >= 0) fd_desc_unref(rd);
            if (wd >= 0 && wfd < 0) fd_desc_unref(wd);
            if (idx >= 0) { pipe_close_reader(idx); pipe_close_writer(idx); }
            // ENFILE when the system-wide pipe table is out, EMFILE
            // when it was this process's descriptors -- the caller can
            // do something about the second and nothing about the first.
            klog_write(KLOG_ERR "syscall: pipe() failed -- no free pipe or fd\n");
            c->regs[14] = (uint64_t)(int64_t)(idx < 0 ? -ENFILE : -EMFILE);
        } else {
            int out[2] = { rfd, wfd };
            // Validated above BEFORE the fds existed; a copy that still
            // fails is reported rather than pretending two numbers landed.
            c->regs[14] = vmm_copy_to_user(pml4, c->a0, out, sizeof out) ? 0 : (uint64_t)(int64_t)-EFAULT;
        }
    }
    return 0;
}
