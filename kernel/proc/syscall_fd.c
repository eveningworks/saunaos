// The file-descriptor namespace, and the syscalls every kind of
// descriptor shares -- read, write, lseek, fstat, close, dup.
//
// One table for files, sockets and pipe ends, as in real Unix rather
// than a parallel table per kind: SYS_CLOSE and fd_release_all() only
// ever look at `used` and `owner_pml4`, so a new kind costs them
// nothing. Each entry records which process owns it BY CR3, which is
// what stops one process reading, writing or closing another's fds --
// the same isolation trick the legacy heap and window state use.
//
// SYS_WRITE and SYS_READ live here rather than with the filesystem
// because all they do is ROUTE: through the description's ops (struct
// fd_ops, syscalls.h), which the subsystem owning each kind of stream
// defines -- file_fd_ops in kernel/fs/, a pipe's in pipe.c, a socket's
// in kernel/net/, a terminal's in kernel/tty/.
#include "syscalls.h"
#include "syscall_abi.h"
#include "klog.h"
#include "heap.h"  // bounce buffers come from here, not the stack
#include "vmm.h"
#include "tty.h" // terminals -- fd 0 is one, and so is a pty end
#include "net.h"     // net_sock_peer() -- fd_peer_ip()
#include "string.h"
#include "klimit.h"

// A BOUNCE BUFFER FOR ONE TRANSFER, sized to what this call actually
// needs rather than to the maximum.
//
// Every read and write op used to ask for SYS_WRITE_MAX flat. That was
// harmless while the cap was 1 KiB and is not now that it is 64: a
// one-byte console write would reserve 64 KiB, and a 64 KiB allocation
// wants sixteen CONTIGUOUS frames, which a fragmented pool can refuse.
//
// So it asks for `*len` and HALVES down to a 1 KiB floor on failure,
// reporting back how much it got. The caller then moves less -- a SHORT
// transfer, which every caller here already handles, because libsys
// loops a write to completion and a short read is Unix's rule
// everywhere. A fragmented heap therefore costs throughput rather than
// turning a write into -ENOMEM.
void *fd_bounce_alloc(uint64_t *len) {
    uint64_t want = *len ? *len : 1;
    for (;;) {
        void *p = kmalloc((size_t)want);
        if (p) { *len = want; return p; }
        if (want <= 1024) return NULL;
        want /= 2;
    }
}

// The table itself. Its TYPES are in syscalls.h, because SYS_OPEN
// (kernel/fs/fs_syscalls.c) and SYS_SPAWN (proc_syscalls.c) index it
// too; the storage is here, with the code that owns it.

// DESCRIPTIONS ARE ALLOCATED ONE AT A TIME, and this is a table of
// pointers to them -- so a description never moves and a caller may
// hold `struct open_file *` across an unrelated allocation. The table
// of POINTERS grows; the things it points at do not. (fd_get() hands
// out such a pointer, and sys_accept() holds one while allocating the
// accepted socket's description, so this is load-bearing rather than
// tidiness.)
static struct open_file **g_desc;
static int g_desc_cap;
static int g_desc_hint;   // no free description below this; a hint, rechecked

// THE CEILINGS, from RAM (klimit.h). A description costs its struct --
// FS_PATH_MAX of name included -- plus its pointer; a sixteenth of RAM
// may go to them. A descriptor table may hold an eighth of that: one
// process can take a share of the system's open files, not all of them.
struct klimit fd_desc_limit = KLIMIT("fd: open-file descriptions", 16,
                                     sizeof(struct open_file) + sizeof(void *), 1024, 1u << 20);
struct klimit fd_proc_limit = KLIMIT("fd: descriptors per process", 16 * 8,
                                     sizeof(struct open_file) + sizeof(void *), 256, 1u << 20);
int fd_desc_ceiling(void) { return (int)klimit_get(&fd_desc_limit); }
int fd_proc_ceiling(void) { return (int)klimit_get(&fd_proc_limit); }

struct open_file *fd_desc_at(int i) {
    int cap = __atomic_load_n(&g_desc_cap, __ATOMIC_ACQUIRE);
    if (i < 0 || i >= cap) return NULL;
    return __atomic_load_n(&g_desc, __ATOMIC_ACQUIRE)[i];
}
int fd_desc_count(void) { return __atomic_load_n(&g_desc_cap, __ATOMIC_ACQUIRE); }

// Grows the pointer table by doubling. The descriptions themselves are
// untouched, so nothing a caller holds is invalidated. THE OLD TABLE IS
// NEVER FREED -- kslots.h's rule: a reader preempted between loading
// the array and indexing it still holds it, and every table ever
// retired sums to less than the live one. Table first, capacity last.
static int desc_table_grow(void) {
    int ceiling = fd_desc_ceiling();
    int cap = g_desc_cap ? g_desc_cap * 2 : 32;
    if (cap > ceiling) cap = ceiling;
    if (cap <= g_desc_cap) return 0;
    struct open_file **n = kzalloc((uint32_t)cap * sizeof *n);
    if (!n) return 0;
    if (g_desc) k_memcpy(n, g_desc, (uint32_t)g_desc_cap * sizeof *n);
    __atomic_store_n(&g_desc, n, __ATOMIC_RELEASE);
    __atomic_store_n(&g_desc_cap, cap, __ATOMIC_RELEASE);
    return 1;
}

// One descriptor table per ADDRESS SPACE, allocated when that space
// first wants an fd, and GROWN BY DOUBLING from FD_TABLE_FIRST up to
// fd_proc_ceiling() -- Linux's expand_files(). `d` and `cloexec` are one
// allocation; a grown pair is published before `cap`, and the old pair
// goes on `retired` rather than to kfree() (a thread of the same
// process may be reading it), freed when the space is released.
struct fd_retired { struct fd_retired *next; void *mem; };
struct fd_space {
    uint64_t pml4;      // 0 = free slot
    int      cap;       // descriptors behind `d`
    int      next_fd;   // no free descriptor below this (Linux's next_fd)
    int     *d;         // d[fd] = index into a description, or -1
    // **PER DESCRIPTOR, NOT PER DESCRIPTION.** open_file::nonblock is on
    // the description and is shared by every dup; close-on-exec is the
    // opposite and has to be, because a dup made precisely to survive an
    // exec must not inherit the flag from the handle it was copied from.
    // Same split as Linux's. AND IT BELONGS TO THE NUMBER WHILE IT IS
    // OPEN: closing, reusing or dup'ing onto a number clears it, or the
    // next file given that number inherits a stranger's mark.
    uint8_t *cloexec;
    struct fd_retired *retired;
};
// Grown by doubling, as g_desc is, up to the process limit plus the
// kernel's own few: one table per live address space.
static struct fd_space **g_spaces;
static int g_spaces_cap;

static int spaces_grow(void) {
    int ceiling = scheduler_max_procs() + 4;
    int cap = g_spaces_cap ? g_spaces_cap * 2 : 32;
    if (cap > ceiling) cap = ceiling;
    if (cap <= g_spaces_cap) return 0;
    struct fd_space **n = kzalloc((uint32_t)cap * sizeof *n);
    if (!n) return 0;
    if (g_spaces) {
        k_memcpy(n, g_spaces, (uint32_t)g_spaces_cap * sizeof *n);
        kfree(g_spaces);
    }
    g_spaces = n;
    g_spaces_cap = cap;
    return 1;
}

// Makes room for descriptor `fd` in `sp`: 1, or 0 at the ceiling (logged
// once) or out of memory. EVERY WRITER OF A TABLE HOLDS THE PREEMPTION
// GUARD, so two threads of one process cannot both grow it or both take
// one free number (kmalloc is allowed under it -- it is the heap's lock).
static int space_reserve(struct fd_space *sp, int fd) {
    if (fd < sp->cap) return 1;
    int ceiling = fd_proc_ceiling();
    if (fd >= ceiling) { klimit_hit(&fd_proc_limit); return 0; }
    int cap = sp->cap ? sp->cap : FD_TABLE_FIRST;
    while (cap <= fd) cap *= 2;
    if (cap > ceiling) cap = ceiling;
    struct fd_retired *old = sp->d ? kmalloc(sizeof *old) : NULL;
    int *d = kmalloc((uint32_t)cap * (sizeof(int) + 1));
    if (!d || (sp->d && !old)) { kfree(d); kfree(old); return 0; }
    uint8_t *cx = (uint8_t *)(d + cap);
    for (int i = 0; i < cap; i++) d[i] = -1;
    k_memset(cx, 0, (uint32_t)cap);
    if (sp->d) {
        k_memcpy(d, sp->d, (uint32_t)sp->cap * sizeof(int));
        k_memcpy(cx, sp->cloexec, (uint32_t)sp->cap);
        old->mem = sp->d;
        old->next = sp->retired;
        sp->retired = old;
    }
    __atomic_store_n(&sp->d, d, __ATOMIC_RELEASE);
    __atomic_store_n(&sp->cloexec, cx, __ATOMIC_RELEASE);
    __atomic_store_n(&sp->cap, cap, __ATOMIC_RELEASE);
    return 1;
}

// The description index at `fd`, or -1 -- safe against a concurrent grow.
static int slot_di(const struct fd_space *sp, int fd) {
    int cap = __atomic_load_n(&sp->cap, __ATOMIC_ACQUIRE);
    if (fd < 0 || fd >= cap) return -1;
    return __atomic_load_n(&sp->d, __ATOMIC_ACQUIRE)[fd];
}

// Points `fd` (in range) at `di`. A number's close-on-exec mark goes
// with whatever it named before.
static void slot_set(struct fd_space *sp, int fd, int di) {
    sp->d[fd] = di;
    sp->cloexec[fd] = 0;
}

static void slot_clear(struct fd_space *sp, int fd) {
    sp->d[fd] = -1;
    sp->cloexec[fd] = 0;
    if (fd < sp->next_fd) sp->next_fd = fd;
}

// The lowest free descriptor at or above `min`, with room reserved for
// it; -1 at the ceiling or out of memory. Preemption guard held.
static int lowest_free(struct fd_space *sp, int min) {
    int fd = min > sp->next_fd ? min : sp->next_fd;
    while (fd < sp->cap && sp->d[fd] >= 0) fd++;
    if (!space_reserve(sp, fd)) return -1;
    if (min <= sp->next_fd) sp->next_fd = fd + 1;
    return fd;
}

// Frees a released space's grown arrays, keeping the struct for reuse.
static void space_drop_arrays(struct fd_space *sp) {
    while (sp->retired) {
        struct fd_retired *r = sp->retired;
        sp->retired = r->next;
        kfree(r->mem);
        kfree(r);
    }
    kfree(sp->d);
    sp->d = NULL;
    sp->cloexec = NULL;
    sp->cap = 0;
    sp->next_fd = 0;
}

int fd_desc_alloc(const struct fd_ops *ops, int aux_idx) {
    // From the hint, then once more from 0 before growing: the hint is
    // only a hint, and growing past a free slot would reach the ceiling
    // with descriptions to spare.
    int from = g_desc_hint;
    for (int i = from; ; i++) {
        if (i >= g_desc_cap) {
            if (from) { from = 0; i = -1; continue; }
            if (!desc_table_grow()) {
                if (g_desc_cap >= fd_desc_ceiling()) klimit_hit(&fd_desc_limit);
                return -1;
            }
        }
        if (!g_desc[i]) {
            g_desc[i] = kzalloc(sizeof **g_desc);
            if (!g_desc[i]) return -1;
        }
        if (g_desc[i]->refs) continue;

        // A FRESH DESCRIPTION CARRIES NOTHING FROM THE LAST ONE. These
        // slots are a pool, and every field left set is inherited by
        // whatever reuses the slot -- which for `nonblock` meant a
        // program that had set it on a socket handed the flag to the
        // next program's socket, so a blocking receive returned 0 at
        // once and every reply looked lost. It reproduced as `host`
        // failing only AFTER something unrelated had run, which is the
        // worst shape a bug can have.
        k_memset(g_desc[i], 0, sizeof *g_desc[i]);

        g_desc[i]->refs = 1;
        g_desc[i]->ops = ops;
        g_desc_hint = i + 1;
        if (ops->open) ops->open(g_desc[i], aux_idx);
        return i;
    }
}

// The one place that decides a stream is really gone. Everything that
// closes an fd -- SYS_CLOSE, dup2 over an open descriptor, a process
// dying -- lands here, so a pipe end cannot be closed twice or leaked
// depending on which path got there.
void fd_desc_unref(int di) {
    struct open_file *f = fd_desc_at(di);
    if (!f) return;
    if (f->refs <= 0) return;
    if (--f->refs > 0) return; // somebody else still names it

    // The kind's own teardown: a pipe end closed, a socket's port given
    // back, a pty end's far side told. Each owner's comment says why.
    if (f->ops->release) f->ops->release(f);
    f->ops = NULL;
    if (di < g_desc_hint) g_desc_hint = di;
}

static struct tty *g_kernel_tty;
void fd_set_kernel_tty(struct tty *t) { g_kernel_tty = t; }
struct tty *fd_kernel_tty(void) { return g_kernel_tty; }

static struct fd_space *space_find(uint64_t pml4) {
    for (int i = 0; i < g_spaces_cap; i++)
        if (g_spaces[i] && g_spaces[i]->pml4 == pml4) return g_spaces[i];
    return NULL;
}

int fd_space_open(uint64_t pml4) {
    if (!pml4) return -1;
    if (space_find(pml4)) return 0; // idempotent

    // Allocated on demand: only a live address space pays for its
    // table, and the slot array is bounded by the process table rather
    // than by a number somebody picked.
    struct fd_space *sp = NULL;
    int slot = -1;
    for (int i = 0; i < g_spaces_cap; i++) {
        if (g_spaces[i] && !g_spaces[i]->pml4) { sp = g_spaces[i]; slot = i; break; }
        if (!g_spaces[i] && slot < 0) slot = i;
    }
    if (!sp && slot < 0) {
        int at = g_spaces_cap;
        if (spaces_grow()) slot = at;
    }
    if (!sp) {
        if (slot < 0) {
            klog_write("fd: no free descriptor table -- too many address spaces\n");
            return -1;
        }
        sp = kzalloc(sizeof *sp);
        if (!sp) {
            klog_write("fd: out of memory for a descriptor table\n");
            return -1;
        }
        g_spaces[slot] = sp;
    }

    // A reused table's arrays went with its dead process (fd_release_all()).
    if (!space_reserve(sp, FD_STDERR)) {
        klog_write("fd: out of memory for a descriptor table\n");
        return -1;
    }
    sp->pml4 = pml4;

    // The three a process starts with: ALL THREE ON THE CONSOLE, one
    // description named three times -- what login(1) hands a shell on a
    // tty. A table built here belongs to a program the kernel started
    // in the foreground (the shell's `run`), so its errors belong on the
    // terminal it was run from. A DETACHED start is given the kernel log
    // as stderr by its spawner instead (scheduler_spawn_env()), and that
    // is where a service's stderr comes from: init's (docs/decisions.md,
    // "stderr is the terminal's, and the kernel log only without one").
    int con = g_kernel_tty ? fd_desc_alloc(&tty_fd_ops, tty_index(g_kernel_tty))
                           : fd_desc_alloc(&console_fd_ops, -1);
    if (con < 0) {
        sp->pml4 = 0;
        return -1;
    }
    fd_desc_at(con)->refs += 2; // one reference per descriptor
    slot_set(sp, FD_STDIN, con);
    slot_set(sp, FD_STDOUT, con);
    slot_set(sp, FD_STDERR, con);
    sp->next_fd = FD_STDERR + 1;
    return 0;
}

// Every lookup goes through here, so a process that has not touched an
// fd yet still gets its standard three rather than an empty table --
// which matters because nothing calls fd_space_open() at spawn for the
// legacy loader.
static struct fd_space *space_get(uint64_t pml4) {
    struct fd_space *sp = space_find(pml4);
    if (sp) return sp;
    if (fd_space_open(pml4) < 0) return NULL;
    return space_find(pml4);
}

int fd_install(uint64_t pml4, int di) {
    struct fd_space *sp = space_get(pml4);
    if (!sp || di < 0) return -1;
    scheduler_preempt_disable();
    int fd = lowest_free(sp, 0);
    if (fd >= 0) slot_set(sp, fd, di);
    scheduler_preempt_enable();
    return fd;
}

int fd_desc_index(uint64_t pml4, int fd) {
    struct fd_space *sp = space_get(pml4);
    if (!sp) return -1;
    int di = slot_di(sp, fd);
    if (di < 0 || !fd_desc_at(di)->refs) return -1;
    return di;
}

struct open_file *fd_get(uint64_t pml4, int fd) {
    int di = fd_desc_index(pml4, fd);
    return fd_desc_at(di);
}

// THE PEER THIS PROCESS IS TALKING TO, or 0 if it holds no connected
// socket at all. How the kernel knows a session arrived over the network
// without being told: telnetd is handed its connection by inetd and then
// makes a session for the shell it spawns, so asking the PARENT answers
// it (kernel/include/kernel/remote_log.h).
//
// **EVERY DESCRIPTOR, NOT fd 0.** The obvious version asked fd 0 and
// found a PTY: telnetd dup2s the pty onto 0/1/2 around the spawn so the
// child inherits them, and its socket is on a descriptor it kept. The
// question worth asking is not "what is this process reading" but "is
// it serving a connection".
uint32_t fd_peer_ip(uint64_t pml4) {
    struct fd_space *sp = space_find(pml4);
    int cap = sp ? __atomic_load_n(&sp->cap, __ATOMIC_ACQUIRE) : 0;
    for (int fd = 0; fd < cap; fd++) {
        struct open_file *f = fd_get(pml4, fd);
        if (!f || f->ops != &socket_fd_ops) continue;
        uint32_t ip = 0;
        uint16_t port = 0;
        net_sock_peer(f->socket.idx, &ip, &port);
        if (ip) return ip;
    }
    return 0;
}

int fd_close(uint64_t pml4, int fd) {
    struct fd_space *sp = space_get(pml4);
    if (!sp) return -1;
    scheduler_preempt_disable();
    int di = slot_di(sp, fd);
    if (di >= 0) slot_clear(sp, fd);
    scheduler_preempt_enable();
    if (di < 0) return -1;
    fd_desc_unref(di);   // outside the guard: a release may sleep
    return 0;
}

// dup to the lowest free descriptor AT OR ABOVE `min`. What fcntl's
// F_DUPFD asks for and what neither fd_dup() (lowest free, no floor)
// nor fd_dup2() (an exact number) can express -- see
// abi/syscall_abi.h's SYS_DUPFD.
//
// **THE COPY DOES NOT INHERIT close-on-exec**, which is the whole point
// of the call in a shell: the copy exists precisely to survive the exec
// the original must not. POSIX says F_DUPFD clears FD_CLOEXEC on the
// new descriptor, and this is why.
int fd_dup_from(uint64_t pml4, int oldfd, int min) {
    struct fd_space *sp = space_get(pml4);
    if (!sp) return -EBADF;
    int di = fd_desc_index(pml4, oldfd);
    if (di < 0) return -EBADF;
    // A floor past the ceiling is a bad argument, not a table that
    // filled: refused without the ceiling's log line.
    if (min >= fd_proc_ceiling()) return -EMFILE;
    scheduler_preempt_disable();
    int fd = lowest_free(sp, min);
    if (fd >= 0) {
        fd_desc_at(di)->refs++;
        slot_set(sp, fd, di);
    }
    scheduler_preempt_enable();
    return fd >= 0 ? fd : -EMFILE;
}

// Points descriptor `fd` at `di`, taking a reference; whatever it named
// before is unref'd (outside the guard -- a release may sleep). -1 when
// `fd` is past this process's ceiling (a bad number, not logged: POSIX's
// EBADF for dup2) or the table cannot grow.
static int put_at(struct fd_space *sp, int fd, int di) {
    if (fd < 0 || fd >= fd_proc_ceiling()) return -1;
    scheduler_preempt_disable();
    if (!space_reserve(sp, fd)) { scheduler_preempt_enable(); return -1; }
    int was = sp->d[fd];
    if (was == di) { scheduler_preempt_enable(); return fd; }   // do not unref it
    fd_desc_at(di)->refs++;
    slot_set(sp, fd, di);   // POSIX: the copy survives an exec
    scheduler_preempt_enable();
    if (was >= 0) fd_desc_unref(was);
    return fd;
}

int fd_dup2(uint64_t pml4, int oldfd, int newfd) {
    struct fd_space *sp = space_get(pml4);
    if (!sp) return -1;
    int di = fd_desc_index(pml4, oldfd);
    if (di < 0) return -1;
    // POSIX: dup2(fd, fd) is a no-op and specifically does NOT close.
    // Getting that wrong destroys the stream it was asked to preserve.
    if (oldfd == newfd) return newfd;
    return put_at(sp, newfd, di);
}

int fd_set_desc(uint64_t pml4, int fd, int di) {
    struct fd_space *sp = space_get(pml4);
    if (!sp) return -1;
    { struct open_file *g = fd_desc_at(di); if (!g || !g->refs) return -1; }
    return put_at(sp, fd, di);
}

void fd_inherit(uint64_t child, uint64_t parent) {
    if (!child) return;
    struct fd_space *ps = parent ? space_find(parent) : NULL;
    if (fd_space_open(child) < 0) return;
    if (!ps) return; // kernel-spawned: the standard three are right

    struct fd_space *cs = space_find(child);
    if (!cs) return;

    // ONLY THE STANDARD THREE, and this is the whole design rather than
    // a simplification.
    //
    // Unix hands a child everything not marked close-on-exec, and gets
    // away with it because the shell runs code IN the child (between
    // fork and exec) to close the pipe ends it must not keep. There is
    // no fork here and no CLOEXEC, so "inherit the whole table" means
    // every spawn silently hands the child every pipe end the parent
    // happens to hold -- and a pipeline can then NEVER see EOF, because
    // the reading stage is itself a writer of the pipe it is reading.
    // Observed exactly that way: two processes blocked forever, one
    // waiting for data and holding the write end that would have ended
    // it.
    //
    // 0/1/2 is also precisely what posix_spawn() and Windows'
    // STARTUPINFO pass by default, for the same reason: they are the
    // streams a child is *meant* to be given, and everything else is
    // the parent's private business.
    // The child's table is FD_TABLE_FIRST from fd_space_open(), and
    // nothing else runs in it yet.
    for (int i = 0; i <= FD_STDERR; i++) {
        if (cs->d[i] >= 0) { fd_desc_unref(cs->d[i]); slot_clear(cs, i); }
        int di = slot_di(ps, i);
        if (di < 0 || !fd_desc_at(di)->refs) continue;
        fd_desc_at(di)->refs++;
        slot_set(cs, i, di);
    }
}

void fd_clone(uint64_t child, uint64_t parent) {
    if (!child) return;
    struct fd_space *ps = parent ? space_find(parent) : NULL;
    if (fd_space_open(child) < 0) return;
    struct fd_space *cs = space_find(child);
    if (!cs || !ps) return;
    for (int i = 0; i < cs->cap; i++)
        if (cs->d[i] >= 0) { fd_desc_unref(cs->d[i]); slot_clear(cs, i); }
    // The parent's whole table, at its size: the child is not running
    // yet, and the parent is the caller, so neither table moves under us.
    if (!space_reserve(cs, ps->cap - 1)) return;
    for (int i = 0; i < ps->cap; i++) {
        int di = ps->d[i];
        if (di < 0 || !fd_desc_at(di)->refs) continue;
        fd_desc_at(di)->refs++;
        slot_set(cs, i, di);
        cs->cloexec[i] = ps->cloexec[i];   // fork copies the marks; the exec reads them
    }
    cs->next_fd = 0;   // a hint only: the lowest free is searched from here
}

// Query or change a descriptor's close-on-exec flag. `op` is -1 to
// query, 0 to clear, 1 to set. Returns the flag as it was BEFORE the
// call, or -EBADF.
int fd_cloexec(uint64_t pml4, int fd, int op) {
    struct fd_space *sp = space_find(pml4);
    if (!sp || slot_di(sp, fd) < 0) return -EBADF;
    int was = sp->cloexec[fd] ? 1 : 0;
    if (op == 0 || op == 1) sp->cloexec[fd] = (uint8_t)op;
    return was;
}

// Close every descriptor marked close-on-exec. Called from fd_rekey(),
// which is the one place an exec carries descriptors into a new image.
void fd_close_on_exec(uint64_t pml4) {
    struct fd_space *sp = space_find(pml4);
    if (!sp) return;
    for (int fd = 0; fd < sp->cap; fd++) {
        if (!sp->cloexec[fd] || sp->d[fd] < 0) continue;
        fd_close(pml4, fd);
    }
}

void fd_rekey(uint64_t old_pml4, uint64_t new_pml4) {
    struct fd_space *sp = space_find(old_pml4);
    if (!sp || !new_pml4) return;
    // The console claim follows the process, not the tables.
    tty_fd_rekey(old_pml4, new_pml4);
    sp->pml4 = new_pml4;
    // **THIS IS WHERE close-on-exec BITES**, and it is the only place an
    // exec carries descriptors into the new image -- so a flag honoured
    // anywhere else would be honoured at the wrong time. After the
    // rekey, so the close acts on the space the new image will see.
    fd_close_on_exec(new_pml4);
}

// The TERMINAL an fd names, or NULL. ONE place knows that fd 0 on the
// console means tty0, so the four terminal syscalls do not each have to
// -- and so a future virtual terminal changes one function.
struct tty *fd_tty(uint64_t pml4, int fd) {
    struct open_file *f = fd_get(pml4, fd);
    if (!f || !f->ops->tty) return NULL;
    return f->ops->tty(f);
}

int sys_write(struct syscall_ctx *c) {
    // ABI: RDI = fd (was the buffer pointer before SYS_OPEN/SYS_READ
    // existed -- see the "ABI NOTE" on SYS_WRITE in syscall_abi.h),
    // RSI = buffer pointer, RDX = length.
    int fd = (int)c->a0;
    uint64_t buf_ptr = c->a1;
    uint64_t len = c->a2;
    if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;

    // The buffer is validated by each op's vmm_copy_from_user(), never
    // read through directly: a process could otherwise hand the kernel a
    // kernel-only address (still *present*, since PML4 entry 0 is shared
    // with every process -- see vmm.h) and have it read on its behalf.
    uint64_t pml4 = c->pml4;

    // ROUTED THROUGH THE DESCRIPTION'S OPS, not on the fd number. That is
    // the whole point of the two-level table: after `dup2(pipe_w, 1)`
    // fd 1 IS a pipe, and nothing here needs to know that a
    // redirection happened. The `if (fd == 1)` this replaced could not
    // express that, which is why redirection used to be a field on the
    // process instead.
    struct open_file *f = fd_get(pml4, fd);
    if (!f) {
        klog_write(KLOG_ERR "syscall: write() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }

    if (!f->ops->write) {
        // A pipe's READ end, or a shm object, which is only ever mapped.
        // Rejected like any other bad fd rather than silently treated as
        // something it is not.
        klog_write(KLOG_ERR "syscall: write() rejected -- wrong kind of fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    // 1 when the op PARKED the caller -- its result is then the wake's.
    return f->ops->write(c, f, buf_ptr, len);
}

int sys_read(struct syscall_ctx *c) {
    int fd = (int)c->a0;
    uint64_t buf_ptr = c->a1;
    uint64_t len = c->a2;
    if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;

    uint64_t pml4 = c->pml4;

    // Routed through the description's ops, exactly as SYS_WRITE is, so
    // `dup2(pipe_r, 0)` makes fd 0 a pipe with nothing here changed.
    struct open_file *f = fd_get(pml4, fd);
    if (!f) {
        klog_write(KLOG_ERR "syscall: read() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    if (!vmm_validate_user_range(pml4, buf_ptr, len)) {
        klog_write(KLOG_ERR "syscall: read() rejected -- invalid buffer pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    if (!f->ops->read) {
        // A pipe WRITE end, a log, or a shm object -- none has a reader.
        klog_write(KLOG_ERR "syscall: read() rejected -- wrong kind of fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    return f->ops->read(c, f, buf_ptr, len);
}

// --- position and identity -------------------------------------------
//
// The two calls a buffered stdio cannot be written without: where am I
// in this stream, and what KIND of stream is it. Both live here rather
// than with the filesystem because both answer questions about a
// DESCRIPTION, not about a path -- the same reason SYS_READ and
// SYS_WRITE are in this file.

int sys_lseek(struct syscall_ctx *c) {
    struct open_file *f = fd_get(c->pml4, (int)c->a0);
    if (!f) {
        klog_write(KLOG_ERR "syscall: lseek() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    // ESPIPE, not EINVAL, and the name is a historical accident worth
    // keeping: POSIX spells "this stream has no position" this way for
    // a pipe, a socket and a terminal alike, and a libc's fseek() turns
    // exactly this code into the errno a program expects.
    if (!f->ops->seek) {
        c->regs[14] = (uint64_t)(int64_t)-ESPIPE;
        return 0;
    }
    // The new position, or a negative errno -- file_fd_seek() says which.
    c->regs[14] = (uint64_t)f->ops->seek(f, (int64_t)c->a1, c->a2);
    return 0;
}

int sys_fstat(struct syscall_ctx *c) {
    struct open_file *f = fd_get(c->pml4, (int)c->a0);
    if (!f) {
        klog_write(KLOG_ERR "syscall: fstat() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    struct sys_stat out;
    k_memset(&out, 0, sizeof out);
    // Reported for EVERY kind, before the switch: non-blocking is a
    // property of the description whatever it names, and fcntl(F_GETFL)
    // has no other way to read it back.
    if (f->nonblock) out.flags |= SYS_STAT_NONBLOCK;
    if (f->ops->seek) out.flags |= SYS_STAT_SEEKABLE;
    // What isatty() will read. A pty end is as much a terminal as the
    // console is -- that is the entire claim of the tty layer, and a flag
    // that said otherwise would make a program behave differently in a
    // window than on the console.
    if (f->ops->tty) out.flags |= SYS_STAT_TTY;
    // A pipe end or a socket has no stat op: no size, no position, no
    // timestamps. Zero is the honest answer rather than a lossy one.
    if (f->ops->stat) f->ops->stat(f, &out);
    if (!vmm_copy_to_user(c->pml4, c->a1, &out, sizeof out)) {
        klog_write(KLOG_ERR "syscall: fstat() rejected -- invalid output pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    c->regs[14] = 0;
    return 0;
}

int sys_close(struct syscall_ctx *c) {
    // Kind-agnostic, and refcount-aware: fd_close() drops the
    // DESCRIPTOR, and the description behind it is torn down only when
    // the last descriptor naming it goes. That is what makes the
    // shell's save/restore dance safe --
    //
    //     saved = dup(1); dup2(f, 1); spawn(...); dup2(saved, 1); close(saved);
    //
    // -- because closing `saved` must not close the stream it named.
    // Closing the LAST writer of a pipe is still what turns a blocked
    // reader's wait into EOF; that now happens inside fd_desc_unref().
    c->regs[14] = fd_close(c->pml4, (int)c->a0) < 0 ? (uint64_t)(int64_t)-EBADF : 0;
    return 0;
}

int sys_dup(struct syscall_ctx *c) {
    // Lowest free descriptor naming the same description, as POSIX.
    uint64_t pml4 = c->pml4;
    int di = fd_desc_index(pml4, (int)c->a0);
    if (di < 0) { c->regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }
    fd_desc_at(di)->refs++;
    int fd = fd_install(pml4, di);
    if (fd < 0) {
        fd_desc_unref(di); // undo: the table was full
        c->regs[14] = (uint64_t)(int64_t)-EMFILE;
    } else {
        c->regs[14] = (uint64_t)fd;
    }
    return 0;
}

int sys_dup2(struct syscall_ctx *c) {
    // fd_dup2() reports -1 for both "oldfd is not open" and "newfd is out
    // of range", which are the same answer to the caller: EBADF names a
    // descriptor that cannot be used, whichever of the two it was.
    int r = fd_dup2(c->pml4, (int)c->a0, (int)c->a1);
    c->regs[14] = r < 0 ? (uint64_t)(int64_t)-EBADF : (uint64_t)r;
    return 0;
}

// Everything this process left open. Not part of its address space
// (fs.c and pipe.c are separate kernel resources, nothing about them is
// mapped into the process), so vmm_destroy_address_space() would not
// reclaim any of it.
void fd_release_all(uint64_t pml4_phys) {
    // The console claim is not an fd, but it is the same kind of thing: a
    // kernel resource keyed by address space that nothing else would
    // reclaim -- see tty_fd_process_gone().
    tty_fd_process_gone(pml4_phys);
    // Every descriptor this space holds, dropped through the same
    // refcounted path SYS_CLOSE uses. A pipe end must be RELEASED, not
    // just forgotten: a process that dies holding the last write end is
    // exactly how a reader learns there is no more output coming, and
    // dropping the reference silently would leave that reader blocked
    // forever on a dead writer. Sharing the unref path is also what
    // makes an INHERITED fd safe -- the parent's copy survives its
    // child dying.
    struct fd_space *sp = space_find(pml4_phys);
    if (sp) {
        for (int i = 0; i < sp->cap; i++) {
            if (sp->d[i] < 0) continue;
            fd_desc_unref(sp->d[i]);
            sp->d[i] = -1;
        }
        sp->pml4 = 0; // the struct is reusable now; its arrays are not kept
        space_drop_arrays(sp);
    }
}


// SYS_DUPFD -- dup to the lowest free descriptor AT OR ABOVE a floor.
// See abi/syscall_abi.h for why SYS_DUP and SYS_DUP2 cannot express it.
int sys_dupfd(struct syscall_ctx *c) {
    int fd = (int)(int32_t)c->a0;
    int min = (int)(int32_t)c->a1;
    if (min < 0) min = 0;
    c->regs[14] = (uint64_t)(int64_t)fd_dup_from(c->pml4, fd, min);
    return 0;
}

// SYS_FD_CLOEXEC -- query (-1), clear (0) or set (1). Returns the flag
// as it was before the call.
int sys_fd_cloexec(struct syscall_ctx *c) {
    int fd = (int)(int32_t)c->a0;
    int op = (int)(int32_t)c->a1;
    c->regs[14] = (uint64_t)(int64_t)fd_cloexec(c->pml4, fd, op);
    return 0;
}
