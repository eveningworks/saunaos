// etc_config's FILE half -- the four entry points that touch fs.h.
//
// Split out of etc_config.c so the `name=value` parser beside it can be
// freestanding and compiled a second time into libuapp.a, which is what
// lets the ring-3 WM read a .desktop file without a second parser to
// drift from this one. Same arrangement as kfmt.c/kfmt_print.c, and for
// the same reason: one file is shareable, the other reaches for kernel
// state, and mixing them makes neither.
//
// If you add an entry point here, ask which half it belongs in: does it
// look at a buffer, or does it look at a file?
#include "etc_config.h"
#include "fs.h"
#include "string.h"
#include "heap.h"
#include "irqflags.h"
#include "kfmt.h"   // klog_printf
#include "klog.h"   // KLOG_WARN

const struct etc_config_mem etc_config_kmem = { kmalloc, kfree };

// A per-key read: a load, one parse, a free. The document is the only
// allocation; a failed one reads as "no such file", which is what a
// caller already has to handle.
int etc_config_get(const char *path, const char *key, char *out, uint32_t out_size) {
    return etc_config_get_in(path, 0, key, out, out_size);
}

int etc_config_get_in(const char *path, const char *section, const char *key,
                      char *out, uint32_t out_size) {
    struct etc_config_buf buf;
    int ok = 0;
    if (etc_config_load(path, &buf)) {
        ok = etc_config_buf_get_in(&buf, section, key, out, out_size);
    } else if (out && out_size) {
        out[0] = '\0';
    }
    etc_config_buf_free(&buf);
    return ok;
}

// AN UNCHANGED FILE IS ANSWERED FROM MEMORY. Every setting's `stored`
// value is a per-key read of its file, so a client asking about settings
// re-read /etc/saunaos.conf over a thousand times a session. Slots are keyed
// by the path and fs_generation_of(path) SAMPLED BEFORE THE READ: a write
// landing during it moves the generation, and the next ask refuses the
// entry. A write through a hard link is not seen (lib/uconf.c's blind
// spot too). Only successful reads are kept: a failed one may be an I/O
// error rather than an absent file. irq_save() is the lock, never held
// across the read -- nor across kmalloc/kfree, so a hit sizes its copy
// under the lock, allocates outside it, and copies only if the slot is
// still the one it sized (`serial`). One core (smp-design.md).
//
// EACH SLOT HOLDS ITS DOCUMENT AT ITS OWN SIZE, under a TOTAL-BYTES cap:
// eight slots of a 1 MiB file each would be 8 MiB of kernel heap. A new
// document evicts the least recently used until it fits; one bigger
// than the whole cap is not kept. Either is logged the first time.
#define CACHE_SLOTS 8
#define CACHE_PATH_MAX 64
#define CACHE_CAP_DEFAULT (256u * 1024u)
struct cache_slot {
    char path[CACHE_PATH_MAX];      // "" = empty
    uint64_t gen, stamp, serial;
    uint32_t size;
    char *data;                     // kmalloc'd at `size`; NULL = empty
};
static struct cache_slot g_slots[CACHE_SLOTS];
static uint64_t g_stamp, g_serial, g_hits, g_cap_hits;
static uint32_t g_bytes, g_cap = CACHE_CAP_DEFAULT, g_cap_logs;

uint64_t etc_config_cache_hits(void) { return g_hits; }
uint64_t etc_config_cache_cap_hits(void) { return g_cap_hits; }
uint32_t etc_config_cache_cap_logs(void) { return g_cap_logs; }

uint32_t etc_config_cache_set_cap(uint32_t bytes) {
    uint64_t f = irq_save();
    uint32_t was = g_cap;
    g_cap = bytes;
    irq_restore(f);
    return was;
}

static void cap_hit(const char *path, uint32_t size) {
    uint64_t f = irq_save();
    g_cap_hits++;
    int first = g_cap_logs == 0;
    if (first) g_cap_logs++;          // counts what was SAID: the KTEST holds it at 1
    uint32_t cap = g_cap;
    irq_restore(f);
    if (first)
        klog_printf(KLOG_WARN "config: the load cache is at its %u-byte cap keeping %s "
                    "(%u bytes); older files are dropped and read again (said once)\n",
                    cap, path, size);
}

static int cache_get(const char *path, uint64_t gen, struct etc_config_buf *buf) {
    uint64_t f = irq_save();
    struct cache_slot *c = 0;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (g_slots[i].data && g_slots[i].gen == gen && k_strcmp(g_slots[i].path, path) == 0) {
            c = &g_slots[i];
            break;
        }
    }
    uint64_t serial = c ? c->serial : 0;
    uint32_t size = c ? c->size : 0;
    irq_restore(f);
    if (!c) return 0;

    char *d = kmalloc((size_t)size + 1);
    if (!d) return 0;
    int hit = 0;
    f = irq_save();
    if (c->data && c->serial == serial) {
        k_memcpy(d, c->data, size);
        c->stamp = ++g_stamp;
        g_hits++;
        hit = 1;
    }
    irq_restore(f);
    if (!hit) {             // evicted or replaced meanwhile: read it instead
        kfree(d);
        return 0;
    }
    d[size] = '\0';
    buf->data = d;
    buf->size = size;
    buf->valid = 1;
    buf->mem = &etc_config_kmem;
    return 1;
}

static void cache_put(const char *path, uint64_t gen, const struct etc_config_buf *buf) {
    if (k_strlen(path) >= CACHE_PATH_MAX) return;
    if (buf->size > g_cap) {
        cap_hit(path, buf->size);
        return;
    }
    char *copy = kmalloc(buf->size ? buf->size : 1);
    if (!copy) return;
    k_memcpy(copy, buf->data, buf->size);

    char *drop[CACHE_SLOTS];
    int ndrop = 0, evicted_for_bytes = 0;
    uint64_t f = irq_save();
    // The path's own slot if it has one (an older generation), else an
    // empty one, else the least recently used.
    struct cache_slot *c = 0;
    for (int i = 0; i < CACHE_SLOTS && !c; i++)
        if (g_slots[i].data && k_strcmp(g_slots[i].path, path) == 0) c = &g_slots[i];
    for (int i = 0; i < CACHE_SLOTS && !c; i++)
        if (!g_slots[i].data) c = &g_slots[i];
    if (!c) {
        c = &g_slots[0];
        for (int i = 1; i < CACHE_SLOTS; i++)
            if (g_slots[i].stamp < c->stamp) c = &g_slots[i];
    }
    if (c->data) {
        drop[ndrop++] = c->data;
        g_bytes -= c->size;
        c->data = 0;
    }
    // Then the least recently used others, until the new one fits.
    while (g_bytes + buf->size > g_cap) {
        struct cache_slot *v = 0;
        for (int i = 0; i < CACHE_SLOTS; i++)
            if (g_slots[i].data && (!v || g_slots[i].stamp < v->stamp)) v = &g_slots[i];
        if (!v) break;
        drop[ndrop++] = v->data;
        g_bytes -= v->size;
        v->data = 0;
        v->path[0] = '\0';
        evicted_for_bytes = 1;
    }
    k_strlcpy(c->path, path, sizeof c->path);
    c->data = copy;
    c->size = buf->size;
    c->gen = gen;
    c->stamp = ++g_stamp;
    c->serial = ++g_serial;
    g_bytes += buf->size;
    irq_restore(f);

    for (int i = 0; i < ndrop; i++) kfree(drop[i]);
    if (evicted_for_bytes) cap_hit(path, buf->size);
}

// Refusing a file over the bound is logged once a boot: a settings read
// can ask for the same file thousands of times.
static int g_over_logged;

// AT THE FILE'S SIZE: fs_size(), then a buffer one byte longer (for
// fs_read_into()'s NUL). A file that grew between the two is refused by
// fs_read_into() rather than cut, and asked for again; one that keeps
// growing is refused, as any read that cannot be completed is.
static int read_whole(const char *path, struct etc_config_buf *buf) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t size = fs_size(path);
        if (size == 0) return 0;      // missing, a directory, or empty: no document
        if (size > ETC_CONFIG_FILE_MAX) {
            if (!g_over_logged) {
                g_over_logged = 1;
                klog_printf(KLOG_WARN "config: %s is %llu bytes, over the %u a config "
                            "file may be -- not read (said once)\n", path,
                            (unsigned long long)size, (unsigned)ETC_CONFIG_FILE_MAX);
            }
            return 0;
        }
        char *d = kmalloc((size_t)size + 1);
        if (!d) return 0;
        uint32_t n = fs_read_into(path, d, (uint32_t)size + 1);
        if (n) {
            buf->data = d;
            buf->size = n;
            buf->valid = 1;
            buf->mem = &etc_config_kmem;
            return 1;
        }
        kfree(d);
        if (fs_size(path) <= size) return 0;   // a real failure, not a race
    }
    return 0;
}

int etc_config_load(const char *path, struct etc_config_buf *buf) {
    if (!buf) return 0;
    buf->data = 0;
    buf->size = 0;
    buf->valid = 0;
    buf->mem = 0;
    if (!path) return 0;
    uint64_t gen = fs_generation_of(path);
    if (cache_get(path, gen, buf)) return 1;

    // fs_read_into(), not fs_read() + a copy: the copy would still have
    // to happen after fs_read() returned, and the kernel context can be
    // preempted in that window by a ring-3 process whose own file read
    // swaps the shared staging buffer underneath it.
    if (!read_whole(path, buf)) return 0;
    cache_put(path, gen, buf);
    return 1;
}

// Both of these are now read / rewrite / write, with the rewrite shared
// (etc_config_buf_set). They used to carry a copy of that loop each.
static int rewrite(const char *path, const char *section,
                   const char *key, const char *value) {
    // A missing file is not an error for a SET -- the key is appended to
    // an empty document and the file created. It is for an unset, which
    // etc_config_buf_set_alloc() refuses as "not there".
    //
    // A file that EXISTS but cannot be read whole is not a missing one:
    // rewriting it from an empty document would replace it with one key.
    struct etc_config_buf in;
    if (!etc_config_load(path, &in) && fs_size(path) > 0) return 0;
    uint32_t n = 0;
    char *out = etc_config_buf_set_alloc(in.data, in.size, section, key, value,
                                         &etc_config_kmem, &n);
    // An unset of a present key always writes, even the empty document.
    int rc = out ? fs_write(path, out, 0) : 0;   // a NUL-terminated string
    kfree(out);
    etc_config_buf_free(&in);
    return rc;
}

int etc_config_unset(const char *path, const char *key) {
    return rewrite(path, 0, key, 0);
}

int etc_config_set(const char *path, const char *key, const char *value) {
    return rewrite(path, 0, key, value);
}

int etc_config_unset_in(const char *path, const char *section, const char *key) {
    return rewrite(path, section, key, 0);
}

int etc_config_set_in(const char *path, const char *section,
                      const char *key, const char *value) {
    return rewrite(path, section, key, value);
}
