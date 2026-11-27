// uchan -- see base/uchan.h for what this is and base/uchan_page.h for the
// layout. Nothing here is a syscall of its own: a channel is named
// shared memory plus a futex, both of which exist for other reasons.
#include "base/uchan.h"
#include "rt/sys.h"
#include "syscall_abi.h"
#include "query_abi.h"
#include "base/ugrow.h"
#include "base/ulog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define RING_BYTES ((uint64_t)sizeof(struct uchan_ring))

// **AN UNMAP IS THE WHOLE REGION OR NOTHING.** sys_munmap() refuses a
// shared mapping that is not exactly the page-rounded region, and it
// refuses SILENTLY to a caller that ignores the return -- so an unmap
// with the object's byte size (20 for a beacon) leaked the mapping
// every time, and a client that probed the beacon every half second
// ran its address space out of mappings and read that as the server
// having died. Every unmap here goes through this.
static void unmap(void *p, uint64_t bytes) {
    if (p) sys_munmap(p, (bytes + 4095) & ~4095ull);
}

// A BEACON is public: every client has to be able to find it. Its rings
// are not -- see uchan_client_open().
static void *map_public(const char *name, uint64_t bytes) {
    int fd = sys_shm_open(name, bytes, SHM_CREATE | SHM_PUBLIC);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    return p == (void *)-1 ? 0 : p;
}

// An EXISTING object, only if `pid` created it -- the kernel's word.
static void *map_from(const char *name, uint64_t bytes, int pid) {
    int fd = sys_shm_open_from(name, pid);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    return p == (void *)-1 ? 0 : p;
}

// `create`: made here, and refused (-1) if the name is taken.
static void *map_object(const char *name, uint64_t bytes, int create) {
    int fd = sys_shm_open(name, create ? bytes : 0, create ? SHM_CREATE | SHM_EXCL : 0);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    return p == (void *)-1 ? 0 : p;
}

// --- server ----------------------------------------------------------

int uchan_server_open(struct uchan_server *s, const char *name) {
    memset(s, 0, sizeof *s);
    if (!name || !*name || strlen(name) >= UCHAN_NAME_MAX || strchr(name, '.')) return -1;
    snprintf(s->name, sizeof s->name, "%s", name);

    // Unlink first: a previous server that died left its beacon behind,
    // and a client opening THAT would wake a pid that no longer exists.
    sys_shm_unlink(name);
    s->beacon = map_public(name, sizeof(struct uchan_beacon));
    if (!s->beacon) return -1;

    s->beacon->wake = 0;
    s->beacon->server_pid = sys_getpid();
    s->beacon->version = 1;
    // LAST, so a client that sees the magic sees a page that is ready.
    s->beacon->magic = UCHAN_MAGIC;

    // The kernel bumps this same word when it queues an event, which is
    // what lets one wait cover clients AND input.
    sys_wakeword(&s->beacon->wake);
    return 0;
}

static int ring_index(const struct uchan_server *s, int conn) {
    for (int i = 0; i < s->count; i++) if (s->c[i].conn == conn) return i;
    return -1;
}

// "<name>.<pid>", or "<name>.<pid>.<sub>" for a process's further rings.
static void ring_name(char *out, unsigned long cap, const char *svc, int pid, uint32_t sub) {
    if (sub) snprintf(out, cap, "%s.%d.%u", svc, pid, (unsigned)sub);
    else snprintf(out, cap, "%s.%d", svc, pid);
}

static int ring_held(const struct uchan_server *s, int pid, uint32_t sub) {
    for (int i = 0; i < s->count; i++) if (s->c[i].pid == pid && s->c[i].sub == sub) return i;
    return -1;
}

int uchan_server_peer_pid(const struct uchan_server *s, int conn) {
    int i = ring_index(s, conn);
    return i < 0 ? 0 : s->c[i].pid;
}

int uchan_server_ring_name(const struct uchan_server *s, int conn, char *out, unsigned long cap) {
    int i = ring_index(s, conn);
    if (i < 0) return 0;
    ring_name(out, cap, s->name, s->c[i].pid, s->c[i].sub);
    return 1;
}

int uchan_server_scan(struct uchan_server *s, int *gone, int gone_cap) {
    int ngone = 0;
    if (!s->beacon) return 0;
    s->scan_wake = __atomic_load_n(&s->beacon->wake, __ATOMIC_ACQUIRE);   // BEFORE the walk
    s->scan_valid = 1;
    // Rings whose NAME now belongs to another object: the client execed
    // (the kernel let its old ring's name go) and opened its channel
    // again before this scan ran, so the name never stopped resolving.
    // Its old ring is a dead client's, dropped below as one; the new one
    // is adopted next scan. Pid alone could not tell them apart.
    for (int i = 0; i < s->count; i++) s->c[i].replaced = 0;
    char prefix[UCHAN_NAME_MAX + 2];
    snprintf(prefix, sizeof prefix, "%s.", s->name);
    size_t plen = strlen(prefix);

    struct query_shm rec;
    QUERY_FOREACH(QUERY_SHM, rec, qi) {
        if (rec.flags & QUERY_SHM_UNLINKED) continue;
        if (strncmp(rec.name, prefix, plen) != 0) continue;
        // A RING IS "<server>.<pid>[.<n>]" EXACTLY: a client's other
        // objects may share the prefix (a webview's "wv63.65b" buffer
        // beside its "wv63.65" ring), and one taken for the ring read as a
        // replaced ring and dropped the live one every scan.
        int pid = 0;
        uint32_t sub = 0;
        const char *p = rec.name + plen;
        for (; *p >= '0' && *p <= '9'; p++)
            pid = pid * 10 + (*p - '0');
        if (*p == '.') {
            const char *q = ++p;
            for (; *p >= '0' && *p <= '9'; p++) sub = sub * 10u + (uint32_t)(*p - '0');
            if (p == q || !sub) continue;
        }
        if (*p) continue;
        int held = pid > 0 ? ring_held(s, pid, sub) : -1;
        if (held >= 0 && rec.serial && s->c[held].serial && rec.serial != s->c[held].serial)
            s->c[held].replaced = 1;
        if (pid <= 0 || held >= 0) continue;
        // **A RING SPEAKS FOR THE PID IN ITS NAME ONLY IF THAT PID MADE
        // IT.** Otherwise one client could post as another -- a page's
        // engine as the Browser itself, to /bin/webnet.
        if (rec.creator_pid != pid) continue;

        struct uchan_ring *r = map_from(rec.name, RING_BYTES, pid);
        if (!r) continue;
        // A ring whose header does not agree is one this build cannot
        // read. Dropped rather than guessed at -- the client is still
        // writing into it and would see its messages vanish either way,
        // but a mis-sized read would scribble past the mapping.
        if (r->magic != UCHAN_MAGIC || r->slot_bytes != UCHAN_SLOT_BYTES ||
            r->slots != UCHAN_SLOTS) {
            unmap(r, RING_BYTES);
            continue;
        }
        if (!UGROW(s->c, s->cap, s->count + 1)) {
            // Out of memory: the ring stays unadopted, and the next scan
            // tries again.
            if (!s->warned++) ulogf("uchan: %s: no memory for client %d's ring -- it waits\n", s->name, pid);
            unmap(r, RING_BYTES);
            continue;
        }
        // A further ring's id is past every pid; wrapping back to the
        // first is safe while 2^30 connections have not come and gone.
        int conn = pid;
        if (sub) {
            conn = UCHAN_CONN_EXTRA + s->next_extra;
            s->next_extra = (s->next_extra + 1) & (UCHAN_CONN_EXTRA - 1);
        }
        struct uchan_conn *nc = &s->c[s->count++];
        nc->ring = r;
        nc->conn = conn;
        nc->pid = pid;
        nc->sub = sub;
        nc->serial = rec.serial;
        nc->replaced = 0;
    }

    // A CLIENT THAT DIED TAKES ITS NAME WITH IT (the kernel unlinks a
    // dead creator's objects), so a name that no longer resolves is how
    // this notices. The frames live until this unmaps, which is why the
    // drop has to happen rather than being left to the allocator.
    for (int k = 0; k < s->count; ) {
        char nm[SHM_NAME_MAX];
        ring_name(nm, sizeof nm, s->name, s->c[k].pid, s->c[k].sub);
        int fd = s->c[k].replaced ? -1 : sys_shm_open(nm, 0, 0);
        if (fd >= 0) { sys_close(fd); k++; continue; }
        // **WHICH CLIENT WENT IS REPORTED, NOT JUST RECLAIMED.** A
        // server usually holds state per client -- the compositor holds
        // a window -- and reclaiming the slot silently leaves that
        // state with nothing to retire it. A death that does not fit in
        // `gone` KEEPS its ring, so the next scan reports it: dropping
        // the ring unreported lost the death for good, and its windows
        // with it.
        if (gone && ngone >= gone_cap) { k++; continue; }
        if (gone) gone[ngone] = s->c[k].conn;
        ngone++;
        unmap(s->c[k].ring, RING_BYTES);
        s->c[k] = s->c[--s->count];
    }
    return ngone;
}

// The bytes actually moved: never more than the slot holds, never more
// than the caller's buffer takes.
static unsigned long clamp_len(unsigned long n) {
    return n > UCHAN_SLOT_BYTES ? UCHAN_SLOT_BYTES : n;
}

int uchan_server_recv(struct uchan_server *s, void *out, unsigned long cap) {
    if (!s->beacon || s->count <= 0) return 0;
    // ROUND-ROBIN from where the last one left off, so a client sending
    // continuously cannot hold the others off forever.
    for (int n = 0; n < s->count; n++) {
        int i = (int)((s->next + (unsigned)n) % (unsigned)s->count);
        struct uchan_ring *r = s->c[i].ring;
        if (r->head == r->tail) continue;
        uint32_t slot = r->tail % UCHAN_SLOTS;
        memcpy(out, r->slot[slot], clamp_len(cap));
        // The copy BEFORE the tail bump: the client is free to refill
        // this slot the moment it sees the space, and a bump first is a
        // message read half from the old sender and half from the new.
        r->tail++;
        s->next = (unsigned)(i + 1);
        return s->c[i].conn;
    }
    return 0;
}

void uchan_server_reply(struct uchan_server *s, int conn, const void *msg,
                        unsigned long len) {
    int i = ring_index(s, conn);
    if (i < 0) return;
    struct uchan_ring *r = s->c[i].ring;
    len = clamp_len(len);
    memcpy((void *)r->reply, msg, len);
    // The tail of the slot is ZEROED, not left as it was: a reader
    // asking for more than this message carries must see zeros rather
    // than the previous message's bytes.
    memset((void *)r->reply + len, 0, UCHAN_SLOT_BYTES - len);
    r->reply_seq++;   // last, so a client that sees it sees the payload
    sys_futex_wake(&r->reply_seq, 0);
}

void uchan_server_wait(struct uchan_server *s, int timeout_ms) {
    if (!s->beacon) { sys_sleep_ms(timeout_ms); return; }
    // SINCE THE LAST SCAN, not since now: a client whose ring the scan
    // did not see yet rang after it, and only the word sampled before the
    // scan has moved for it (struct uchan_server's `scan_wake`).
    uint32_t seen = s->scan_valid ? s->scan_wake : s->beacon->wake;
    s->scan_valid = 0;   // one use: a loop that waits without scanning samples afresh
    // ANYTHING ALREADY QUEUED MEANS DO NOT PARK. Checked after sampling
    // the word, so a message arriving between the two moves it and the
    // wait below returns at once instead of sleeping through it.
    for (int i = 0; i < s->count; i++)
        if (s->c[i].ring->head != s->c[i].ring->tail) return;
    sys_futex_wait(&s->beacon->wake, seen, timeout_ms);
}

int uchan_server_room(struct uchan_server *s, int conn) {
    int i = ring_index(s, conn);
    if (i < 0) return 0;
    struct uchan_ring *r = s->c[i].ring;
    return (int)(UCHAN_IN_SLOTS - (r->in_head - r->in_tail));
}

int uchan_server_send(struct uchan_server *s, int conn, const void *msg,
                      unsigned long len) {
    int i = ring_index(s, conn);
    if (i < 0) return -1;
    struct uchan_ring *r = s->c[i].ring;
    if (r->in_head - r->in_tail >= UCHAN_IN_SLOTS) {
        r->in_dropped++;
        return -1;
    }
    if (len > UCHAN_IN_BYTES) len = UCHAN_IN_BYTES;
    uint8_t *slot = r->in_slot[r->in_head % UCHAN_IN_SLOTS];
    memcpy(slot, msg, len);
    memset(slot + len, 0, UCHAN_IN_BYTES - len);
    r->in_head++;   // after the payload, for the reader's sake
    // Bump, then wake -- the same order uchan_send() keeps, for the
    // same lost-wakeup reason. Atomic because the client's own threads
    // bump this word too (uchan_client_kick).
    __atomic_fetch_add(&r->in_wake, 1, __ATOMIC_SEQ_CST);
    sys_futex_wake((void *)&r->in_wake, 0);
    return 0;
}

void uchan_server_close(struct uchan_server *s) {
    if (!s->beacon) return;
    sys_wakeword(0);
    for (int i = 0; i < s->count; i++) unmap(s->c[i].ring, RING_BYTES);
    free(s->c);
    sys_shm_unlink(s->name);
    unmap(s->beacon, sizeof(struct uchan_beacon));
    memset(s, 0, sizeof *s);
}

// --- client ----------------------------------------------------------

int uchan_client_open(struct uchan_client *c, const char *name) {
    return uchan_client_open_from(c, name, 0);
}

int uchan_client_open_from(struct uchan_client *c, const char *name, int pid) {
    memset(c, 0, sizeof *c);
    if (!name || !*name || strlen(name) >= UCHAN_NAME_MAX || strchr(name, '.')) return -1;

    c->beacon = pid > 0 ? map_from(name, sizeof(struct uchan_beacon), pid)
                        : map_object(name, sizeof(struct uchan_beacon), 0);
    if (!c->beacon) return -1;              // no server (or not that one)
    int vouched = 0;
    if (c->beacon->magic == UCHAN_MAGIC && c->beacon->server_pid > 0 &&
        (pid <= 0 || c->beacon->server_pid == pid)) {
        int fd = sys_shm_open_from(name, c->beacon->server_pid);
        vouched = fd >= 0;
        if (fd >= 0) sys_close(fd);
    }
    if (!vouched) {
        unmap(c->beacon, sizeof(struct uchan_beacon));
        c->beacon = 0;
        return -1;
    }

    // THE PROCESS'S FIRST RING IS "<name>.<pid>", a further one
    // "<name>.<pid>.<n>" -- each claimed EXCLUSIVELY, so a second channel
    // to one server is a second connection instead of replacing the first
    // (the desktop's lock screen dropped its own privd agent, f0c30a5fe).
    // No stale ring of this pid holds the name: the kernel unlinks a
    // creator's objects at its death and its exec.
    static uint32_t next_sub;   // process-wide; ids are never reused
    uint32_t sub = 0;
    for (int tries = 0; tries < 8; tries++) {
        ring_name(c->ring_name, sizeof c->ring_name, name, sys_getpid(), sub);
        if (strlen(c->ring_name) + 2 > SHM_NAME_MAX) break;   // ubulk's "b" must fit too
        c->ring = map_object(c->ring_name, RING_BYTES, 1);
        if (c->ring) break;
        sub = __atomic_add_fetch(&next_sub, 1, __ATOMIC_RELAXED);
    }
    if (!c->ring) {
        unmap(c->beacon, sizeof(struct uchan_beacon));
        c->beacon = 0;
        return -1;
    }
    c->ring->slot_bytes = UCHAN_SLOT_BYTES;
    c->ring->slots = UCHAN_SLOTS;
    c->ring->client_pid = sys_getpid();
    // THE SERVER IS LET IN EXPLICITLY. A ring is a channel between two
    // processes and is private like any other named object, so the one
    // that has to read it is granted by name -- and nobody else can,
    // which before this any process could do by reading `lsshm`.
    sys_shm_grant(c->ring_name, c->beacon->server_pid);
    c->ring->magic = UCHAN_MAGIC;   // last: the server's admission check
    return 0;
}

int uchan_send(struct uchan_client *c, const void *msg, unsigned long len) {
    if (!c->ring || !c->beacon) return -1;
    if (c->ring->head - c->ring->tail >= UCHAN_SLOTS) return -1;  // full
    len = clamp_len(len);
    uint8_t *slot = c->ring->slot[c->ring->head % UCHAN_SLOTS];
    memcpy(slot, msg, len);
    memset(slot + len, 0, UCHAN_SLOT_BYTES - len);
    c->ring->head++;   // after the payload, for the reader's sake

    // THE BUMP, THEN THE WAKE, and the order is what closes the race: a
    // server that sampled `wake` before this and parks after it finds
    // the value moved and does not park at all. A wake alone would land
    // in that window and be lost.
    c->beacon->wake++;
    sys_futex_wake(&c->beacon->wake, 0);
    return 0;
}

int uchan_send_wait(struct uchan_client *c, const void *msg, unsigned long len, int timeout_ms) {
    // Polled at a millisecond: the server says nothing when it drains,
    // and a burst that fills 32 slots is gone within a frame of its
    // loop -- a wake word for "room" would cost every message a write.
    for (int waited = 0;; waited++) {
        if (uchan_send(c, msg, len) == 0) return 0;
        if (waited >= timeout_ms || !c->ring || !uchan_client_server_alive(c)) return -1;
        sys_sleep_ms(1);
    }
}

int uchan_call(struct uchan_client *c, const void *msg, unsigned long len,
               void *reply, unsigned long reply_cap, int timeout_ms) {
    if (!c->ring) return -1;
    uint32_t seq = c->ring->reply_seq;
    if (uchan_send(c, msg, len) < 0) return -1;
    // Sampled BEFORE the send, so an answer that arrives while this is
    // still in uchan_send() is not waited for a second time.
    while (c->ring->reply_seq == seq) {
        if (sys_futex_wait(&c->ring->reply_seq, seq, timeout_ms) < 0 &&
            c->ring->reply_seq == seq)
            return -1;   // -EAGAIN means it moved; anything else is the timeout
    }
    memcpy(reply, (const void *)c->ring->reply, clamp_len(reply_cap));
    return 0;
}

void uchan_client_close(struct uchan_client *c) {
    if (c->ring) {
        sys_shm_unlink(c->ring_name);
        unmap(c->ring, RING_BYTES);
    }
    if (c->beacon) unmap(c->beacon, sizeof(struct uchan_beacon));
    memset(c, 0, sizeof *c);
}

// --- client: the inbox ----------------------------------------------

int uchan_client_pending(const struct uchan_client *c) {
    if (!c->ring) return 0;
    return (int)(c->ring->in_head - c->ring->in_tail);
}

int uchan_client_recv(struct uchan_client *c, void *out, unsigned long cap) {
    if (!c->ring) return 0;
    struct uchan_ring *r = c->ring;
    if (r->in_head == r->in_tail) return 0;
    if (cap > UCHAN_IN_BYTES) cap = UCHAN_IN_BYTES;
    memcpy(out, r->in_slot[r->in_tail % UCHAN_IN_SLOTS], cap);
    r->in_tail++;   // the copy first -- the server refills on seeing space
    return 1;
}

void uchan_client_wait(struct uchan_client *c, int timeout_ms) {
    if (!c->ring || timeout_ms <= 0) return;
    uint32_t seen = c->ring->in_wake;
    // Sampled BEFORE the emptiness test, so a message landing between
    // the two moves the word and the wait returns at once.
    if (c->ring->in_head != c->ring->in_tail) return;
    sys_futex_wait((void *)&c->ring->in_wake, seen, timeout_ms);
}

void uchan_client_kick(struct uchan_client *c) {
    if (!c->ring) return;
    __atomic_fetch_add(&c->ring->in_wake, 1, __ATOMIC_SEQ_CST);
    sys_futex_wake((void *)&c->ring->in_wake, 0);
}

uint32_t uchan_client_dropped(const struct uchan_client *c) {
    return c->ring ? c->ring->in_dropped : 0;
}

int uchan_client_server_alive(const struct uchan_client *c) {
    if (!c->beacon) return 0;
    // THE NAME, NOT THE MAPPING: this process's view of the beacon is
    // the old page whatever happened, so the live object is opened
    // afresh and its pid compared. A successor compositor has a new
    // pid, and a ring granted to the old one is nothing to it.
    char name[UCHAN_NAME_MAX];
    const char *dot = strchr(c->ring_name, '.');
    unsigned long n = dot ? (unsigned long)(dot - c->ring_name) : 0;
    if (!n || n >= sizeof name) return 0;
    memcpy(name, c->ring_name, n);
    name[n] = '\0';
    struct uchan_beacon *b = map_object(name, sizeof *b, 0);
    if (!b) return 0;
    int alive = b->magic == UCHAN_MAGIC && b->server_pid == c->beacon->server_pid;
    unmap(b, sizeof *b);
    return alive;
}
