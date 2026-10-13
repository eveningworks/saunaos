#ifndef ULIB_UBULK_H
#define ULIB_UBULK_H

#include <stdint.h>
#include "lib/uchan.h"

// ubulk -- records of any size, up to UBULK_REC_MAX, between two ring-3
// processes: a uchan channel for the doorbells, the discovery and the
// death of a client, and beside each client's uchan ring ONE MORE shm
// object holding a byte ring each way. What a uchan slot cannot carry --
// a URL, a page title, a response body -- travels here.
//
// The shape is Wayland's (a small message saying "look", the payload in
// shared memory) and Mojo's data pipe (a byte ring per connection). The
// uchan transport underneath is unchanged: the compositor's protocol
// never sees any of this.
//
// **A RECORD IS (type, id, bytes).** `type` is the protocol's, `id`
// names whatever the record is about -- a fetch, a view -- so one
// connection carries many streams without a channel per stream.
//
// **THE DOORBELL RINGS ONCE PER BATCH, not once per record.** A ring's
// `bell` says the reader has been woken and has not drained yet; a
// writer that finds it set adds its record and rings nothing. The reader
// clears it BEFORE draining, so a record added while it drains is either
// drained now or rings again -- with a full barrier on both sides, since
// this is a store followed by a load on each (Dekker's shape, which
// x86's store buffer breaks without one).
//
// **THREADS.** A client is driven by one thread, and so is a server's
// poll; ubulk_server_send() takes the server's lock and may be called
// from any thread (/bin/webnet's fetch workers each write their own
// responses).

#define UBULK_RING_BYTES (256u * 1024u)   // each way; a power of two
#define UBULK_REC_MAX    (64u * 1024u)    // the largest payload
#define UBULK_PEERS      64

struct ubulk_ring {
    volatile uint32_t head;    // bytes ever written -- ONLY the writer writes it
    volatile uint32_t tail;    // bytes ever read -- ONLY the reader writes it
    volatile uint32_t space;   // bumped by the reader after a drain; a full writer parks on it
    volatile uint32_t bell;    // 1: the reader was rung and has not drained since
    uint8_t data[UBULK_RING_BYTES];
};

// The object the CLIENT creates as "<service>.<pid>b" and grants to the
// server. `up` is client to server, `down` the other way.
struct ubulk_page {
    uint32_t magic;
    uint32_t version;
    int32_t  client_pid;
    int32_t  server_pid;
    struct ubulk_ring up;
    struct ubulk_ring down;
};

// A received record, pointing into the caller's buffer.
struct ubulk_msg {
    int      pid;        // the sender (server side); the server's pid on a client
    uint32_t type, id, len;
    const void *data;
};

// --- client ----------------------------------------------------------

struct ubulk_client {
    struct uchan_client ch;
    struct ubulk_page  *pg;
    char                name[32];
};

// Opens `service`'s channel and publishes this process's rings. 0, or -1
// when there is no such server.
int  ubulk_client_open(struct ubulk_client *c, const char *service);
void ubulk_client_close(struct ubulk_client *c);

// Queues one record and rings the server. WAITS for room while the ring
// is full, up to `timeout_ms` and only while the server lives. 0, or -1.
int  ubulk_send(struct ubulk_client *c, uint32_t type, uint32_t id, const void *data,
                uint32_t len, int timeout_ms);

// The next record from the server into `buf` (UBULK_REC_MAX is always
// enough). 1 with `m` filled, 0 when the ring is empty. A record larger
// than `cap` is skipped, never truncated -- a parser handed half a record
// guesses.
int  ubulk_client_recv(struct ubulk_client *c, struct ubulk_msg *m, void *buf, uint32_t cap);

// Parks until the server rings, another thread kicks, or `timeout_ms`.
void ubulk_client_wait(struct ubulk_client *c, int timeout_ms);
// For a WATCHER THREAD beside a loop that parks on something else: parks
// until the server has rung since `*seen` was taken (returning at once if
// it already has), and updates `*seen`. 1 if it rang. Reads nothing, so
// the loop it wakes still owns the ring.
int  ubulk_client_wait_change(struct ubulk_client *c, uint32_t *seen, int timeout_ms);
void ubulk_client_kick(struct ubulk_client *c);
int  ubulk_client_server_alive(const struct ubulk_client *c);

// --- server ----------------------------------------------------------

struct ubulk_peer {
    int pid;
    struct ubulk_page *pg;
};

struct ubulk_server {
    struct uchan_server ch;
    struct ubulk_peer   peer[UBULK_PEERS];
    int                 npeers;
    uint32_t            seen;   // the beacon word, for ubulk_server_wait_change()
    void               *lock;   // a pthread mutex, made at open
};

int  ubulk_server_open(struct ubulk_server *s, const char *service);
void ubulk_server_close(struct ubulk_server *s);

// Adopts new clients and drops departed ones, writing up to `gone_cap`
// departed pids into `gone`; returns how many departed. Then the caller
// drains with ubulk_server_recv() until it returns 0.
int  ubulk_server_poll(struct ubulk_server *s, int *gone, int gone_cap);
int  ubulk_server_recv(struct ubulk_server *s, struct ubulk_msg *m, void *buf, uint32_t cap);

// Queues one record for `pid` and rings it. Any thread. Waits for room
// as ubulk_send() does. 0, or -1 (no such client, gone, or timed out).
int  ubulk_server_send(struct ubulk_server *s, int pid, uint32_t type, uint32_t id,
                       const void *data, uint32_t len, int timeout_ms);

// Parks until a client rings or `timeout_ms`, returning AT ONCE if one
// has rung since the last call -- what a listener thread beside a
// toolkit loop calls before it uapp_post()s. 1 if something rang.
int  ubulk_server_wait_change(struct ubulk_server *s, int timeout_ms);

#endif
