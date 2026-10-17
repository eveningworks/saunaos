// Per-process I/O rates -- see uprocio.h.
#include "lib/uprocio.h"
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"

int uprocio_update(struct uprocio *u) {
    // Read into a fresh array, then swap: the previous one is the base.
    int cap = u->cap ? u->cap : 64;
    struct query_procio *next = 0;
    int n;
    for (;;) {
        free(next);
        next = malloc((size_t)cap * sizeof *next);
        if (!next) return -1;
        n = 0;
        struct query_procio r;
        QUERY_FOREACH(QUERY_PROCIO, r, i) {
            if (n == cap) break;
            next[n++] = r;
        }
        // A full array may have cut the list short: grow and read again.
        if (n < cap) break;
        cap *= 2;
    }
    free(u->prev);
    u->prev = u->cur;
    u->nprev = u->ncur;
    u->prev_ns = u->cur_ns;
    u->cur = next;
    u->ncur = n;
    u->cur_ns = sys_monotonic_ns();
    u->cap = cap;
    return n;
}

static uint64_t per_sec(uint64_t now, uint64_t was, uint64_t ns, uint64_t scale) {
    if (!ns || now < was) return 0;
    // 128-bit, so a large count times the scale cannot wrap.
    return (uint64_t)((unsigned __int128)(now - was) * scale * 1000000000u / ns);
}

static void rate_of(const struct uprocio *u, const struct query_procio *c,
                    struct uprocio_rate *out) {
    memset(out, 0, sizeof *out);
    out->pid = c->pid;
    out->total = *c;
    if (!u->prev) return;   // the first read: nothing to compare with
    struct query_procio zero = { 0 };
    const struct query_procio *p = &zero;
    for (int i = 0; i < u->nprev; i++)
        if (u->prev[i].pid == c->pid) { p = &u->prev[i]; break; }
    uint64_t ns = u->cur_ns - u->prev_ns;
    out->read_bps = per_sec(c->disk_read_bytes, p->disk_read_bytes, ns, 1);
    out->write_bps = per_sec(c->disk_write_bytes, p->disk_write_bytes, ns, 1);
    out->read_mops = per_sec(c->disk_reads, p->disk_reads, ns, 1000);
    out->write_mops = per_sec(c->disk_writes, p->disk_writes, ns, 1000);
    out->file_read_bps = per_sec(c->file_read_bytes, p->file_read_bytes, ns, 1);
    out->file_write_bps = per_sec(c->file_write_bytes, p->file_write_bytes, ns, 1);
}

int uprocio_rate(const struct uprocio *u, int pid, struct uprocio_rate *out) {
    for (int i = 0; i < u->ncur; i++)
        if (u->cur[i].pid == pid) { rate_of(u, &u->cur[i], out); return 1; }
    return 0;
}

int uprocio_rate_at(const struct uprocio *u, int i, struct uprocio_rate *out) {
    if (i < 0 || i >= u->ncur) return 0;
    rate_of(u, &u->cur[i], out);
    return 1;
}

void uprocio_free(struct uprocio *u) {
    free(u->prev);
    free(u->cur);
    memset(u, 0, sizeof *u);
}
