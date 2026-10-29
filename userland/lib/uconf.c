// See lib/uconf.h.
#include <stdlib.h>
#include "lib/uconf.h"
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>   // snprintf, for the said-once lines

static void *mem_alloc(unsigned long n) { return malloc(n); }
static void mem_free(void *p) { free(p); }
const struct etc_config_mem uconf_mem = { mem_alloc, mem_free };

static void say_once(int *said, const char *fmt, const char *path, unsigned long long a,
                     unsigned long long b) {
    if (*said) return;
    *said = 1;
    char line[200];
    snprintf(line, sizeof line, fmt, path, a, b);
    sys_eprint(line);
}

static int g_over_said;

// THE WHOLE FILE, AT ITS SIZE: fstat() sizes the first allocation, and a
// file that turns out longer (it grew in between) grows the buffer
// rather than being cut. Past ETC_CONFIG_FILE_MAX it is refused -- the
// same contract, and the same reason, as the kernel's fs_read_into():
// half a config file parses as a whole one with keys missing.
static int read_whole(const char *path, struct etc_config_buf *buf) {
    int fd = sys_open(path, 0);
    if (fd < 0) return 0;
    struct sys_stat st;
    uint64_t want = sys_fstat(fd, &st) == 0 ? st.size : 0;
    if (want > ETC_CONFIG_FILE_MAX) {
        sys_close(fd);
        say_once(&g_over_said, "uconf: %s is %llu bytes, over the %llu a config file "
                 "may be -- not read (said once a process)\n", path, want,
                 ETC_CONFIG_FILE_MAX);
        return 0;
    }
    uint32_t cap = (uint32_t)want + 2, total = 0;   // +2: a byte to see growth by, and the NUL
    char *d = malloc(cap);
    for (;;) {
        if (!d) { sys_close(fd); return 0; }
        if (total + 1 >= cap) {
            if (total > ETC_CONFIG_FILE_MAX) {
                free(d);
                sys_close(fd);
                say_once(&g_over_said, "uconf: %s grew past %llu bytes while read, "
                         "over the %llu a config file may be -- not read\n", path,
                         (unsigned long long)total, ETC_CONFIG_FILE_MAX);
                return 0;
            }
            uint32_t ncap = cap * 2 > ETC_CONFIG_FILE_MAX + 2 ? ETC_CONFIG_FILE_MAX + 2 : cap * 2;
            char *g = realloc(d, ncap);
            if (!g) { free(d); d = 0; continue; }
            d = g;
            cap = ncap;
        }
        int64_t n = sys_read(fd, d + total, cap - 1 - total);
        if (n < 0) { free(d); sys_close(fd); return 0; }
        if (n == 0) break;
        total += (uint32_t)n;
    }
    sys_close(fd);
    if (total == 0) { free(d); return 0; }   // empty: no document, as before
    d[total] = '\0';
    buf->data = d;
    buf->size = total;
    buf->valid = 1;
    buf->mem = &uconf_mem;
    return 1;
}

// ---- the per-process cache ---------------------------------------------
//
// A file is re-read only when its fs_generation_of() moved: one syscall
// and a copy instead of an open, the reads and a close -- and a disk read
// each, with no page cache under them. Per key, that was toywm re-reading
// /etc/desktop.conf ~35 times per window opened, and System Settings a
// settings.d file 248 times per launch (the dconf shape: readers keep the
// document and a cheap "is it stale" check). A missing file is cached
// too: creating it moves its generation.
//
// THE GENERATION IS THE PATH'S, NOT THE FILE'S: a write through a HARD
// LINK to the same file moves only the link's, and this keeps answering
// the old text -- the blind spot every fs_generation_of() watcher has.
// The generation is read BEFORE the file, so a write in between is
// cached under the old number and re-read next time, never the reverse.
//
// EACH SLOT HOLDS ITS DOCUMENT AT ITS OWN SIZE, under a total-bytes cap
// (UCONF_CACHE_BYTES): eight 1 MiB files would otherwise be 8 MiB of
// every process that read them. A new document evicts the least
// recently used until it fits, and one over the whole cap is not kept;
// the first time either happens it is said, once a process.
#define UCONF_SLOTS 8
#define UCONF_PATH_MAX 96   // longer paths are simply not cached
#define UCONF_CACHE_BYTES (256u * 1024u)

struct uconf_slot {
    char path[UCONF_PATH_MAX];
    long long gen;          // 0 = empty
    uint32_t stamp;         // LRU: the smallest goes
    uint32_t size;
    int valid;              // 0: the file was missing, and that is cached
    char *data;             // malloc'd at size + 1 when valid
};

static struct uconf_slot g_slots[UCONF_SLOTS];
static uint32_t g_stamp, g_bytes, g_cap = UCONF_CACHE_BYTES;
static unsigned long long g_cap_hits;
static int g_cap_said;               // times the cap was reported: 0 or 1
static volatile int g_lock;          // a process may ask from two threads (webcontent)

static void lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) sys_yield();
}

static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

uint32_t uconf_cache_set_cap(uint32_t bytes) {
    lock();
    uint32_t was = g_cap;
    g_cap = bytes;
    unlock();
    return was;
}

unsigned long long uconf_cache_cap_hits(void) { return g_cap_hits; }
int uconf_cache_cap_said(void) { return g_cap_said; }

static void drop_slot(struct uconf_slot *s) {
    if (s->valid) g_bytes -= s->size;
    free(s->data);
    memset(s, 0, sizeof *s);
}

// Under the lock; the message waits for unlock(), sys_eprint is a syscall.
static int note_cap_hit(void) {
    g_cap_hits++;
    if (g_cap_said) return 0;
    g_cap_said++;           // counts what was SAID: etc_config_test holds it at 1
    return 1;
}

static void say_cap(const char *path, uint32_t size) {
    char line[200];
    snprintf(line, sizeof line, "uconf: the config cache is at its %u-byte cap keeping "
             "%s (%u bytes); older files are dropped and read again (said once)\n",
             (unsigned)g_cap, path, (unsigned)size);
    sys_eprint(line);
}

// 1 = answered from the cache into `buf`.
static int cache_get(const char *path, long long gen, struct etc_config_buf *buf) {
    int hit = 0;
    lock();
    for (int i = 0; i < UCONF_SLOTS; i++) {
        struct uconf_slot *s = &g_slots[i];
        if (s->gen == gen && !strcmp(s->path, path)) {
            // A copy the caller frees: the slot may be dropped while the
            // caller still parses, and the caller may be another thread.
            if (s->valid && !etc_config_buf_copy(buf, s->data, s->size, &uconf_mem)) break;
            s->stamp = ++g_stamp;
            hit = 1;
            break;
        }
    }
    unlock();
    return hit;
}

static void cache_put(const char *path, long long gen, const struct etc_config_buf *buf) {
    uint32_t size = buf->valid ? buf->size : 0;
    char *copy = 0;
    if (buf->valid) {
        if (size > g_cap) {
            lock();
            int say = note_cap_hit();
            unlock();
            if (say) say_cap(path, size);
            return;
        }
        copy = malloc(size + 1);
        if (!copy) return;
        memcpy(copy, buf->data, size);
        copy[size] = '\0';
    }
    int say = 0;
    lock();
    struct uconf_slot *v = 0;
    for (int i = 0; i < UCONF_SLOTS && !v; i++)
        if (g_slots[i].gen && !strcmp(g_slots[i].path, path)) v = &g_slots[i];   // its stale copy
    for (int i = 0; i < UCONF_SLOTS && !v; i++)
        if (!g_slots[i].gen) v = &g_slots[i];
    if (!v) {
        v = &g_slots[0];
        for (int i = 1; i < UCONF_SLOTS; i++)
            if (g_slots[i].stamp < v->stamp) v = &g_slots[i];
    }
    drop_slot(v);
    while (g_bytes + size > g_cap) {
        struct uconf_slot *o = 0;
        for (int i = 0; i < UCONF_SLOTS; i++)
            if (g_slots[i].valid && (!o || g_slots[i].stamp < o->stamp)) o = &g_slots[i];
        if (!o) break;
        drop_slot(o);
        say |= note_cap_hit();
    }
    strlcpy(v->path, path, sizeof v->path);
    v->gen = gen;
    v->stamp = ++g_stamp;
    v->valid = buf->valid;
    v->size = size;
    v->data = copy;
    g_bytes += size;
    unlock();
    if (say) say_cap(path, size);
}

int uconf_load(const char *path, struct etc_config_buf *buf) {
    if (!buf) return 0;
    buf->data = 0;
    buf->size = 0;
    buf->valid = 0;
    buf->mem = 0;
    if (!path) return 0;

    long long gen = strlen(path) < UCONF_PATH_MAX ? sys_fs_generation_of(path) : 0;
    if (gen > 0 && cache_get(path, gen, buf)) return buf->valid;

    read_whole(path, buf);
    if (gen > 0) cache_put(path, gen, buf);
    return buf->valid;
}

// A per-key read: a load, one parse, a free.
int uconf_get_in(const char *path, const char *section, const char *key,
                 char *out, uint32_t out_size) {
    struct etc_config_buf buf;
    int ok = uconf_load(path, &buf);
    int rc = ok ? etc_config_buf_get_in(&buf, section, key, out, out_size) : 0;
    if (!ok && out && out_size) out[0] = '\0';
    etc_config_buf_free(&buf);
    return rc;
}

int uconf_get(const char *path, const char *key, char *out, uint32_t out_size) {
    return uconf_get_in(path, 0, key, out, out_size);
}

int uconf_set_in(const char *path, const char *section,
                 const char *key, const char *value) {
    // A missing file is not an error: the key is appended to an empty
    // document and the file created. Same behaviour as the kernel's
    // etc_config_set(), because it is the same rewriter underneath.
    //
    // A file that EXISTS but could not be read whole is not a missing
    // one: rewriting it from an empty document would replace it with a
    // single key.
    struct etc_config_buf in;
    if (!uconf_load(path, &in)) {
        struct sys_stat st;
        if (sys_stat(path, &st) == 0 && st.size > 0) return 0;
    }

    // An unset of a key that is not there is a refusal (NULL), and so is
    // a section that could not be read back; an unset that empties the
    // file is a document of length 0, and is written.
    uint32_t n = 0;
    char *out = etc_config_buf_set_alloc(in.data, in.size, section, key, value,
                                         &uconf_mem, &n);
    int rc = 0;
    if (!out) goto done;

    // A WRITE THAT CHANGES NOTHING IS NOT PERFORMED, which is what
    // `config` already does for a setting. It stopped being free when
    // /bin/dhcp started running at boot: the nameserver is usually the
    // one already on disk, and every write here is a real filesystem
    // transaction.
    if (in.valid && in.size == n && !memcmp(in.data, out, n)) { rc = 1; goto done; }

    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) goto done;
    int64_t w = sys_write(fd, out, n);
    sys_close(fd);

    // A short write is a failed write. A config file half-rewritten is
    // worse than one not rewritten at all -- it parses, with the tail
    // of the document missing.
    rc = (w == (int64_t)n);

done:
    free(out);
    etc_config_buf_free(&in);
    return rc;
}

int uconf_set(const char *path, const char *key, const char *value) {
    return uconf_set_in(path, 0, key, value);
}

int uconf_unset(const char *path, const char *key) {
    return uconf_set_in(path, 0, key, 0);
}
