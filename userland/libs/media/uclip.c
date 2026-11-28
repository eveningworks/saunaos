// The system clipboard, over the shared page /bin/clipboardd owns --
// see media/uclip.h for the API and base/uclip_page.h for why the kernel
// is not in this any more.
#include "media/uclip.h"
#include "media/uimg.h"
#include "base/uchan.h"   // a large copy asks clipboardd for its object
#include "base/ulog.h"
#include "base/uself.h"   // uproc_path(): root checks who made the page
#include "base/uutf8.h"   // the clipboard is UTF-8: stray bytes become their Latin-1 letter
#include "rt/sys.h"
#include "syscall_abi.h"
#include "query_abi.h"
#include "proc_info.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile struct clip_page *g_page;
static volatile struct clip_page *g_stale;   // the page before, unmapped at the next drop
static int g_fd = -1;
static uint64_t g_checked_ms;
#define RECHECK_MS 1000   // how often the name is asked whether it is still this daemon's

// ROOT'S CHECK of the page it is about to trust: made by /bin/clipboardd,
// started by init, running as the account the page names. A root client
// (remoted) serves whichever session is on, so SHM_USER's "my account"
// cannot be its test. Needs PROMISE_PROC and PROMISE_QUERY, which only an
// unsandboxed root process keeps -- every root client today.
static int root_trusted_creator(void) {
    struct query_shm r;
    QUERY_FOREACH(QUERY_SHM, r, i) {
        if (strcmp(r.name, CLIP_SHM_NAME) || (r.flags & QUERY_SHM_UNLINKED)) continue;
        if (!(r.flags & QUERY_SHM_ACCOUNT)) return 0;
        struct proc_info pi;
        for (int s = 0; sys_proc_info(s, &pi) == 0; s++) {
            if (pi.pid != r.creator_pid || pi.tgid != pi.pid) continue;
            char exe[64];
            if (pi.ppid != 1 || pi.euid != r.uid || uproc_path(pi.pid, exe, sizeof exe) < 0 ||
                strcmp(exe, "/bin/clipboardd"))
                return 0;
            return r.creator_pid;
        }
        return 0;
    }
    return 0;
}

// Is the page we hold still the live one? Its magic goes when clipboardd
// stops cleanly; its name goes when it is killed outright, which the
// magic never shows -- so that is asked too, once a RECHECK_MS.
static int page_live(volatile struct clip_page *pg) {
    if (pg->magic != CLIP_MAGIC) return 0;
    uint64_t now = sys_monotonic_ns() / 1000000ull;
    if (now - g_checked_ms < RECHECK_MS) return 1;
    g_checked_ms = now;
    int fd = sys_shm_open_from(CLIP_SHM_NAME, (int)pg->daemon_pid);
    if (fd < 0) return 0;
    sys_close(fd);
    return 1;
}

// Maps the clipboard, once per clipboardd. Returns NULL when the service
// is not running, or the name is not this account's clipboard, which is
// an ordinary answer and not an error: every caller here degrades to
// "the clipboard is empty" or "the copy did not happen".
//
// THE NAME IS CHECKED, NOT TRUSTED: between one session's clipboardd and
// the next, another account could take it first. A non-root client opens
// with SHM_USER, which the kernel refuses unless the page is its own
// account's; root checks who made it (root_trusted_creator()).
//
// A PAGE THAT ENDED is let go of (page_live()), so a process that outlives
// the session (root's remoted) maps the next one rather than keep the last
// person's. Its mapping is unmapped one page LATER: another thread of this
// process may be reading it this moment, and its bytes are wiped.
static volatile struct clip_page *page(void) {
    if (g_page && page_live(g_page)) return g_page;
    if (g_page) {
        if (g_stale) sys_munmap((void *)g_stale, sizeof(struct clip_page));
        g_stale = g_page;
        g_page = 0;
        sys_close(g_fd);
        g_fd = -1;
    }
    if (geteuid() == 0) {
        int pid = root_trusted_creator();
        g_fd = pid > 0 ? sys_shm_open_from(CLIP_SHM_NAME, pid) : -1;
    } else {
        g_fd = sys_shm_open(CLIP_SHM_NAME, 0, SHM_USER);
    }
    if (g_fd < 0) return 0;
    void *p = sys_mmap(0, sizeof(struct clip_page),
                        SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, g_fd, 0);
    if (p == (void *)-1) { sys_close(g_fd); g_fd = -1; return 0; }
    volatile struct clip_page *pg = (volatile struct clip_page *)p;
    if (pg->magic != CLIP_MAGIC) {
        // The daemon is mid-initialisation. Not cached, so the next
        // call looks again rather than remembering a page that was not
        // ready the first time anybody asked.
        sys_munmap(p, sizeof(struct clip_page));
        sys_close(g_fd);
        g_fd = -1;
        return 0;
    }
    g_page = pg;
    g_checked_ms = sys_monotonic_ns() / 1000000ull;
    return g_page;
}

static unsigned long pages_of(uint64_t len) { return (unsigned long)((len + 4095) & ~4095ull); }

// Object `n`, mapped: read-only for a paster, read-write for the copier
// filling it. NULL when it is gone (replaced and unlinked meanwhile) or
// smaller than `len` says, which the kernel's mmap refuses.
static void *map_object(uint32_t n, uint64_t len, int writable) {
    char name[SHM_NAME_MAX];
    snprintf(name, sizeof name, CLIP_OBJECT_FMT, (unsigned)n);
    int fd = sys_shm_open(name, 0, 0);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, pages_of(len), SYS_PROT_READ | (writable ? SYS_PROT_WRITE : 0),
                        SYS_MAP_SHARED, fd, 0);
    sys_close(fd);   // the mapping holds the object now
    return p == (void *)-1 ? 0 : p;
}

// --- reading ----------------------------------------------------------

void uclip_free(struct uclip *c) {
    if (!c) return;
    if (c->own) {
        if (c->own_map) sys_munmap(c->own, c->own_map);
        else free(c->own);
    }
    memset(c, 0, sizeof *c);
    c->data = "";
}

// One consistent read of a slot's header -- the clipboard's, or with
// `drag` the drag slot's -- under the seqlock, an inline payload copied
// into `inl` (`inl_cap` at most). 1 when settled; 0 when a writer kept
// the page moving.
struct head { uint32_t op, kind, count, len, serial, where, object; };

static int read_head(volatile struct clip_page *pg, int drag, struct head *h,
                     char *inl, uint32_t inl_cap) {
    // THE SEQLOCK. Odd means a writer is inside; a change across the
    // copy means it landed while we were reading. Bounded, because a
    // writer that died mid-update would otherwise spin us forever --
    // and the daemon repairs that within a second or two.
    for (int tries = 0; tries < 64; tries++) {
        uint32_t s1 = __atomic_load_n(&pg->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1u) continue;
        if (drag) {
            h->op = pg->drag_count ? CLIP_OP_COPY : CLIP_OP_NONE;
            h->kind = pg->drag_kind;
            h->count = pg->drag_count;
            h->len = pg->drag_len;
            h->serial = pg->drag_serial;
            h->where = pg->drag_where;
            h->object = pg->drag_object;
        } else {
            h->op = pg->op;
            h->kind = pg->kind;
            h->count = pg->count;
            h->len = pg->len;
            h->serial = pg->serial;
            h->where = pg->where;
            h->object = pg->object;
        }
        if (h->where == CLIP_INLINE && inl) {
            if (h->len > inl_cap) h->len = inl_cap;
            const volatile char *src = drag ? pg->drag_data : pg->data;
            for (uint32_t i = 0; i < h->len; i++) inl[i] = src[i];
        }
        uint32_t s2 = __atomic_load_n(&pg->seq, __ATOMIC_ACQUIRE);
        if (s1 == s2) return 1;
    }
    return 0;
}

// Loads a slot into `c`, releasing what it held: an inline payload is
// COPIED, into a buffer sized from a first read of the header and filled
// by a second, which must be the same payload. A LARGE ONE IS MAPPED,
// never copied, and retried when its object went between the read and
// the open: the page then names a newer one.
static int load_slot(struct uclip *c, int drag) {
    if (!c) return 0;
    uclip_free(c);
    volatile struct clip_page *pg = page();
    if (!pg) return 1;   // no service: the clipboard is empty, not broken
    uint32_t inl_max = drag ? CLIP_DRAG_BYTES : CLIP_BYTES;
    for (int tries = 0; tries < 8; tries++) {
        struct head h;
        if (!read_head(pg, drag, &h, 0, 0)) break;
        c->op = h.op;
        c->kind = h.kind;
        c->count = h.count;
        c->serial = h.serial;
        if (h.op != CLIP_OP_NONE && h.where == CLIP_OBJECT) {
            void *p = h.len ? map_object(h.object, h.len, 0) : 0;
            if (!p) continue;
            c->len = h.len;
            c->data = p;
            c->own = p;
            c->own_map = pages_of(h.len);
            return 1;
        }
        uint32_t cap = h.len > inl_max ? inl_max : h.len;
        char *buf = malloc(cap ? cap : 1);
        if (!buf) { ulogf("uclip: no memory for a paste\n"); break; }
        struct head h2;
        if (!read_head(pg, drag, &h2, buf, cap) || h2.serial != h.serial ||
            h2.where != CLIP_INLINE) {
            free(buf);
            continue;
        }
        c->len = h2.len;
        c->data = buf;
        c->own = buf;
        return 1;
    }
    uclip_free(c);   // never a HALF-read clipboard
    return 1;
}

int uclip_load(struct uclip *c) { return load_slot(c, 0); }

int uclip_op(const struct uclip *c) { return c ? (int)c->op : UCLIP_NONE; }
int uclip_count(const struct uclip *c) { return c ? (int)c->count : 0; }
unsigned uclip_serial(const struct uclip *c) { return c ? c->serial : 0; }
int uclip_kind(const struct uclip *c) { return c ? (int)c->kind : UCLIP_KIND_FILES; }

unsigned uclip_peek_serial(void) {
    volatile struct clip_page *pg = page();
    return pg ? __atomic_load_n(&pg->serial, __ATOMIC_ACQUIRE) : 0;
}

unsigned uclip_peek_drag_serial(void) {
    volatile struct clip_page *pg = page();
    return pg ? __atomic_load_n(&pg->drag_serial, __ATOMIC_ACQUIRE) : 0;
}

// The op WITHOUT the payload. uclip_load() takes the whole payload,
// which is far too much to spend telling an app that a cut it is drawing
// has been replaced by somebody else's copy.
int uclip_peek_op(void) {
    volatile struct clip_page *pg = page();
    return pg ? (int)__atomic_load_n(&pg->op, __ATOMIC_ACQUIRE) : UCLIP_NONE;
}

const char *uclip_path(const struct uclip *c, int i) {
    if (!c || !c->data || c->kind != UCLIP_KIND_FILES) return 0;
    if (i < 0 || i >= (int)c->count) return 0;
    // WALKED, not indexed: the entries are variable-length and packed,
    // which is what keeps a clipboard of short names cheap -- from the
    // last answer when `i` is past it, so a loop over thousands of
    // entries is not quadratic. The place is the snapshot's own memory
    // (uclip.h), hence the cast.
    struct uclip *m = (struct uclip *)c;
    int n = 0;
    const char *p = c->data;
    if (m->at_i > 0 && m->at_i <= i) { n = m->at_i; p = c->data + m->at_off; }
    const char *end = c->data + c->len;
    for (; n < i; n++) {
        while (p < end && *p) p++;
        if (p >= end) return 0;
        p++;                       // step over the NUL
    }
    if (p >= end) return 0;
    m->at_i = i;
    m->at_off = (uint32_t)(p - c->data);
    return p;
}

const char *uclip_text(const struct uclip *c, int *out_len) {
    if (out_len) *out_len = 0;
    if (!c || !c->data || c->kind != UCLIP_KIND_TEXT || c->count != 1) return 0;
    if (c->len == 0 || c->len > 0x7FFFFFFFu) return 0;
    // A payload arriving without its terminator is a malformed one, and
    // reading past it would run off the buffer.
    if (c->data[c->len - 1] != '\0') return 0;
    if (out_len) *out_len = (int)c->len - 1;
    return c->data;
}

// --- writing ----------------------------------------------------------
//
// Staged into a process-local buffer, grown to fit, and published under
// the lock, so the page is never partly a new clipboard: a reader that
// arrives mid-copy is caught by the seqlock and retries, and one that
// arrives before or after sees a whole clipboard either way.

static struct {
    uint32_t op, kind, count;
    uint32_t len;
    size_t cap;
    char *data;
    int failed;    // an add was refused: the commit refuses the whole set
} g_out;

// Room for `more` bytes past `len`, doubling. 0 when memory (or the
// page's 32-bit length) runs out.
static int out_room(size_t more) {
    size_t need = (size_t)g_out.len + more;
    if (need > 0x7FFFFFFFu) return 0;
    if (need <= g_out.cap) return 1;
    size_t cap = g_out.cap ? g_out.cap : 256;
    while (cap < need) cap *= 2;
    char *p = realloc(g_out.data, cap);
    if (!p) return 0;
    g_out.data = p;
    g_out.cap = cap;
    return 1;
}

static void out_reset(int op, int kind) {
    g_out.op = (uint32_t)op;
    g_out.kind = (uint32_t)kind;
    g_out.count = 0;
    g_out.len = 0;
    g_out.failed = 0;
    // A large buffer is not kept for the next small copy.
    if (g_out.cap > CLIP_BYTES) { free(g_out.data); g_out.data = 0; g_out.cap = 0; }
}

static int lock_page(volatile struct clip_page *pg) {
    uint32_t want = (uint32_t)sys_getpid();
    if (!want) want = 1;   // 0 means UNLOCKED, so a pid of 0 cannot be an owner
    for (long i = 0; i < CLIP_LOCK_SPINS; i++) {
        uint32_t free_ = 0;
        if (__atomic_compare_exchange_n(&pg->lock, &free_, want, 0,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            // A page clipboardd wiped as it stopped: no copy lands in it.
            if (pg->magic != CLIP_MAGIC) {
                __atomic_store_n(&pg->lock, 0u, __ATOMIC_RELEASE);
                return 0;
            }
            pg->lock_pid = want;
            pg->lock_ms = sys_monotonic_ns() / 1000000ull;
            return 1;
        }
        __builtin_ia32_pause();
    }
    // REFUSED rather than forced. A lock this stuck has a dead owner,
    // and breaking it here would race a live one; clipboardd breaks it
    // within CLIP_LOCK_STALE_MS and the next copy works.
    return 0;
}

static void unlock_page(volatile struct clip_page *pg) {
    pg->lock_ms = 0;
    pg->lock_pid = 0;
    __atomic_store_n(&pg->lock, 0u, __ATOMIC_RELEASE);
}

// clipboardd's channel, opened at the first large copy and kept.
static struct uchan_client g_srv;
static int g_srv_open;
static int g_wait_ms;   // uclip_set_wait_ms(); 0: scaled to the copy

void uclip_set_wait_ms(int ms) { g_wait_ms = ms > 0 ? ms : 0; }

// Asks clipboardd for an object of `len` bytes. Its number, or 0.
static uint32_t ask_object(uint64_t len) {
    struct clip_req req = { CLIP_REQ_OBJECT, 0, len };
    // A daemon restarted since the channel opened is a new server: the
    // old ring is read by nobody, so it is opened again.
    if (g_srv_open && !uchan_client_server_alive(&g_srv)) {
        uchan_client_close(&g_srv);
        g_srv_open = 0;
    }
    if (!g_srv_open) {
        if (uchan_client_open(&g_srv, CLIP_SERVER_NAME) != 0) return 0;
        g_srv_open = 1;
    }
    // THE DAEMON ZEROES EVERY FRAME before it answers, so the wait grows
    // with the copy: 10 s, and a second per 32 MiB past that.
    int wait = g_wait_ms ? g_wait_ms : 10000 + (int)(len / (32u << 20)) * 1000;
    struct clip_reply rep = { -1, 0 };
    if (uchan_call(&g_srv, &req, sizeof req, &rep, sizeof rep, wait) != 0) {
        // ASKED ONCE, NEVER AGAIN: a second request would make a second
        // object. The channel is closed instead, so the late answer is
        // read by nobody -- and clipboardd, seeing it go, lets that
        // object go too.
        ulogf("uclip: clipboardd did not answer for %llu bytes in %d ms\n",
              (unsigned long long)len, wait);
        uchan_client_close(&g_srv);
        g_srv_open = 0;
        return 0;
    }
    if (rep.status != 0 || !rep.object)
        ulogf("uclip: clipboardd refused %llu bytes (%d)\n",
              (unsigned long long)len, (int)rep.status);
    return rep.status == 0 ? rep.object : 0;
}

// Publishes what is staged in g_out to the clipboard, or with `drag` to
// the drag slot. Returns 1 on success.
static int publish(int drag) {
    volatile struct clip_page *pg = page();
    if (!pg) return 0;

    // A LARGE PAYLOAD GOES INTO ITS OBJECT FIRST, outside the page's
    // lock: the copy may be megabytes, and the lock is a spin.
    uint32_t object = 0;
    if (g_out.len > (drag ? CLIP_DRAG_BYTES : CLIP_BYTES)) {
        object = ask_object(g_out.len);
        if (!object) return 0;
        void *p = map_object(object, g_out.len, 1);
        if (!p) return 0;   // clipboardd lets it go after its grace
        memcpy(p, g_out.data, g_out.len);
        sys_munmap(p, pages_of(g_out.len));
    }
    if (!lock_page(pg)) return 0;

    // ODD while the update is in flight -- the reader's whole signal.
    __atomic_store_n(&pg->seq, pg->seq + 1u, __ATOMIC_RELEASE);
    volatile char *dst;
    if (drag) {
        pg->drag_kind = g_out.kind;
        pg->drag_count = g_out.count;
        pg->drag_len = g_out.len;
        pg->drag_pid = g_out.count ? (uint32_t)sys_getpid() : 0;
        pg->drag_where = object ? CLIP_OBJECT : CLIP_INLINE;
        pg->drag_object = object;
        dst = pg->drag_data;
    } else {
        pg->op = g_out.op;
        pg->kind = g_out.kind;
        pg->count = g_out.count;
        pg->len = g_out.len;
        pg->where = object ? CLIP_OBJECT : CLIP_INLINE;
        pg->object = object;
        dst = pg->data;
    }
    if (!object)
        for (uint32_t i = 0; i < g_out.len; i++) dst[i] = g_out.data[i];
    // Bumped INSIDE the update, so a reader either sees the whole new
    // payload with its new serial or the whole old one with the old.
    if (drag) pg->drag_serial++;
    else pg->serial++;
    __atomic_store_n(&pg->seq, pg->seq + 1u, __ATOMIC_RELEASE);

    unlock_page(pg);
    return 1;
}

// publish(), then the staging goes if it was large: an app that copied
// once does not keep the copy's size for as long as it runs.
static int commit_slot(int drag) {
    int ok = publish(drag);
    if (g_out.cap > CLIP_BYTES) {
        free(g_out.data);
        g_out.data = 0;
        g_out.cap = 0;
        g_out.len = 0;
        g_out.count = 0;
    }
    return ok;
}

static int commit(void) { return commit_slot(0); }

void uclip_begin(struct uclip *c, int op) {
    (void)c;   // the staging buffer is this library's; see uclip.h
    out_reset(op, UCLIP_KIND_FILES);
}

int uclip_add(struct uclip *c, const char *path) {
    (void)c;
    if (!path || !*path) { g_out.failed = 1; return 0; }
    size_t n = strlen(path) + 1;      // the NUL is payload
    if (!out_room(n)) { g_out.failed = 1; return 0; }
    memcpy(g_out.data + g_out.len, path, n);
    g_out.len += (uint32_t)n;
    g_out.count++;
    return 1;
}

int uclip_commit(struct uclip *c) {
    (void)c;
    if (g_out.count == 0 || g_out.failed) return 0;
    return commit();
}

// --- the drag slot ----------------------------------------------------

void uclip_drag_begin(struct uclip *c) { uclip_begin(c, UCLIP_COPY); }

int uclip_drag_commit(struct uclip *c) {
    (void)c;
    if (g_out.count == 0 || g_out.failed) return 0;
    return commit_slot(1);
}

int uclip_drag_clear(void) {
    out_reset(UCLIP_NONE, UCLIP_KIND_FILES);
    return commit_slot(1);
}

int uclip_drag_load(struct uclip *c) { return load_slot(c, 1); }

int uclip_clear(void) {
    out_reset(UCLIP_NONE, UCLIP_KIND_FILES);
    return commit();
}

int uclip_set_text(const char *s, int n) {
    if (!s || n < 0) return 0;
    // COPY even for a cut: see uclip.h -- there is nothing to move once
    // the characters are in the page, so an editor's Cut deletes its
    // own selection and copies.
    out_reset(UCLIP_COPY, UCLIP_KIND_TEXT);
    g_out.count = 1;
    // Copied a CHARACTER at a time: a valid sequence as it is, a stray
    // byte re-encoded as its Latin-1 letter (uclip.h) -- which may grow
    // a byte to two, so the length is counted first and asked for exactly.
    size_t need = 1;
    for (int i = 0; i < n;) {
        int k;
        uint32_t cp = uutf8_decode(s + i, n - i, &k);
        char u[4];
        need += k > 1 ? (size_t)k : (size_t)uutf8_encode(cp, u);
        i += k > 1 ? k : 1;
    }
    if (!out_room(need)) return 0;
    uint32_t o = 0;
    for (int i = 0; i < n;) {
        int k;
        uint32_t cp = uutf8_decode(s + i, n - i, &k);
        if (k <= 0) k = 1;
        char u[4];
        int m = k > 1 ? k : uutf8_encode(cp, u);
        if (k > 1) memcpy(g_out.data + o, s + i, (size_t)k);
        else memcpy(g_out.data + o, u, (size_t)m);
        o += (uint32_t)m;
        i += k;
    }
    g_out.data[o] = '\0';
    g_out.len = o + 1;          // the NUL is payload
    return commit();
}

// --- a picture ------------------------------------------------------------

int uclip_set_image(const struct uimg *im) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0) return 0;
    // A NAME NOTHING ELSE USES: this process and a count, so two apps
    // copying at once cannot write the same file.
    static unsigned n;
    volatile struct clip_page *pg = page();
    if (!pg) return 0;
    char dir[48], part[96], path[96];
    snprintf(dir, sizeof dir, CLIP_IMAGE_DIR_FMT, (unsigned)pg->uid);
    // THE ACCOUNT'S OWN FOLDER, OR NO PICTURE: one somebody else made
    // first under that name would be theirs to read. Made here only by the
    // account itself -- a root copier's would be root's, and refused for
    // the rest of the boot (/tmp is sticky).
    if (geteuid() == pg->uid) mkdir(dir, 0700);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != pg->uid ||
        (st.st_mode & 077)) {
        ulogf("uclip: %s is not this account's own folder -- picture not copied\n", dir);
        return 0;
    }
    snprintf(path, sizeof path, "%s/%d-%u.qoi", dir, (int)getpid(), ++n);
    snprintf(part, sizeof part, "%s.part", path);
    // WRITTEN, THEN RENAMED, THEN NAMED: a paster never opens a file
    // half written, and clipboardd's sweep leaves a .part alone.
    if (uimg_save(part, im, "qoi") != 0) { unlink(part); return 0; }
    if (rename(part, path) != 0) { unlink(part); return 0; }
    out_reset(UCLIP_COPY, UCLIP_KIND_IMAGE);
    size_t len = strlen(path) + 1;
    if (!out_room(len)) { unlink(path); return 0; }
    memcpy(g_out.data, path, len);
    g_out.len = (uint32_t)len;
    g_out.count = 1;
    if (!commit()) { unlink(path); return 0; }
    return 1;
}

const char *uclip_image(const struct uclip *c) {
    if (!c || !c->data || c->op == UCLIP_NONE || c->kind != UCLIP_KIND_IMAGE || !c->count) return 0;
    return c->data;
}
