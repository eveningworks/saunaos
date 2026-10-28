// Syscall tracing -- the kernel half of `/bin/strace`.
//
// Every ring-3 syscall funnels through one function (syscall_dispatch(),
// syscall.c), so tracing needs no per-syscall instrumentation: hooks in
// that one function cover every call, and a syscall added later is
// traced the moment its row exists (abi/syscall_rows.h).
//
// **THE KERNEL RECORDS; RING 3 DECODES** (docs/trace-design.md) --
// FreeBSD's ktrace/kdump split. A tracer hands SYS_SPAWN a ring it
// created (SPAWN_TRACE_RING, abi/trace_abi.h), and this file writes one
// record per syscall entry and exit into it. Names, argument formatting,
// filtering and counting are /bin/strace's (userland/lib/utrace.h).
//
// A TRACE is one ring and the ADDRESS SPACES writing into it: the spawned
// child, and with SPAWN_TRACE_FOLLOW every process it spawns or forks
// after (`strace -f`). Several traces run at once, each its own tracer's.
// The trace ends -- the ring is let go -- when its last address space
// exits; the header's `live` says how many are left, which is how a
// tracer following children knows it has seen everything.
//
// **THE ARM IS THE ASKER'S, both ways**: only its own spawn can collect
// it (a bare "next process anywhere" flag let somebody else's spawn
// claim it), and only it can clear it -- a spawn can sleep loading its
// ELF, and every other spawn ends in strace_disarm().
//
// The bytes of a path or buffer argument are COPIED AT THE CALL, through
// vmm_validate_user_range() as the real handler would: read later, the
// tracee could have changed them, and a tracee must never get the
// kernel to read something its own handler would refuse.
#include "strace.h"
#include "syscall_abi.h"
#include "errno.h"
#include "syscall_table.h"
#include "vmm.h"
#include "scheduler.h"   // scheduler_current_pid(), _pid_alive(), _sleep_current()
#include "string.h"
#include "shm.h"         // the record ring is the tracer's shm object
#include "futex.h"       // futex_note_ready() -- the tracer's wakeword
#include "clocksource.h" // the monotonic clock sleep deadlines are on
#include "trace_abi.h"
#include "kslots.h"
#include "klimit.h"
#include "klog.h"
#include "kfmt.h"        // klog_printf

// THE TABLES GROW, in kslots: the per-syscall path (strace_active(),
// ring_put()) walks them with no lock, and kslots never frees what a
// walk preempted halfway may still be reading.
struct trace {
    int ring;             // its shm index
    int owner;            // the tracer's pid: it drains, a full ring waits on it
    uint32_t nrec;        // slots, clamped to what the object holds
    int follow;           // SPAWN_TRACE_FOLLOW: children join it
    int spaces;           // address spaces still writing into it
};
struct tspace { uint64_t pml4; int t; };      // pml4 0 = being freed
struct tarm { int pid; int ring; int follow; }; // a spawn that asked for a trace

static struct kslots g_traces = KSLOTS_INIT(sizeof(struct trace));
static struct kslots g_spaces = KSLOTS_INIT(sizeof(struct tspace));
static struct kslots g_arms = KSLOTS_INIT(sizeof(struct tarm));
static int g_nspaces;     // what an untraced syscall pays: one compare

// THE CEILING IS ON RING MEMORY, not on a count of traces: every arm and
// trace pins its tracer's ring (shm_get) until the trace ends, whatever
// the tracer does, so what a runaway costs is rings. A sixty-fourth of
// RAM, in KiB, and never below the eight 64 KiB rings the fixed table
// held. Address spaces need no ceiling of their own: one per process.
struct klimit strace_ring_limit = KLIMIT("strace: KiB of trace rings pinned", 64, 1024,
                                         8 * 64, 1u << 30);
static uint64_t g_pinned_kib;

static struct trace *T(int t) { return kslots_at(&g_traces, t); }

// ---- the tables ---------------------------------------------------------------

static struct trace_ring_hdr *ring_hdr(int idx) {
    return (struct trace_ring_hdr *)(uintptr_t)shm_frame(idx, 0);
}

// The slots the object can really hold, whatever the header claims: the
// tracer wrote `nrec`, and a value past its own pages would have the
// kernel write past them.
static uint32_t ring_capacity(int idx) {
    uint64_t bytes = shm_npages(idx) * 4096;
    if (bytes <= TRACE_HDR_SIZE) return 0;
    uint64_t fit = (bytes - TRACE_HDR_SIZE) / TRACE_REC_SIZE;
    uint32_t want = ring_hdr(idx)->nrec;
    return want < fit ? want : (uint32_t)fit;
}

// Holds `ring` for an arm or a trace; -EBUSY past the ceiling (logged).
static int ring_pin(int ring) {
    uint64_t kib = shm_npages(ring) * 4;
    if (g_pinned_kib + kib > klimit_get(&strace_ring_limit)) {
        klimit_hit(&strace_ring_limit);
        return -EBUSY;
    }
    shm_get(ring);
    g_pinned_kib += kib;
    return 0;
}

static void ring_unpin(int ring) {
    uint64_t kib = shm_npages(ring) * 4;   // before the put can free it
    g_pinned_kib = g_pinned_kib > kib ? g_pinned_kib - kib : 0;
    shm_put(ring);
}

static int space_of(uint64_t pml4) {
    if (!g_nspaces || !pml4) return -1;
    for (int i = 0; i < kslots_cap(&g_spaces); i++) {
        struct tspace *sp = kslots_at(&g_spaces, i);
        if (sp && __atomic_load_n(&sp->pml4, __ATOMIC_ACQUIRE) == pml4) return i;
    }
    return -1;
}

// The trace an address space writes into, or -1.
static int trace_of_space(int s) {
    struct tspace *sp = s < 0 ? 0 : kslots_at(&g_spaces, s);
    return sp ? sp->t : -1;
}

// The trace the CURRENT address space writes into, or -1.
static int current_trace(void) { return trace_of_space(space_of(vmm_current_pml4())); }

static void add_space(uint64_t pml4, int t) {
    if (space_of(pml4) >= 0 || !T(t)) return;
    int s = kslots_alloc(&g_spaces);
    if (s < 0) {
        // Out of memory: this process runs untraced. A tracer that cares
        // sees the gap as a pid that never appears.
        klog_printf(KLOG_WARN "strace: no memory to trace a new address space -- untraced\n");
        return;
    }
    struct tspace *sp = kslots_at(&g_spaces, s);
    sp->t = t;
    __atomic_store_n(&sp->pml4, pml4, __ATOMIC_RELEASE);   // last: a walk matches it from here
    g_nspaces++;
    T(t)->spaces++;
    ring_hdr(T(t)->ring)->live = (uint32_t)T(t)->spaces;
}

static void free_space(int s) {
    struct tspace *sp = kslots_at(&g_spaces, s);
    if (!sp) return;
    __atomic_store_n(&sp->pml4, 0, __ATOMIC_RELEASE);
    kslots_free(&g_spaces, s);
    g_nspaces--;
}

static void drop_trace(int t) {
    struct trace *tr = T(t);
    if (!tr) return;
    for (int i = 0; i < kslots_cap(&g_spaces); i++)
        if (trace_of_space(i) == t) free_space(i);
    ring_hdr(tr->ring)->live = 0;
    ring_unpin(tr->ring);
    kslots_free(&g_traces, t);
}

static void remove_space(int s) {
    int t = trace_of_space(s);
    free_space(s);
    struct trace *tr = T(t);
    if (!tr) return;
    if (--tr->spaces <= 0) drop_trace(t);
    else ring_hdr(tr->ring)->live = (uint32_t)tr->spaces;
}

// A trace into `ring` (already pinned -- the pin becomes the trace's)
// for `owner`, with `pml4` its first address space. The trace's index,
// or -1 with the ring let go.
static int trace_start(int ring, int owner, int follow, uint64_t pml4) {
    int t = kslots_alloc(&g_traces);
    if (t < 0) {
        ring_unpin(ring);
        klog_printf(KLOG_WARN "strace: no memory for a trace -- pid %d's child runs untraced\n", owner);
        return -1;
    }
    *T(t) = (struct trace){ ring, owner, ring_capacity(ring), follow, 0 };
    add_space(pml4, t);
    if (!T(t)->spaces) { drop_trace(t); return -1; }
    return t;
}

// ---- naming the tracee ------------------------------------------------------------

int strace_ring_check(const char *name, int pid) {
    int idx = shm_lookup(name);
    if (idx < 0) return -ENOENT;
    // THE TRACER'S OWN OBJECT ONLY: a ring somebody else made would let a
    // spawner fill another process's memory with its child's syscalls.
    if (shm_creator(idx) != pid) return -EPERM;
    if (!shm_npages(idx) || ring_hdr(idx)->magic != TRACE_RING_MAGIC || ring_capacity(idx) < 2)
        return -EINVAL;
    return idx;
}

int strace_arm_for_current(int ring_idx, int follow) {
    int pid = scheduler_current_pid(), slot = -1;
    for (int i = 0; i < kslots_cap(&g_arms); i++) {
        struct tarm *a = kslots_at(&g_arms, i);
        if (a && a->pid == pid) { ring_unpin(a->ring); a->pid = 0; slot = i; break; }
    }
    int rc = ring_pin(ring_idx);   // ours until the trace ends, whatever the tracer does
    if (rc < 0) {
        if (slot >= 0) kslots_free(&g_arms, slot);
        return rc;
    }
    if (slot < 0 && (slot = kslots_alloc(&g_arms)) < 0) {
        ring_unpin(ring_idx);
        return -EBUSY;
    }
    struct tarm *a = kslots_at(&g_arms, slot);
    a->ring = ring_idx;
    a->follow = follow;
    a->pid = pid;
    return 0;
}

void strace_disarm(void) {
    int pid = scheduler_current_pid();
    for (int i = 0; i < kslots_cap(&g_arms); i++) {
        struct tarm *a = kslots_at(&g_arms, i);
        if (!a || a->pid != pid) continue;
        // An arm the spawn never collected: the spawn failed.
        ring_unpin(a->ring);
        a->pid = 0;
        kslots_free(&g_arms, i);
    }
}

void strace_claim(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    int pid = scheduler_current_pid();
    for (int i = 0; pid && i < kslots_cap(&g_arms); i++) {
        struct tarm *a = kslots_at(&g_arms, i);
        if (!a || a->pid != pid) continue;
        int ring = a->ring, follow = a->follow;
        a->pid = 0;
        kslots_free(&g_arms, i);
        trace_start(ring, pid, follow, pml4_phys);
        return;
    }
    // A FOLLOWED TRACEE SPAWNING: its child joins the same ring. (A
    // traced process exec'ing lands here too, with the new address
    // space; strace_rekey() then drops the old one.)
    int t = current_trace();
    if (t >= 0 && T(t) && T(t)->follow) add_space(pml4_phys, t);
}

void strace_fork(uint64_t parent_pml4, uint64_t child_pml4) {
    int t = trace_of_space(space_of(parent_pml4));
    if (t >= 0 && T(t) && T(t)->follow) add_space(child_pml4, t);
}

int strace_active(void) {
    return g_nspaces && space_of(vmm_current_pml4()) >= 0;
}

void strace_rekey(uint64_t old_pml4, uint64_t new_pml4) {
    int s = space_of(old_pml4);
    if (s < 0) return;
    // Already claimed through the follow path: one entry, not two.
    if (space_of(new_pml4) >= 0) remove_space(s);
    else __atomic_store_n(&((struct tspace *)kslots_at(&g_spaces, s))->pml4, new_pml4,
                          __ATOMIC_RELEASE);
}

void strace_release(uint64_t pml4_phys) {
    int s = space_of(pml4_phys);
    if (s >= 0) remove_space(s);
}

// For a KTEST: a trace into `ring_idx` for `owner`, writing from the
// made-up address space `pml4`, without a spawn. The trace's index, or
// -errno / -1 as an arm or a claim would refuse.
int strace_test_start(int ring_idx, int owner, uint64_t pml4) {
    int rc = ring_pin(ring_idx);
    return rc < 0 ? rc : trace_start(ring_idx, owner, 0, pml4);
}

int strace_test_traced(uint64_t pml4) { return space_of(pml4) >= 0; }

// The table is the kernel's only list of syscall names, so anything else
// in ring 0 that wants to NAME a syscall asks here (`kstack syscalls`).
const char *strace_syscall_name(int nr) {
    if (nr < 0) return 0;
    const struct syscall_desc *d = syscall_desc_at((uint64_t)nr);
    return d ? d->name : 0;
}

// ---- writing records ---------------------------------------------------------

static uint32_t ring_room(int t) {
    struct trace *tr = T(t);
    if (!tr) return 0;
    struct trace_ring_hdr *h = ring_hdr(tr->ring);
    uint32_t used = h->head - h->tail;
    // A tail past the head is a tracer writing nonsense: no room, which
    // stalls only its own tracees -- it could SIGSTOP them anyway.
    return used > tr->nrec ? 0 : tr->nrec - used;
}

static void ring_put(int t, struct trace_rec *r) {
    if (t < 0 || !ring_room(t)) return;
    struct trace *tr = T(t);
    struct trace_ring_hdr *h = ring_hdr(tr->ring);
    uint32_t seq = h->head;
    r->seq = seq;
    // Records are 128 bytes after a 128-byte header, so one never
    // straddles a page: each is written through a single frame.
    uint64_t off = TRACE_HDR_SIZE + (uint64_t)(seq % tr->nrec) * TRACE_REC_SIZE;
    void *slot = (void *)(uintptr_t)(shm_frame(tr->ring, off / 4096) + off % 4096);
    k_memcpy(slot, r, sizeof *r);
    __asm__ volatile ("" ::: "memory");   // the record before the head that publishes it
    h->head = seq + 1;
    futex_note_ready(tr->owner);
}

static void rec_blob(struct trace_rec *r, uint64_t nr, uint64_t pml4) {
    r->blob_arg = 0xFF;
    const struct syscall_desc *d = syscall_desc_at(nr);
    if (!d || !pml4) return;
    for (int i = 0; i < 3; i++) {
        uint64_t ptr = r->a[i], max;
        if (d->args[i] == A_PATH) {
            // To the terminator, a byte at a time: a path near the top of
            // the stack fails a whole-FS_PATH_MAX range check (the dynamic
            // loader's first open did) though every byte of it is readable.
            char src[TRACE_BLOB_MAX + 2];
            if (!ptr || !vmm_copy_string_from_user(pml4, src, ptr, sizeof src)) return;
            uint32_t n = (uint32_t)k_strlen(src);
            r->blob_cut = n > TRACE_BLOB_MAX;
            if (n > TRACE_BLOB_MAX) n = TRACE_BLOB_MAX;
            k_memcpy(r->blob, src, n);
            r->blob_len = (uint16_t)n;
            r->blob_arg = (uint8_t)i;
            return;
        }
        if (d->args[i] == A_BUF && i < 2) max = r->a[i + 1];
        else continue;
        if (!ptr) return;
        if (!max) { r->blob_arg = (uint8_t)i; return; }   // write(fd, p, 0): ""
        if (!vmm_validate_user_range(pml4, ptr, max)) return;
        char src[TRACE_BLOB_MAX + 1];
        uint64_t want = max < sizeof src ? max : sizeof src;
        if (want && !vmm_copy_from_user(pml4, src, ptr, want)) return;
        uint32_t n = want < TRACE_BLOB_MAX ? (uint32_t)want : TRACE_BLOB_MAX;
        k_memcpy(r->blob, src, n);
        r->blob_len = (uint16_t)n;
        r->blob_cut = max > n;
        r->blob_arg = (uint8_t)i;
        return;
    }
}

static void rec_exit(uint64_t nr, int kind, uint64_t rax) {
    int t = current_trace();
    if (t < 0) return;
    struct trace_rec r;
    k_memset(&r, 0, sizeof r);
    r.pid = scheduler_current_pid();
    r.nr = (uint16_t)nr;
    r.kind = (uint16_t)kind;
    r.blob_arg = 0xFF;
    r.ret = (int64_t)rax;
    ring_put(t, &r);
}

int strace_wait_for_room(uint64_t *regs) {
    int t = current_trace();
    if (t < 0 || !T(t) || ring_room(t) >= 2) return 0;
    // A tracer that is gone will never drain: its trace is abandoned and
    // its tracees run on untraced. Waiting would hang them for good.
    if (!scheduler_pid_alive(T(t)->owner)) {
        drop_trace(t);
        return 0;
    }
    ring_hdr(T(t)->ring)->stalls++;
    // RE-ISSUE THE CALL rather than hold it: back over the syscall
    // instruction, sleep, and RAX (which a wake writes) back to the
    // number -- so it runs again once there is room for both its
    // records. Nothing has run, so nothing is lost; the signal path
    // rewinds the same way for SA_RESTART.
    uint64_t nr = regs[SCHED_TF_RAX];
    regs[SCHED_TF_RIP] -= SYSCALL_INSN_LEN;
    scheduler_sleep_current(regs, clocksource_now_ns() + 1000000ull);
    regs[SCHED_TF_RAX] = nr;
    return 1;
}

void strace_begin(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    int t = current_trace();
    if (t < 0) return;
    struct trace_rec r;
    k_memset(&r, 0, sizeof r);
    r.pid = scheduler_current_pid();
    r.nr = (uint16_t)nr;
    r.kind = TRACE_ENTRY;
    r.a[0] = a0; r.a[1] = a1; r.a[2] = a2;
    rec_blob(&r, nr, vmm_current_pml4());
    ring_put(t, &r);
}

void strace_end(uint64_t nr, uint64_t rax)         { rec_exit(nr, TRACE_EXIT, rax); }
void strace_end_noreturn(uint64_t nr)              { rec_exit(nr, TRACE_NORETURN, 0); }
void strace_end_resumed(uint64_t nr, uint64_t rax) { rec_exit(nr, TRACE_RESUMED, rax); }
