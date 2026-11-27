// The remote-activity ring. See kernel/include/kernel/remote_log.h for
// what a record is and why remoteness is derived rather than declared.
#include "remote_log.h"
#include "ktime.h"
#include "clocksource.h"
#include "string.h"
#include "klog.h"
#include "scheduler.h"
#include "vmm.h"
#include "syscalls.h"
#include "errno.h"
#include "proc_info.h"
#include "ktest.h"
#include "kslots.h"

// Enough to cover a whole flash (a few dozen commands and the transfers
// between them) without the ring being the reason something is missed,
// and small enough that a per-session buffer was never worth it. The
// tray flyout shows the last dozen; `sessions` explains the rest.
#define REMOTE_LOG_MAX 128

static struct query_remotelog g_ring[REMOTE_LOG_MAX];
static uint64_t g_next_seq = 1;   // the NEXT record's number; 0 is "none"
static int g_count;
static int g_head;                // where the oldest one is

// Open sessions, so the tray item can hide itself when nobody is on.
// A pid rather than a count alone: a session that dies without a close
// (the shell killed, the link dropped) must not leave the indicator up
// forever, and scheduler_exit() names the leader it is retiring.
//
// GROWN ON DEMAND (kslots.h): a slot is taken at a spawn, which may
// allocate, and freed at an exit, which only marks it; a reader walking
// the table meanwhile sees a live slot or none, never freed memory.
struct remote_session {
    int leader;
    uint32_t ip;
    uint64_t opened_utc;
    char comm[24];
    char status[QUERY_REMOTESESS_STATUS_MAX];
};
static struct kslots g_sessions = KSLOTS_INIT(sizeof(struct remote_session));

// The live session led by `leader`, and its slot in *slot; NULL for none.
static struct remote_session *session_of(int leader, int *slot) {
    for (int i = 0; i < kslots_cap(&g_sessions); i++) {
        struct remote_session *r = kslots_at(&g_sessions, i);
        if (r && r->leader == leader) {
            if (slot) *slot = i;
            return r;
        }
    }
    return 0;
}

void remote_log_record(int kind, uint32_t remote_ip, int pid,
                       const char *comm, const char *text) {
    int slot = (g_head + g_count) % REMOTE_LOG_MAX;
    if (g_count == REMOTE_LOG_MAX) {
        slot = g_head;
        g_head = (g_head + 1) % REMOTE_LOG_MAX;
    } else {
        g_count++;
    }

    struct query_remotelog *r = &g_ring[slot];
    k_memset(r, 0, sizeof *r);
    r->seq = g_next_seq++;
    r->utc = ktime_now_sec();
    r->monotonic_ns = clocksource_now_ns();
    r->kind = (uint64_t)kind;
    r->remote_ip = remote_ip;
    r->pid = (uint64_t)pid;
    r->sessions = (uint64_t)remote_log_sessions();
    if (comm) k_strlcpy(r->comm, comm, sizeof r->comm);
    if (text) k_strlcpy(r->text, text, sizeof r->text);
}

uint64_t remote_log_total(void) { return g_next_seq - 1; }

uint64_t remote_log_oldest(void) {
    if (!g_count) return 0;
    return g_ring[g_head].seq;
}

int remote_log_get(uint64_t seq, struct query_remotelog *out) {
    if (!out || !g_count) return 0;
    uint64_t oldest = g_ring[g_head].seq;
    if (seq < oldest || seq > remote_log_total()) return 0;
    int slot = (g_head + (int)(seq - oldest)) % REMOTE_LOG_MAX;
    // The slot's own seq is re-checked for conn_log's reason: a record
    // written between the index arithmetic and the copy moves the ring
    // under a reader, and answering the WRONG record is worse than
    // answering none.
    if (g_ring[slot].seq != seq) return 0;
    *out = g_ring[slot];
    return 1;
}

int remote_log_sessions(void) { return kslots_live(&g_sessions); }

// IS THIS SESSION REMOTE, AND WHOSE? The spawn path asks per spawn, so
// it is a walk of the open sessions rather than anything cleverer.
uint32_t remote_log_session_of(int leader_pid) {
    if (leader_pid <= 0) return 0;
    struct remote_session *r = session_of(leader_pid, 0);
    return r ? r->ip : 0;
}

uint32_t remote_log_session_ip(void) {
    for (int i = kslots_cap(&g_sessions) - 1; i >= 0; i--) {
        struct remote_session *r = kslots_at(&g_sessions, i);
        if (r && r->leader) return r->ip;
    }
    return 0;
}

void remote_log_session_opened(int leader_pid, uint32_t ip, const char *creator) {
    if (leader_pid <= 0) return;
    if (!creator || !creator[0]) creator = "?";
    int slot = kslots_alloc(&g_sessions);
    struct remote_session *r = slot >= 0 ? kslots_at(&g_sessions, slot) : 0;
    if (r) {
        // Zeroed by kslots_alloc(); the leader LAST, so a walk that sees
        // the slot live before this finishes sees no leader to match.
        r->ip = ip;
        r->opened_utc = ktime_now_sec();
        k_strlcpy(r->comm, creator, sizeof r->comm);
        r->leader = leader_pid;
        remote_log_record(QUERY_REMOTE_SESSION, ip, leader_pid, creator, "session opened");
        return;
    }
    // OUT OF MEMORY STILL WRITES THE RECORD, because the alternative is
    // a session nobody can see; only the per-session slot is lost, which
    // costs the close line.
    remote_log_record(QUERY_REMOTE_SESSION, ip, leader_pid, creator,
                      "session opened (no memory to track it)");
}

int remote_log_session_count(void) { return remote_log_sessions(); }

int remote_log_session_get(int index, struct query_remotesess *out) {
    for (int i = 0; i < kslots_cap(&g_sessions); i++) {
        struct remote_session *r = kslots_at(&g_sessions, i);
        if (!r || !r->leader || index--) continue;
        k_memset(out, 0, sizeof *out);
        out->pid = (uint64_t)r->leader;
        out->remote_ip = r->ip;
        out->opened_utc = r->opened_utc;
        k_strlcpy(out->comm, r->comm, sizeof out->comm);
        k_strlcpy(out->status, r->status, sizeof out->status);
        return 1;
    }
    return 0;
}

void remote_log_session_closed(int leader_pid) {
    int slot;
    struct remote_session *r = leader_pid > 0 ? session_of(leader_pid, &slot) : 0;
    if (!r) return;
    // THE SLOT GOES FIRST, so the record this writes carries the count
    // AFTER the close -- a reader takes the newest record's `sessions`
    // as the current answer, and a close that still counted itself would
    // leave the indicator up forever.
    uint32_t ip = r->ip;
    char comm[sizeof r->comm];
    k_strlcpy(comm, r->comm, sizeof comm);
    r->leader = 0;
    kslots_free(&g_sessions, slot);
    remote_log_record(QUERY_REMOTE_SESSION, ip, leader_pid, comm, "session closed");
}

// --- the syscall ------------------------------------------------------

int sys_remote_log(struct syscall_ctx *c) {
    int kind = (int)c->a0;
    uint32_t claimed_ip = (uint32_t)c->a1;

    char text[QUERY_REMOTELOG_TEXT_MAX];
    if (!vmm_copy_string_from_user(c->pml4, text, c->a2, sizeof text)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    if (kind < QUERY_REMOTE_SESSION || kind > QUERY_REMOTE_STATUS) {
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    int pid = scheduler_current_pid();
    // A STATUS IS THE SESSION LEADER'S OWN LINE about its session, and
    // only a leader of a live remote session has one to set -- anyone
    // else is told so, since there is nothing they could mean.
    if (kind == QUERY_REMOTE_STATUS) {
        struct remote_session *r = pid > 0 ? session_of(pid, 0) : 0;
        if (r) {
            k_strlcpy(r->status, text, sizeof r->status);
            c->regs[14] = 0;
            return 0;
        }
        c->regs[14] = (uint64_t)(int64_t)-ESRCH;
        return 0;
    }
    // THE SESSION'S PEER OUTRANKS THE CALLER'S CLAIM, which is what
    // makes a shell's record trustworthy: it says what was typed, and
    // the kernel says where from. A caller with no remote session may
    // still report one it is SERVING (tftpd naming its client), and a
    // caller with neither is dropped -- silently, and still 0, because
    // a program must not behave differently for being unwatched.
    uint32_t ip = remote_log_session_of(scheduler_sid(pid));
    if (!ip) ip = claimed_ip;
    if (!ip) {
        c->regs[14] = 0;
        return 0;
    }

    struct proc_info info;
    const char *comm = "";
    if (scheduler_proc_info_pid(pid, &info)) comm = info.name;
    remote_log_record(kind, ip, pid, comm, text);
    c->regs[14] = 0;
    return 0;
}

// --- KTESTs ---------------------------------------------------------------

KTEST("remote", "a session is listed with its creator and status until it closes") {
    int pid = 0x7ffff0;   // no process has it; the table only keeps numbers
    int before = remote_log_session_count();
    remote_log_session_opened(pid, 0xC0A80167, "remoted");
    struct query_remotesess q;
    int found = 0;
    for (int i = 0; remote_log_session_get(i, &q); i++)
        if ((int)q.pid == pid) { found = 1; break; }
    struct remote_session *r = session_of(pid, 0);
    if (r) k_strlcpy(r->status, "VNC, view only", sizeof r->status);
    struct query_remotesess q2;
    int status_ok = 0;
    for (int i = 0; remote_log_session_get(i, &q2); i++)
        if ((int)q2.pid == pid) status_ok = !k_strcmp(q2.status, "VNC, view only");
    remote_log_session_closed(pid);
    KTEST_ASSERT(found);
    KTEST_ASSERT_EQ(q.remote_ip, 0xC0A80167);
    KTEST_ASSERT(!k_strcmp(q.comm, "remoted"));
    KTEST_ASSERT(status_ok);
    KTEST_ASSERT_EQ(remote_log_session_count(), before);
}

// Past the eight the table once held: every session is listed and each
// close takes its own row away.
KTEST("remote", "twelve sessions at once are all listed, and all close") {
    enum { N = 12 };
    int base = 0x7fff00;   // no process has these
    int before = remote_log_session_count();
    for (int i = 0; i < N; i++) remote_log_session_opened(base + i, 0x0A000002u + (uint32_t)i, "remoted");
    int during = remote_log_session_count();
    int listed = 0, ip_ok = 1;
    struct query_remotesess q;
    for (int i = 0; remote_log_session_get(i, &q); i++) {
        if ((int)q.pid < base || (int)q.pid >= base + N) continue;
        listed++;
        if (q.remote_ip != 0x0A000002u + (uint32_t)((int)q.pid - base)) ip_ok = 0;
    }
    uint32_t last_ip = remote_log_session_of(base + N - 1);
    for (int i = 0; i < N; i++) remote_log_session_closed(base + i);
    KTEST_ASSERT_EQ(during, before + N);
    KTEST_ASSERT_EQ(listed, N);
    KTEST_ASSERT(ip_ok);
    KTEST_ASSERT_EQ(last_ip, 0x0A000002u + N - 1);
    KTEST_ASSERT_EQ(remote_log_session_count(), before);
}
