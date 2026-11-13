// TFS3 against every crash a reordering disk could leave: CrashMonkey's
// shape (and dm-log-writes'), sized to a 4 MiB RAM volume.
//
// A recording device logs every write and every flush while a workload
// runs generations of creates, appends past the direct pointers, in-place
// overwrites, truncates, deletes and a rename-over, each closed by a
// sync. Then, for each flush point, the crash states the block-layer
// model allows are built and mounted:
//
//   - everything written before a COMPLETED flush is on the medium;
//   - of the writes after it, ANY subset may be (a disk reorders freely
//     between barriers), and a multi-sector write may be torn at sector
//     granularity.
//
// Each state must mount (replaying its journal), fsck may find leaked
// blocks and wrong free counts and NOTHING else, and every file must hold
// a version at least as new as the last sync whose flushes are all inside
// the state -- block by block, so an unsynced in-place overwrite may mix
// its old and new blocks (POSIX promises nothing finer), but a rolled-back,
// stale, zeroed or foreign block fails. Every version's bytes are unique
// to (file, version, offset), and never zero.
//
// WHAT THIS DOES NOT MODEL: a crash DURING replay, and the drivers' flush
// (the device here is RAM and honest). docs/tools and the audit's notes
// say where those are covered.
#include "ktest.h"
#include "tfs3.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "mount.h"
#include "fs.h"
#include "kfmt.h"
#include "storage_config.h"
#include "scheduler.h"

#define CR_SECTORS 8192u                  // 4 MiB, tfs3_test.c's size
#define CR_BYTES   (CR_SECTORS * 512u)
#define CR_POINT   "/var/tmp/.ktest_crashmnt"
#define CR_BLOCK   4096u

// ---- the recording device ----------------------------------------------

struct cr_rec { uint64_t lba; uint32_t n; uint8_t *data; };   // n == 0: a flush

static uint8_t *g_img;           // what the device holds now
static int g_recording;
static struct cr_rec *g_log;
static uint32_t g_nlog, g_cap, g_nflush;
static int g_overflow;

static uint64_t cr_count(const struct block_device *self) { (void)self; return CR_SECTORS; }
static int cr_xfer(const struct block_device *self) { (void)self; return 8; }

static int cr_read(const struct block_device *self, uint64_t lba, int n, void *b) {
    (void)self;
    if (!g_img || n <= 0 || lba + (uint64_t)n > CR_SECTORS) return 0;
    k_memcpy(b, g_img + lba * 512, (uint32_t)n * 512);
    return 1;
}

// THE LOG'S ORDER IS THE MEDIUM'S ORDER: the copy into the image and the
// append happen under one preemption guard, so the commit thread's writes
// and the workload's interleave in the log exactly as they landed.
static int cr_write(const struct block_device *self, uint64_t lba, int n, const void *b) {
    (void)self;
    if (!g_img || n <= 0 || lba + (uint64_t)n > CR_SECTORS) return 0;
    uint8_t *copy = 0;
    if (g_recording) {
        copy = kmalloc((uint32_t)n * 512);
        if (copy) k_memcpy(copy, b, (uint32_t)n * 512);
    }
    int kept = 0;
    scheduler_preempt_disable();
    k_memcpy(g_img + lba * 512, b, (uint32_t)n * 512);
    if (g_recording) {
        if (copy && g_nlog < g_cap) {
            g_log[g_nlog++] = (struct cr_rec){ lba, (uint32_t)n, copy };
            kept = 1;
        } else {
            g_overflow = 1;
        }
    }
    scheduler_preempt_enable();
    if (copy && !kept) kfree(copy);
    return 1;
}

static int cr_flush(const struct block_device *self) {
    (void)self;
    scheduler_preempt_disable();
    if (g_recording) {
        if (g_nlog < g_cap) { g_log[g_nlog++] = (struct cr_rec){ 0, 0, 0 }; g_nflush++; }
        else g_overflow = 1;
    }
    scheduler_preempt_enable();
    return 1;
}

static const struct block_device CR_DEV = {
    .name = "t3crash", .sector_count = cr_count, .read_sectors = cr_read,
    .write_sectors = cr_write, .max_sectors_per_xfer = cr_xfer,
    .caps = BLK_CAP_FLUSH, .flush = cr_flush,
};

// ---- the model -----------------------------------------------------------

enum { F_A, F_B, F_C, F_E, F_F, F_G, F_H, F_TMP, NFILES };
static const char *const g_names[NFILES] = {
    CR_POINT "/a", CR_POINT "/b", CR_POINT "/c", CR_POINT "/d/e",
    CR_POINT "/f", CR_POINT "/g", CR_POINT "/h", CR_POINT "/tmp",
};

#define CR_MAXVER 16
#define CR_MAXOPS 64
#define CR_MAXFILE (32u * CR_BLOCK)

// `from`: the size before a GROWING write, which a crash inside it may
// leave anywhere between (a write commits in steps); `size` otherwise.
struct cr_ver { int op; int present; uint32_t size, from; uint8_t *bytes; };
static struct cr_ver g_ver[NFILES][CR_MAXVER];
static int g_nver[NFILES];
// The flushes the log held when each op RETURNED, and whether the op is a
// durability point (a sync; every op under strict).
static uint32_t g_op_nflush[CR_MAXOPS];
static int g_op_durable[CR_MAXOPS];
static int g_nops;
static uint32_t g_verid;
static int g_model_bad;   // an op failed or a table filled: the run measures nothing
// The rename-over: the sync that made the temp file durable, and the
// version of `a` the rename produced.
static int g_rename_sync = -1, g_rename_ver = -1;

static uint8_t gen(int f, uint32_t v, uint32_t off) {
    uint32_t x = (uint32_t)(f + 1) * 0x9E3779B1u ^ (v + 1) * 0x85EBCA77u ^
                 ((off >> 12) + 1) * 0xC2B2AE3Du;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= (off & 4095u) * 0x297A2D39u;
    x ^= x >> 13;
    return (uint8_t)(1u + x % 255u);
}

static const struct cr_ver *latest(int f) { return g_nver[f] ? &g_ver[f][g_nver[f] - 1] : 0; }

static void push(int f, int present, uint32_t size, uint32_t from, uint8_t *bytes) {
    if (g_nver[f] >= CR_MAXVER) { g_model_bad = 1; kfree(bytes); return; }
    g_ver[f][g_nver[f]++] = (struct cr_ver){ g_nops, present, size, from, bytes };
}

static void op_done(int ok, int sync) {
    if (!ok || g_nops >= CR_MAXOPS) { g_model_bad = 1; return; }
    g_op_nflush[g_nops] = g_nflush;
    g_op_durable[g_nops] = sync || !storage_sync_batched();
    g_nops++;
}

static void m_write(int f, uint32_t off, uint32_t len) {
    const struct cr_ver *v = latest(f);
    uint32_t old = v && v->present ? v->size : 0, size = off + len > old ? off + len : old;
    uint8_t *b = size ? kmalloc(size) : 0;
    if (size && !b) { g_model_bad = 1; return; }
    if (old) k_memcpy(b, v->bytes, old);
    if (off > old) k_memset(b + old, 0, off - old);
    uint32_t id = g_verid++;
    for (uint32_t i = 0; i < len; i++) b[off + i] = gen(f, id, off + i);
    int ok = fs_write_range(g_names[f], off, b + off, len) == 1;
    push(f, 1, size, old, b);
    op_done(ok, 0);
}

static void m_truncate(int f, uint32_t size) {
    const struct cr_ver *v = latest(f);
    uint8_t *b = size ? kmalloc(size) : 0;
    if (size && (!b || !v || !v->present || size > v->size)) { g_model_bad = 1; kfree(b); return; }
    if (size) k_memcpy(b, v->bytes, size);
    int ok = fs_truncate(g_names[f], size) == 1;
    push(f, 1, size, size, b);
    op_done(ok, 0);
}

static void m_delete(int f) {
    int ok = fs_delete(g_names[f]) == 1;
    push(f, 0, 0, 0, 0);
    op_done(ok, 0);
}

static void m_rename_replace(int from, int to) {
    const struct cr_ver *v = latest(from);
    uint8_t *b = v && v->present && v->size ? kmalloc(v->size) : 0;
    if (!v || !v->present || (v->size && !b)) { g_model_bad = 1; kfree(b); return; }
    if (v->size) k_memcpy(b, v->bytes, v->size);
    int ok = fs_rename_replace(g_names[from], g_names[to]) == 1;
    g_rename_ver = g_nver[to];
    push(to, 1, v->size, v->size, b);
    push(from, 0, 0, 0, 0);
    op_done(ok, 0);
}

static void m_sync(void) { op_done(fs_sync_path(CR_POINT) == 1, 1); }

// Generations, each closed by a sync, then a tail no sync covers. Sizes
// cross the 12 direct pointers (an indirect table), and a delete is
// followed by allocations that can take the freed blocks -- the reuse a
// commit's ordering must survive.
static void workload(void) {
    m_write(F_A, 0, 3 * CR_BLOCK);
    m_write(F_B, 0, 14 * CR_BLOCK);
    m_write(F_C, 0, CR_BLOCK);
    m_write(F_E, 0, 2 * CR_BLOCK);
    m_sync();

    m_write(F_A, 3 * CR_BLOCK, 13 * CR_BLOCK);     // a grows to 16: its indirect table
    m_write(F_B, 2 * CR_BLOCK, 4 * CR_BLOCK);      // in place
    m_delete(F_C);
    m_write(F_F, 0, 5 * CR_BLOCK);                 // may take c's block
    m_truncate(F_E, 1000);
    m_sync();

    m_write(F_TMP, 0, 6 * CR_BLOCK);
    m_sync();
    g_rename_sync = g_nops - 1;
    m_rename_replace(F_TMP, F_A);                  // write-then-rename, an updater's shape
    m_delete(F_B);
    m_write(F_G, 0, 20 * CR_BLOCK);                // takes b's blocks
    m_truncate(F_F, 0);
    m_write(F_F, 0, 2 * CR_BLOCK);
    m_sync();

    m_write(F_C, 0, CR_BLOCK);
    m_sync();
    m_truncate(F_C, 300);
    m_sync();
    m_write(F_C, 300, 700);                        // grows inside its one block: allocates nothing
    m_sync();

    m_write(F_E, 1000, 3 * CR_BLOCK);              // appends from mid-block
    m_write(F_H, 0, CR_BLOCK);
    m_delete(F_G);
    m_write(F_A, CR_BLOCK, CR_BLOCK);              // in place, never synced
}

// ---- checking one crash state ---------------------------------------------

static uint8_t g_rd[CR_MAXFILE];
static int g_logged;
#define CR_LOG_MAX 12

static void say(const char *mode, uint32_t e, const char *variant, const char *what, const char *detail) {
    if (g_logged++ >= CR_LOG_MAX) return;
    klog_printf("tfs3crash: %s crash after flush %u (%s): %s %s\n", mode, e, variant, what, detail);
}

// The newest durability point whose flushes are all inside the state.
static int honoured(uint32_t e) {
    int h = -1;
    for (int k = 0; k < g_nops; k++)
        if (g_op_durable[k] && g_op_nflush[k] <= e) h = k;
    return h;
}

// Index of the first allowed version of f: the one in effect at op h.
static int first_allowed(int f, int h) {
    int first = -1;   // -1: the initial state, absent
    for (int i = 0; i < g_nver[f]; i++)
        if (g_ver[f][i].op <= h) first = i;
    return first;
}

// Against the versions from `first` on (-1: from the initial absence).
static int file_ok_from(int f, int first, const char **why) {
    int exists = fs_exists(g_names[f]);
    if (!exists) {
        if (first < 0) return 1;
        for (int i = first; i < g_nver[f]; i++)
            if (!g_ver[f][i].present) return 1;
        *why = "is missing";
        return 0;
    }
    uint64_t size = fs_size(g_names[f]);
    if (size > CR_MAXFILE) { *why = "is larger than any version"; return 0; }
    // A version's own size, or -- for a growing write after the version in
    // effect, which a crash may catch part-way -- any size it passed through.
    int size_ok = 0;
    for (int i = first < 0 ? 0 : first; i < g_nver[f]; i++) {
        const struct cr_ver *v = &g_ver[f][i];
        if (!v->present) continue;
        if (v->size == size) size_ok = 1;
        if (i > first && v->from <= size && size < v->size) size_ok = 1;
    }
    if (!size_ok) { *why = "has a size no allowed version has"; return 0; }
    if (size && fs_read_range(g_names[f], 0, g_rd, (uint32_t)size) != size) {
        *why = "cannot be read whole";
        return 0;
    }
    // BY SECTOR: an unsynced in-place write may tear inside a block.
    for (uint32_t off = 0; off < size; off += 512) {
        uint32_t n = size - off < 512 ? (uint32_t)size - off : 512;
        int match = 0;
        for (int i = first < 0 ? 0 : first; i < g_nver[f] && !match; i++) {
            const struct cr_ver *v = &g_ver[f][i];
            match = v->present && v->size >= off + n && k_memcmp(g_rd + off, v->bytes + off, n) == 0;
        }
        if (!match) {
            static char w[128];
            int zero = 1;
            for (uint32_t i = 0; i < n; i++) zero &= g_rd[off + i] == 0;
            k_snprintf(w, sizeof w, "has a sector from no allowed version (size %u, at %u, %s)",
                       (uint32_t)size, off, zero ? "zeros" : "nonzero");
            *why = w;
            return 0;
        }
    }
    return 1;
}

static int file_ok(int f, int h, const char **why) { return file_ok_from(f, first_allowed(f, h), why); }

static int state_ok(const char *mode, uint32_t e, const char *variant) {
    const char *why = 0;
    if (!mount_add(&CR_DEV, "tfs3", CR_POINT, 0, 0, &why)) {
        say(mode, e, variant, "the volume does not mount:", why ? why : "");
        return 0;
    }
    int ok = 1;
    struct fs_check_result r;
    k_memset(&r, 0, sizeof r);
    if (fs_check_at(CR_POINT, 0, &r) != 0) {
        say(mode, e, variant, "fsck could not run", "");
        ok = 0;
    } else {
        // Leaks and free counts are the price of the leak rule; anything
        // else is damage.
        uint32_t damage = r.referenced_but_free + r.double_allocated + r.out_of_range +
                          r.links_wrong + r.bad_names + r.dir_errors;
        if (damage) {
            char d[96];
            k_snprintf(d, sizeof d, "refd-free %u double %u range %u links %u names %u dirs %u",
                       r.referenced_but_free, r.double_allocated, r.out_of_range,
                       r.links_wrong, r.bad_names, r.dir_errors);
            say(mode, e, variant, "fsck finds damage:", d);
            ok = 0;
        }
    }
    int h = honoured(e);
    for (int f = 0; f < NFILES; f++) {
        const char *fw = 0;
        if (!file_ok(f, h, &fw)) { say(mode, e, variant, g_names[f], fw); ok = 0; }
    }
    // A synced temp file renamed over `a` survives under one of the names.
    // (`a` as the rename made it, or anything later -- the tail rewrites it.)
    const char *rw = 0;
    if (g_rename_sync >= 0 && h >= g_rename_sync && g_rename_ver >= 0 &&
        !fs_exists(g_names[F_TMP]) && !file_ok_from(F_A, g_rename_ver, &rw)) {
        say(mode, e, variant, "the renamed file is under neither name:", rw);
        ok = 0;
    }
    if (!mount_remove(CR_POINT, &why)) {
        say(mode, e, variant, "unmount refused:", why ? why : "");
        ok = 0;
    }
    return ok;
}

// ---- enumerating the states ------------------------------------------------

static uint8_t *g_base, *g_prefix, *g_work;
static uint32_t g_lcg;
static uint32_t rnd(void) { g_lcg = g_lcg * 1103515245u + 12345u; return g_lcg >> 8; }

static void apply(uint8_t *img, const struct cr_rec *w, uint32_t from_s, uint32_t to_s) {
    k_memcpy(img + (w->lba + from_s) * 512, w->data + from_s * 512, (to_s - from_s) * 512);
}

// The writes of one epoch, by log index; `pick` says which persist. A torn
// write (`torn` its index into idx, or -1) persists its first half only.
static int try_state(const char *mode, uint32_t e, const char *variant,
                     const uint32_t *idx, uint32_t m, const uint8_t *pick, int torn) {
    k_memcpy(g_work, g_prefix, CR_BYTES);
    for (uint32_t i = 0; i < m; i++) {
        if (!pick[i]) continue;
        const struct cr_rec *w = &g_log[idx[i]];
        if ((int)i == torn) apply(g_work, w, 0, w->n / 2);
        else apply(g_work, w, 0, w->n);
    }
    g_img = g_work;
    return state_ok(mode, e, variant);
}

#define CR_SAMPLE 4   // per kind of variant, per epoch

static int explore(const char *mode, uint32_t *states) {
    int bad = 0;
    uint32_t *idx = kmalloc(g_nlog * sizeof *idx + 1);
    uint8_t *pick = kmalloc(g_nlog + 1);
    if (!idx || !pick) { kfree(idx); kfree(pick); return -1; }
    k_memcpy(g_prefix, g_base, CR_BYTES);
    uint32_t pos = 0;
    for (uint32_t e = 0; e <= g_nflush; e++) {
        // Epoch e: the writes after the e-th flush, up to the next one.
        uint32_t m = 0, end = pos;
        while (end < g_nlog && g_log[end].n) idx[m++] = end++;
        // none, all
        k_memset(pick, 0, m);
        bad += !try_state(mode, e, "none", idx, m, pick, -1); (*states)++;
        if (m) {
            k_memset(pick, 1, m);
            bad += !try_state(mode, e, "all", idx, m, pick, -1); (*states)++;
        }
        for (uint32_t s = 0; m > 1 && s < CR_SAMPLE && s < m - 1; s++) {
            uint32_t k = 1 + s * (m - 1) / CR_SAMPLE;       // a prefix of k writes
            for (uint32_t i = 0; i < m; i++) pick[i] = i < k;
            bad += !try_state(mode, e, "prefix", idx, m, pick, -1); (*states)++;
            uint32_t one = s * m / CR_SAMPLE;               // one write alone, and all but it
            for (uint32_t i = 0; i < m; i++) pick[i] = i == one;
            bad += !try_state(mode, e, "one", idx, m, pick, -1); (*states)++;
            for (uint32_t i = 0; i < m; i++) pick[i] = i != one;
            bad += !try_state(mode, e, "all-but-one", idx, m, pick, -1); (*states)++;
        }
        for (int s = 0; m > 2 && s < 2; s++) {
            for (uint32_t i = 0; i < m; i++) pick[i] = (uint8_t)(rnd() & 1);
            bad += !try_state(mode, e, "subset", idx, m, pick, -1); (*states)++;
        }
        for (uint32_t i = 0, torn = 0; i < m && torn < 2; i++) {
            if (g_log[idx[i]].n < 2) continue;
            k_memset(pick, 1, m);
            bad += !try_state(mode, e, "torn", idx, m, pick, (int)i); (*states)++;
            torn++;
        }
        // Into the next epoch: this one's writes, then its flush, are durable.
        for (uint32_t i = 0; i < m; i++) apply(g_prefix, &g_log[idx[i]], 0, g_log[idx[i]].n);
        pos = end + 1;
    }
    kfree(idx);
    kfree(pick);
    return bad;
}

static void reset_run(void) {
    for (uint32_t i = 0; i < g_nlog; i++) kfree(g_log[i].data);
    g_nlog = g_nflush = 0;
    g_overflow = g_model_bad = 0;
    for (int f = 0; f < NFILES; f++) {
        for (int i = 0; i < g_nver[f]; i++) kfree(g_ver[f][i].bytes);
        g_nver[f] = 0;
    }
    g_nops = 0;
    g_verid = 0;
    g_rename_sync = g_rename_ver = -1;
}

// One mode: format, record the workload, explore. -1: could not run.
static int run_mode(const char *mode, int batched, uint32_t *states) {
    reset_run();
    g_logged = 0;
    k_memset(g_work, 0, CR_BYTES);
    g_img = g_work;
    struct fs_scratch sc;
    int ok = mount_scratch_begin(&tfs3_ops, &sc);
    if (ok) { ok = tfs3_ops.format(sc.st, &CR_DEV); mount_scratch_end(&sc); }
    const char *why = 0;
    if (!ok || !mount_add(&CR_DEV, "tfs3", CR_POINT, 0, 0, &why)) return -1;
    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, batched);
    ok = fs_mkdir(CR_POINT "/d") && fs_sync_path(CR_POINT);
    // The base: a quiet volume, nothing in flight.
    fs_exclusive_begin();
    k_memcpy(g_base, g_img, CR_BYTES);
    g_recording = ok;
    fs_exclusive_end();
    if (ok) workload();
    fs_exclusive_begin();
    g_recording = 0;
    fs_exclusive_end();
    int gone = mount_remove(CR_POINT, &why);
    int bad = -1;
    if (ok && gone && !g_overflow && !g_model_bad) {
        klog_printf("tfs3crash: %s: %u writes, %u flushes, %d ops recorded\n",
                    mode, g_nlog - g_nflush, g_nflush, g_nops);
        bad = explore(mode, states);
    }
    storage_config_set_mode_for_test(was_strict, was_batched);
    return bad;
}

KTEST("tfs3crash", "every crash state a reordering disk allows mounts clean and keeps what was synced") {
    if (!storage_sync_strict()) KTEST_SKIP("storage.sync is lazy: no barriers to test");
    g_cap = 8192;
    g_log = kzalloc(g_cap * sizeof *g_log);
    g_base = kmalloc(CR_BYTES);
    g_prefix = kmalloc(CR_BYTES);
    g_work = kmalloc(CR_BYTES);
    fs_mkdir(CR_POINT);
    int ready = g_log && g_base && g_prefix && g_work;
    uint32_t states = 0;
    g_lcg = 0x5EED;
    g_logged = 0;
    int bad_strict = ready ? run_mode("strict", 0, &states) : -1;
    int bad_batched = ready ? run_mode("batched", 1, &states) : -1;
    klog_printf("tfs3crash: %u crash states, %d bad under strict, %d under batched\n",
                states, bad_strict, bad_batched);
    reset_run();
    fs_delete(CR_POINT);
    g_img = 0;
    kfree(g_log); kfree(g_base); kfree(g_prefix); kfree(g_work);
    g_log = 0;
    if (!ready) KTEST_SKIP("could not allocate the log and three 4 MiB images");
    KTEST_ASSERT(bad_strict >= 0 && bad_batched >= 0);   // the workload ran and was recorded
    KTEST_ASSERT(states > 100);                          // ...into a real number of states
    KTEST_ASSERT_EQ(bad_strict, 0);
    KTEST_ASSERT_EQ(bad_batched, 0);
}
