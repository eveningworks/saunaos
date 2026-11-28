// See ec_proto.h.
#include "ec_proto.h"

// Until the status has `bit` set (want 1) or clear (want 0), or the
// bound runs out. The longest wait that came back in time is kept.
static int wait_for(struct ec_io *e, uint8_t bit, int want) {
    uint64_t t0 = e->now_ns();
    for (;;) {
        uint8_t st = e->in(e->cmd);
        uint64_t d = e->now_ns() - t0;
        if (!!(st & bit) == want) {
            if (d > e->longest_wait_ns) e->longest_wait_ns = d;
            return 0;
        }
        if (d >= e->wait_ns) {
            e->timeouts++;
            return -1;
        }
        if (d >= e->busy_ns && e->nap) {
            e->nap();
            e->naps++;
        }
    }
}

// A byte left in the output buffer by an abandoned transaction would be
// taken as the next answer. Read it away first -- a bounded few, as an
// EC never queues more.
static void ec_flush(struct ec_io *e) {
    for (int i = 0; i < 4 && (e->in(e->cmd) & EC_STS_OBF); i++) {
        (void)e->in(e->data);
        e->flushed++;
    }
}

// A command, then each byte after it, waiting for the EC to take each.
static int send(struct ec_io *e, uint8_t cmd) {
    ec_flush(e);
    e->transactions++;
    if (wait_for(e, EC_STS_IBF, 0)) return -1;
    e->out(e->cmd, cmd);
    return 0;
}

static int put(struct ec_io *e, uint8_t v) {
    if (wait_for(e, EC_STS_IBF, 0)) return -1;
    e->out(e->data, v);
    return 0;
}

static int get(struct ec_io *e, uint8_t *v) {
    if (wait_for(e, EC_STS_OBF, 1)) return -1;
    *v = e->in(e->data);
    return 0;
}

int ec_read(struct ec_io *e, uint8_t addr, uint8_t *val) {
    if (send(e, EC_CMD_READ) || put(e, addr)) return -1;
    return get(e, val);
}

int ec_write(struct ec_io *e, uint8_t addr, uint8_t val) {
    if (send(e, EC_CMD_WRITE) || put(e, addr) || put(e, val)) return -1;
    // The value is taken when IBF clears; until then the EC is busy.
    return wait_for(e, EC_STS_IBF, 0);
}

int ec_query(struct ec_io *e, uint8_t *q) {
    *q = 0;
    if (send(e, EC_CMD_QUERY)) return -1;
    return get(e, q);
}
