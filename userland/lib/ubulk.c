// ubulk -- see lib/ubulk.h.
#include "lib/ubulk.h"
#include "rt/sys.h"
#include "syscall_abi.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UBULK_MAGIC 0x4B4C5542u   // "BULK"
#define PAGE_BYTES  (((uint64_t)sizeof(struct ubulk_page) + 4095) & ~4095ull)
#define RMASK       (UBULK_RING_BYTES - 1)

// A record's header in the ring. Records start on 16 bytes, so a header
// never wraps; a payload may, and is copied in two pieces.
struct rec_hdr {
    uint32_t type, id, len, _pad;
};

static uint32_t rec_bytes(uint32_t len) { return (uint32_t)sizeof(struct rec_hdr) + ((len + 15u) & ~15u); }

static void barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static void ring_copy_in(struct ubulk_ring *r, uint32_t at, const void *src, uint32_t n) {
    uint32_t off = at & RMASK, first = UBULK_RING_BYTES - off;
    if (first > n) first = n;
    memcpy(r->data + off, src, first);
    if (n > first) memcpy(r->data, (const uint8_t *)src + first, n - first);
}

static void ring_copy_out(const struct ubulk_ring *r, uint32_t at, void *dst, uint32_t n) {
    uint32_t off = at & RMASK, first = UBULK_RING_BYTES - off;
    if (first > n) first = n;
    memcpy(dst, r->data + off, first);
    if (n > first) memcpy((uint8_t *)dst + first, r->data, n - first);
}

// 1 if the record went in, 0 if the ring has no room for it now.
static int ring_put(struct ubulk_ring *r, uint32_t type, uint32_t id, const void *data, uint32_t len) {
    uint32_t need = rec_bytes(len), head = r->head;
    if (need > UBULK_RING_BYTES - (head - r->tail)) return 0;
    struct rec_hdr h = { type, id, len, 0 };
    ring_copy_in(r, head, &h, sizeof h);
    if (len) ring_copy_in(r, head + (uint32_t)sizeof h, data, len);
    barrier();          // the payload before the head that publishes it
    r->head = head + need;
    return 1;
}

// 1 with `m` filled, 0 when empty. A record bigger than `cap` is passed
// over and the next one tried.
static int ring_get(struct ubulk_ring *r, struct ubulk_msg *m, void *buf, uint32_t cap) {
    for (;;) {
        uint32_t tail = r->tail;
        if (tail == r->head) return 0;
        barrier();      // the head before the bytes it covers
        struct rec_hdr h;
        ring_copy_out(r, tail, &h, sizeof h);
        uint32_t need = rec_bytes(h.len);
        int fits = h.len <= cap && h.len <= UBULK_REC_MAX;
        if (fits && h.len) ring_copy_out(r, tail + (uint32_t)sizeof h, buf, h.len);
        barrier();      // the copy before the tail that frees the space
        r->tail = tail + need;
        __atomic_fetch_add(&r->space, 1, __ATOMIC_SEQ_CST);
        sys_futex_wake(&r->space, 0);
        if (!fits) continue;
        m->type = h.type;
        m->id = h.id;
        m->len = h.len;
        m->data = buf;
        return 1;
    }
}

// Puts, waiting for room while `alive(ctx)` and the time allows. The
// lock (when given) is held around each try and dropped across a wait.
static int ring_put_wait(struct ubulk_ring *r, uint32_t type, uint32_t id, const void *data,
                         uint32_t len, int timeout_ms, int (*alive)(void *), void *ctx) {
    if (len > UBULK_REC_MAX) return -1;
    unsigned long long until = sys_monotonic_ns() + (unsigned long long)timeout_ms * 1000000ull;
    for (;;) {
        uint32_t seen = r->space;
        if (ring_put(r, type, id, data, len)) return 0;
        if (sys_monotonic_ns() >= until || !alive(ctx)) return -1;
        sys_futex_wait(&r->space, seen, 20);
    }
}

// The writer's half of the doorbell: 1 if the reader must be rung.
static int bell_needed(struct ubulk_ring *r) {
    barrier();          // our head store before reading the reader's bell
    if (r->bell) return 0;
    r->bell = 1;
    return 1;
}

// The reader's half: cleared BEFORE the drain.
static void bell_clear(struct ubulk_ring *r) {
    r->bell = 0;
    barrier();          // our clear before reading the writer's head
}

static void *map_page(const char *name, int create) {
    int fd = sys_shm_open(name, create ? PAGE_BYTES : 0, create ? SHM_CREATE | SHM_EXCL : 0);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, PAGE_BYTES, SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    return p == (void *)-1 ? 0 : p;
}

// --- client ----------------------------------------------------------

int ubulk_client_open(struct ubulk_client *c, const char *service) {
    memset(c, 0, sizeof *c);
    if (uchan_client_open(&c->ch, service) != 0) return -1;
    snprintf(c->name, sizeof c->name, "%sb", c->ch.ring_name);
    sys_shm_unlink(c->name);   // a previous holder of this pid
    c->pg = map_page(c->name, 1);
    if (!c->pg) {
        uchan_client_close(&c->ch);
        return -1;
    }
    c->pg->version = 1;
    c->pg->client_pid = sys_getpid();
    c->pg->server_pid = c->ch.beacon->server_pid;
    sys_shm_grant(c->name, c->ch.beacon->server_pid);
    c->pg->magic = UBULK_MAGIC;   // last: the server's admission check
    // The first doorbell, so the server adopts this client now rather
    // than at its first record.
    uchan_send(&c->ch, "o", 1);
    return 0;
}

void ubulk_client_close(struct ubulk_client *c) {
    if (c->pg) {
        sys_shm_unlink(c->name);
        sys_munmap(c->pg, PAGE_BYTES);
    }
    uchan_client_close(&c->ch);
    memset(c, 0, sizeof *c);
}

static int client_alive(void *ctx) { return ubulk_client_server_alive(ctx); }

int ubulk_send(struct ubulk_client *c, uint32_t type, uint32_t id, const void *data, uint32_t len,
               int timeout_ms) {
    if (!c->pg) return -1;
    if (ring_put_wait(&c->pg->up, type, id, data, len, timeout_ms, client_alive, c) != 0) return -1;
    // A doorbell that could not be sent leaves the bell clear, so the
    // next record tries again rather than every later one staying silent.
    if (bell_needed(&c->pg->up) && uchan_send(&c->ch, "b", 1) != 0) c->pg->up.bell = 0;
    return 0;
}

int ubulk_client_recv(struct ubulk_client *c, struct ubulk_msg *m, void *buf, uint32_t cap) {
    if (!c->pg) return 0;
    // The inbox only ever holds doorbells; empty it so it cannot fill.
    char drop[UCHAN_IN_BYTES];
    while (uchan_client_recv(&c->ch, drop, sizeof drop)) {}
    bell_clear(&c->pg->down);
    if (!ring_get(&c->pg->down, m, buf, cap)) return 0;
    m->pid = c->pg->server_pid;
    return 1;
}

void ubulk_client_wait(struct ubulk_client *c, int timeout_ms) {
    if (!c->pg) { sys_sleep_ms(timeout_ms); return; }
    if (c->pg->down.head != c->pg->down.tail) return;
    uchan_client_wait(&c->ch, timeout_ms);
}

int ubulk_client_wait_change(struct ubulk_client *c, uint32_t *seen, int timeout_ms) {
    if (!c->ch.ring) { sys_sleep_ms(timeout_ms); return 0; }
    volatile uint32_t *w = &c->ch.ring->in_wake;
    uint32_t now = *w;
    if (now == *seen) {
        sys_futex_wait(w, now, timeout_ms);
        now = *w;
    }
    int rang = now != *seen;
    *seen = now;
    return rang;
}

void ubulk_client_kick(struct ubulk_client *c) { uchan_client_kick(&c->ch); }

int ubulk_client_server_alive(const struct ubulk_client *c) { return uchan_client_server_alive(&c->ch); }

// --- server ----------------------------------------------------------

static void lock(struct ubulk_server *s) { pthread_mutex_lock(s->lock); }
static void unlock(struct ubulk_server *s) { pthread_mutex_unlock(s->lock); }

int ubulk_server_open(struct ubulk_server *s, const char *service) {
    memset(s, 0, sizeof *s);
    pthread_mutex_t *mu = malloc(sizeof *mu);
    if (!mu) return -1;
    pthread_mutex_init(mu, NULL);
    s->lock = mu;
    if (uchan_server_open(&s->ch, service) != 0) {
        free(mu);
        s->lock = 0;
        return -1;
    }
    return 0;
}

void ubulk_server_close(struct ubulk_server *s) {
    if (!s->lock) return;
    for (int i = 0; i < s->npeers; i++) sys_munmap(s->peer[i].pg, PAGE_BYTES);
    uchan_server_close(&s->ch);
    pthread_mutex_destroy(s->lock);
    free(s->lock);
    memset(s, 0, sizeof *s);
}

static int peer_index(struct ubulk_server *s, int pid) {
    for (int i = 0; i < s->npeers; i++) if (s->peer[i].pid == pid) return i;
    return -1;
}

int ubulk_server_poll(struct ubulk_server *s, int *gone, int gone_cap) {
    if (!s->lock) return 0;
    lock(s);
    int ngone = uchan_server_scan(&s->ch, gone, gone_cap);
    // Doorbells carry nothing; draining them keeps the uchan rings empty.
    char msg[UCHAN_SLOT_BYTES];
    while (uchan_server_recv(&s->ch, msg, sizeof msg) > 0) {}
    // Every uchan client is a ubulk client: adopt the ones not yet
    // mapped, and drop the ones uchan no longer has.
    for (int k = 0; k < s->ch.count; k++) {
        int pid = s->ch.pid[k];
        if (peer_index(s, pid) >= 0 || s->npeers >= UBULK_PEERS) continue;
        char nm[40];
        snprintf(nm, sizeof nm, "%s.%db", s->ch.name, pid);
        struct ubulk_page *pg = map_page(nm, 0);
        if (!pg) continue;
        if (pg->magic != UBULK_MAGIC || pg->client_pid != pid) {
            sys_munmap(pg, PAGE_BYTES);
            continue;
        }
        s->peer[s->npeers++] = (struct ubulk_peer){ pid, pg };
    }
    for (int i = 0; i < s->npeers;) {
        int live = 0;
        for (int k = 0; k < s->ch.count; k++) live |= s->ch.pid[k] == s->peer[i].pid;
        if (live) { i++; continue; }
        sys_munmap(s->peer[i].pg, PAGE_BYTES);
        s->peer[i] = s->peer[--s->npeers];
    }
    for (int i = 0; i < s->npeers; i++) bell_clear(&s->peer[i].pg->up);
    unlock(s);
    return ngone;
}

int ubulk_server_recv(struct ubulk_server *s, struct ubulk_msg *m, void *buf, uint32_t cap) {
    if (!s->lock) return 0;
    lock(s);
    int got = 0;
    for (int i = 0; i < s->npeers && !got; i++) {
        if (ring_get(&s->peer[i].pg->up, m, buf, cap)) {
            m->pid = s->peer[i].pid;
            got = 1;
        }
    }
    unlock(s);
    return got;
}

struct send_ctx { struct ubulk_server *s; int pid; };

// Called with the lock dropped: is `pid` still a client? Takes the lock
// to look, and leaves it taken for the next try.
static int server_peer_alive(void *ctx) {
    struct send_ctx *x = ctx;
    unlock(x->s);
    char nm[40];
    snprintf(nm, sizeof nm, "%s.%db", x->s->ch.name, x->pid);
    int fd = sys_shm_open(nm, 0, 0);
    if (fd >= 0) sys_close(fd);
    lock(x->s);
    return fd >= 0 && peer_index(x->s, x->pid) >= 0;
}

int ubulk_server_send(struct ubulk_server *s, int pid, uint32_t type, uint32_t id, const void *data,
                      uint32_t len, int timeout_ms) {
    if (!s->lock || len > UBULK_REC_MAX) return -1;
    lock(s);
    unsigned long long until = sys_monotonic_ns() + (unsigned long long)timeout_ms * 1000000ull;
    struct send_ctx x = { s, pid };
    int r = -1;
    for (;;) {
        // Looked up afresh each time round: a poll may have dropped and
        // unmapped the page while the lock was down.
        int i = peer_index(s, pid);
        if (i < 0) break;
        struct ubulk_ring *ring = &s->peer[i].pg->down;
        uint32_t seen = ring->space;
        if (ring_put(ring, type, id, data, len)) {
            if (bell_needed(ring) && uchan_server_send(&s->ch, pid, "b", 1) != 0) ring->bell = 0;
            r = 0;
            break;
        }
        if (sys_monotonic_ns() >= until) break;
        unlock(s);
        sys_futex_wait(&ring->space, seen, 20);
        lock(s);
        if (!server_peer_alive(&x)) break;
    }
    unlock(s);
    return r;
}

int ubulk_server_wait_change(struct ubulk_server *s, int timeout_ms) {
    if (!s->ch.beacon) { sys_sleep_ms(timeout_ms); return 0; }
    uint32_t now = s->ch.beacon->wake;
    if (now == s->seen) {
        sys_futex_wait(&s->ch.beacon->wake, now, timeout_ms);
        now = s->ch.beacon->wake;
    }
    int rang = now != s->seen;
    s->seen = now;
    return rang;
}
