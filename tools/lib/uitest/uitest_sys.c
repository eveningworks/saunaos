// THE PLATFORM, STUBBED: the sys_* calls the toolkit reaches, answered on
// the host -- what Qt's offscreen QPA plugin is to QtWidgets. Compiled
// against SaunaOS's headers (rt/sys.h's contract: -1 and sys_errno()),
// talking to the host only through uitest_host.h.
//
// Only what a test has needed is here; a widget that reaches a call
// nobody stubbed fails to LINK, naming it, which is the right place to
// find out. A stub's answer is the smallest true one: one process, one
// thread, no compositor, no kernel registry, a clock that moves only
// when the harness moves it.
//
// Also here: the C library calls whose STRUCTS differ between tolibc and
// glibc, which the toolkit would otherwise take from glibc and read at
// the wrong offsets -- opendir/readdir (tolibc's d_name is at offset 1).
// tools/hostchecks/uitest_hostcheck.py refuses a binary that imports one
// of those from glibc; it compiles tolibc's own stat.c over these stubs.
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/sys.h"
#include "setting_abi.h"
#include "syscall_abi.h"
#include "caltime.h"
#include "win_proto.h"
#include "query_abi.h"
#include "uitest.h"
#include "uitest_host.h"

// sys_errno() and errno are one storage in tolibc (<errno.h>), and here
// both are glibc's errno, which tolibc's macro reaches the same way.
#define g_errno errno

static int fail(long long rc) {   // -errno from the host -> rt/sys.h's -1
    g_errno = (int)-rc;
    return -1;
}

int sys_errno(void) { return g_errno; }

// --- files, under $UITEST_ROOT ---------------------------------------------

int sys_open(const char *path, int flags) {
    int rc = uh_open(path, (flags & SYS_O_WRITE) != 0, (flags & SYS_O_CREAT) != 0,
                     (flags & SYS_O_TRUNC) != 0, (flags & SYS_O_APPEND) != 0,
                     (flags & SYS_O_RDWR) != 0);
    return rc < 0 ? fail(rc) : rc;
}

int64_t sys_read(int fd, void *buf, size_t len) {
    long long n = uh_read(fd, buf, len);
    return n < 0 ? fail(n) : n;
}

int64_t sys_write(int fd, const void *buf, size_t len) {
    long long n = uh_write(fd, buf, len);
    return n < 0 ? fail(n) : n;
}

static int shm_close(int fd);
int sys_close(int fd) {
    if (shm_close(fd)) return 0;
    int rc = uh_close(fd);
    return rc < 0 ? fail(rc) : 0;
}

long long sys_lseek(int fd, long long offset, int whence) {
    long long r = uh_lseek(fd, offset, whence);
    return r < 0 ? fail(r) : r;
}

static void to_sys_stat(const struct uh_stat *h, struct sys_stat *out) {
    memset(out, 0, sizeof *out);
    out->size = h->size;
    out->ino = h->ino;
    out->is_dir = h->is_dir;
    out->flags = SYS_STAT_INODES | SYS_STAT_MODE | (h->is_dir ? 0 : SYS_STAT_SEEKABLE);
    out->mode = (uint16_t)h->mode;
    out->nlink = (uint16_t)h->nlink;
    cal_epoch_to_rtc(h->mtime > 0 ? (uint64_t)h->mtime : 0, &out->modified);
    out->created = out->modified;
}

int sys_stat(const char *path, struct sys_stat *out) {
    struct uh_stat h;
    int rc = uh_stat(path, &h);
    if (rc < 0) return fail(rc);
    to_sys_stat(&h, out);
    return 0;
}

int sys_fstat(int fd, struct sys_stat *out) {
    struct uh_stat h;
    int rc = uh_fstat(fd, &h);
    if (rc < 0) return fail(rc);
    to_sys_stat(&h, out);
    return 0;
}

int sys_mkdir(const char *path) {
    int rc = uh_mkdir(path);
    return rc < 0 ? fail(rc) : 0;
}

int sys_unlink(const char *path) {
    int rc = uh_unlink(path);
    return rc < 0 ? fail(rc) : 0;
}

int sys_rename(const char *from, const char *to) {
    int rc = uh_rename(from, to);
    return rc < 0 ? fail(rc) : 0;
}

// Metadata nothing in a widget sets: refused, so a caller sees a failure
// rather than a change that did not happen.
int sys_chmod(const char *path, unsigned mode) { (void)path; (void)mode; return fail(-38); }
int sys_fchmod(int fd, unsigned mode) { (void)fd; (void)mode; return fail(-38); }
int sys_utime(const char *path, unsigned long long epoch) { (void)path; (void)epoch; return fail(-38); }
int sys_utimens(const char *path, unsigned long long epoch, unsigned nsec)
{ (void)path; (void)epoch; (void)nsec; return fail(-38); }
int sys_futimens(int fd, unsigned long long epoch, unsigned nsec)
{ (void)fd; (void)epoch; (void)nsec; return fail(-38); }

// Nothing else writes the root while a test runs, so one generation.
long long sys_fs_generation_of(const char *path) { (void)path; return 1; }

// SYS_GETDENTS: whole records from the `start`th entry, each padded to 8.
int sys_getdents(const char *path, void *buf, uint32_t len, uint32_t start) {
    void *h = uh_dir_open(path);
    if (!h) return fail(-2);
    char name[256];
    int is_dir = 0;
    uint32_t i = 0, used = 0;
    while (uh_dir_next(h, name, sizeof name, &is_dir)) {
        if (i++ < start) continue;
        size_t nlen = strlen(name) + 1;
        uint32_t rec = (uint32_t)((sizeof(struct sys_dirent64) + nlen + 7) & ~(size_t)7);
        if (used + rec > len) {
            uh_dir_close(h);
            return used ? (int)used : fail(-22);   // EINVAL: grow the buffer
        }
        struct sys_dirent64 *d = (struct sys_dirent64 *)((char *)buf + used);
        memset(d, 0, rec);
        d->d_off = i;
        d->d_reclen = (uint16_t)rec;
        d->d_type = is_dir ? SYS_DT_DIR : SYS_DT_REG;
        memcpy(d->d_name, name, nlen);
        if (!is_dir) {
            char full[4096];
            struct uh_stat st;
            snprintf(full, sizeof full, "%s/%s", path, name);
            if (uh_stat(full, &st) == 0) {
                d->d_size = st.size;
                cal_epoch_to_rtc(st.mtime > 0 ? (uint64_t)st.mtime : 0, &d->d_modified);
            }
        }
        used += rec;
    }
    uh_dir_close(h);
    return (int)used;
}

// tolibc's opendir/readdir, in tolibc's struct dirent.
struct _DIR {
    void *host;
    struct dirent ent;
};

DIR *opendir(const char *path) {
    void *h = uh_dir_open(path);
    if (!h) { g_errno = 2; return NULL; }   // ENOENT
    DIR *d = calloc(1, sizeof *d);
    if (!d) { uh_dir_close(h); return NULL; }
    d->host = h;
    return d;
}

struct dirent *readdir(DIR *d) {
    int is_dir = 0;
    if (!d || !uh_dir_next(d->host, d->ent.d_name, sizeof d->ent.d_name, &is_dir)) return NULL;
    d->ent.d_type = is_dir ? DT_DIR : DT_REG;
    return &d->ent;
}

int closedir(DIR *d) {
    if (!d) return -1;
    uh_dir_close(d->host);
    free(d);
    return 0;
}

// --- shared memory: named objects, one process ------------------------------------
//
// REAL ONES, because the window protocol is built of them: uapp's buffers
// and its channel to the compositor (base/uchan.h) are shm objects, and
// the stand-in compositor (uitest_app.c) maps the same objects by name.
// EVERY MAPPING OF ONE OBJECT IS THE SAME MEMORY -- as two processes'
// mappings of one shm object are the same pages -- so the server's view
// of a ring is the client's. An object lives until it is unlinked AND
// unmapped is not tracked: a test is short, so an unlinked object's
// memory simply stays (nothing reads it again).

struct shm_obj {
    char name[64];
    void *mem;
    uint64_t bytes;
    int creator, unlinked;
};
static struct shm_obj *g_shm;
static int g_shm_count, g_shm_cap;
#define SHM_FD_BASE (1 << 20)   // clear of the host's fds, which sys_open hands out

static struct shm_obj *shm_find(const char *name) {
    for (int i = g_shm_count - 1; i >= 0; i--)
        if (!g_shm[i].unlinked && !strcmp(g_shm[i].name, name)) return &g_shm[i];
    return NULL;
}

static int shm_close(int fd) { return fd >= SHM_FD_BASE && fd < SHM_FD_BASE + g_shm_count; }

int sys_shm_open(const char *name, uint64_t length, int flags) {
    if (!name || strlen(name) >= sizeof g_shm[0].name) return fail(-22);
    struct shm_obj *o = shm_find(name);
    if (o && (flags & SHM_CREATE) && (flags & SHM_EXCL)) return fail(-17);   // EEXIST
    if (!o) {
        if (!(flags & SHM_CREATE)) return fail(-2);
        if (g_shm_count == g_shm_cap) {
            int cap = g_shm_cap ? g_shm_cap * 2 : 32;
            struct shm_obj *grown = realloc(g_shm, (size_t)cap * sizeof *grown);
            if (!grown) return fail(-12);
            g_shm = grown;
            g_shm_cap = cap;
        }
        o = &g_shm[g_shm_count++];
        memset(o, 0, sizeof *o);
        strlcpy(o->name, name, sizeof o->name);
        o->bytes = length;
        o->creator = sys_getpid();
        o->mem = calloc(1, length ? (size_t)length : 1);
        if (!o->mem) { g_shm_count--; return fail(-12); }
    }
    return SHM_FD_BASE + (int)(o - g_shm);
}

int sys_shm_open_from(const char *name, int pid) {
    struct shm_obj *o = shm_find(name);
    if (!o || o->creator != pid) return fail(-2);
    return SHM_FD_BASE + (int)(o - g_shm);
}

int sys_shm_unlink(const char *name) {
    struct shm_obj *o = shm_find(name);
    if (!o) return fail(-2);
    o->unlinked = 1;
    return 0;
}

int sys_shm_grant(const char *name, int pid) { (void)pid; return shm_find(name) ? 0 : fail(-2); }

void *sys_mmap(void *addr, uint64_t length, int prot, int flags, int fd, uint64_t offset) {
    (void)addr; (void)prot;
    if (!(flags & SYS_MAP_ANONYMOUS) && fd >= SHM_FD_BASE && fd < SHM_FD_BASE + g_shm_count) {
        struct shm_obj *o = &g_shm[fd - SHM_FD_BASE];
        if (offset + length > o->bytes) { g_errno = 22; return (void *)-1; }
        return (char *)o->mem + offset;
    }
    if (flags & SYS_MAP_ANONYMOUS) {   // fd and offset ignored, as the ABI says
        void *p = calloc(1, length ? (size_t)length : 1);
        if (p) return p;
        g_errno = 12;
        return (void *)-1;
    }
    g_errno = 19;   // ENODEV: a file mapping is not stubbed
    return (void *)-1;
}
int sys_munmap(void *addr, uint64_t length) { (void)addr; (void)length; return 0; }

// ONE THREAD: a program that would start another (a player's audio thread)
// is refused, and takes the path it has for that. tolibc's own pthread.c
// is linked over these (its structs are not glibc's).
int sys_thread_create(void (*entry)(void *), void *stack_top, void *arg,
                      void *tls, int detached, unsigned stack_size) {
    (void)entry; (void)stack_top; (void)arg; (void)tls; (void)detached; (void)stack_size;
    return fail(-11);   // EAGAIN
}
// rt's thread-local block for a new thread: never reached, since the
// create above is refused, but pthread_create names them first.
uint64_t rt_tls_size(void) { return 64; }
void *rt_tls_install(void *mem) { return mem; }
// No loader on the host: pthread.c takes rt's path (and is refused above).
int rt_tls_by_loader(void) { return 0; }
void sys_thread_exit(int code) { (void)code; uh_die("sys_thread_exit with no thread to end"); }
int sys_thread_join(int tid) { (void)tid; return fail(-3); }     // ESRCH
int sys_thread_detach(int tid) { (void)tid; return fail(-3); }

// No sound card: a program that would play takes its no-device path.
int sys_snd_open(void) { return fail(-19); }   // ENODEV
int sys_snd_ctl(int op) { (void)op; return fail(-19); }
int sys_snd_format(uint32_t rate, uint32_t bits) { (void)rate; (void)bits; return fail(-19); }

// No process table: clock() and friends take their failure path.
int sys_proc_info(int index, struct proc_info *out) { (void)index; (void)out; return fail(-38); }

// No compositor: WIN_REQ_FONT and the rest answer "no server".
int sys_win_request(struct win_request_msg *req) { (void)req; return 0; }

// QUERY_SHM is answered (uchan's server finds its clients' rings by it);
// every other class has no records, and a caller takes its default.
int sys_query_record(unsigned cls, unsigned index, void *out, unsigned len) {
    if (cls != QUERY_SHM || len < sizeof(struct query_shm)) return fail(-2);
    int live = -1;
    for (int i = 0; i < g_shm_count; i++) {
        if (g_shm[i].unlinked || ++live != (int)index) continue;
        struct query_shm r;
        memset(&r, 0, sizeof r);
        strlcpy(r.name, g_shm[i].name, sizeof r.name);
        r.bytes = g_shm[i].bytes;
        r.refs = 1;
        r.creator_pid = g_shm[i].creator;
        memcpy(out, &r, sizeof r);
        return (int)sizeof r;
    }
    return 0;
}

// --- the console --------------------------------------------------------------

int64_t sys_print(const char *s) { return sys_write(1, s, strlen(s)); }
// What the app logs (base/ulog.h writes through here) is KEPT as well as
// printed: an app's own report of its state -- Minesweeper's `mines:
// state ...` line -- is what a case asserts on, as the GUI tools read it
// from the debug console.
static char *g_log;
static size_t g_log_len, g_log_cap;

int64_t sys_eprint(const char *s) {
    size_t n = strlen(s);
    if (g_log_len + n + 1 > g_log_cap) {
        size_t cap = g_log_cap ? g_log_cap * 2 : 65536;
        while (cap < g_log_len + n + 1) cap *= 2;
        char *grown = realloc(g_log, cap);
        if (grown) { g_log = grown; g_log_cap = cap; }
    }
    if (g_log_len + n + 1 <= g_log_cap) {
        memcpy(g_log + g_log_len, s, n + 1);
        g_log_len += n;
    }
    return sys_write(2, s, n);
}

size_t uitest_log_mark(void) { return g_log_len; }

int uitest_log_find(size_t mark, const char *needle, char *out, size_t cap) {
    if (!g_log || mark > g_log_len) return 0;
    int found = 0;
    for (char *line = g_log + mark; line < g_log + g_log_len;) {
        char *end = memchr(line, '\n', (size_t)(g_log + g_log_len - line));
        size_t len = end ? (size_t)(end - line) : (size_t)(g_log + g_log_len - line);
        char save = line[len];
        line[len] = 0;
        if (strstr(line, needle)) {
            if (out && cap) strlcpy(out, line, cap);
            found = 1;
        }
        line[len] = save;
        line += len + (end ? 1 : 0);
    }
    return found;
}

// A FIXED sequence, so a game's board is the same on every run.
int sys_getrandom(void *buf, unsigned long n) {
    static uint32_t x = 0x2545F491u;
    for (unsigned long i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        ((unsigned char *)buf)[i] = (unsigned char)x;
    }
    return (int)n;
}

// --- one process, one thread ---------------------------------------------------

int sys_getpid(void) { return 1; }
// uselocale()'s per-thread slot, which rt keeps beside errno.
void **__toy_locale_location(void) { static void *loc; return &loc; }
int sys_gettid(void) { return 1; }
// No children: an app's reap of its spawned programs finds none.
int sys_waitpid_nohang(int pid, int *status) { (void)pid; (void)status; return fail(-10); }   // ECHILD
void sys_yield(void) {}

// A FUTEX WAIT IS WHERE AN APP PARKS. With an app running under
// uitest_app.c, the wait hands control to the stand-in compositor and the
// test, and returns when they resume the app; otherwise nothing else runs,
// so it returns at once as a timeout and the caller's loop re-checks.
int (*uitest_futex_hook)(volatile uint32_t *word, uint32_t expected, int timeout_ms);
int sys_futex_wait(volatile uint32_t *word, uint32_t expected, int timeout_ms) {
    if (*word != expected) { g_errno = 11; return -1; }   // EAGAIN: it moved
    if (uitest_futex_hook) return uitest_futex_hook(word, expected, timeout_ms);
    g_errno = 110;   // ETIMEDOUT
    return -1;
}
int sys_futex_wake(volatile uint32_t *word, int count) { (void)word; (void)count; return 0; }
// The word the kernel would bump on an event; the stand-in wakes by the
// ring's own words instead.
int sys_wakeword(volatile uint32_t *word) { (void)word; return 0; }

// --- the clock: it moves when the harness says ---------------------------------

static uint64_t g_now_ns = 1000000000ull;   // never 0: "never" in several widgets

void uitest_advance_ms(int ms) { g_now_ns += (uint64_t)((int64_t)ms * 1000000); }
uint64_t uitest_now_ns(void) { return g_now_ns; }
unsigned long long sys_monotonic_ns(void) { return g_now_ns; }
unsigned long sys_ticks(void) { return (unsigned long)(g_now_ns / 10000000ull); }   // 100 Hz
// A sleep under uitest_app.c is a park too: the app is waiting for the
// other side (uchan_send_wait() on a full ring), which runs now.
int (*uitest_sleep_hook)(int ms);
int sys_sleep_ms(int ms) {
    uitest_advance_ms(ms);
    if (uitest_sleep_hook) uitest_sleep_hook(ms);
    return 0;
}

// The wall clock: a FIXED date, so a widget that shows "today" draws the
// same thing on every run -- Thursday 2026-01-15 12:00:00 UTC, moved on by
// the harness's clock.
int sys_gettime(struct rtc_time *out) {
    cal_epoch_to_rtc(1768478400ull + g_now_ns / 1000000000ull, out);
    return 0;
}

// --- settings: the kernel registry, as the test declares it ----------------------

struct setting {
    struct setting *next;
    char *name, *value;
};
static struct setting *g_settings;
static uint32_t g_generation = 1;

void uitest_setting(const char *name, const char *value) {
    struct setting **p = &g_settings;
    while (*p && strcmp((*p)->name, name)) p = &(*p)->next;
    if (!value) {
        if (*p) {
            struct setting *gone = *p;
            *p = gone->next;
            free(gone->name);
            free(gone->value);
            free(gone);
        }
    } else if (*p) {
        free((*p)->value);
        (*p)->value = strdup(value);
    } else {
        struct setting *s = calloc(1, sizeof *s);
        if (!s) return;
        s->name = strdup(name);
        s->value = strdup(value);
        *p = s;
    }
    g_generation++;
}

int sys_setting(struct setting_msg *m) {
    m->generation = g_generation;
    struct setting *s = g_settings;
    switch (m->op) {
    case SETTING_OP_COUNT:
        m->count = 0;   // the registry's INFO walk is not stubbed: a GET by name is
        return 0;
    case SETTING_OP_TOUCH:
        g_generation++;
        m->generation = g_generation;
        return 0;
    case SETTING_OP_GET:
        while (s && strcmp(s->name, m->name)) s = s->next;
        if (!s) { g_errno = 2; return -1; }
        m->type = SETTING_ABI_TYPE_STRING;
        strlcpy(m->value, s->value, sizeof m->value);
        return 0;
    case SETTING_OP_SET:
        while (s && strcmp(s->name, m->name)) s = s->next;
        if (!s) { g_errno = 2; return -1; }
        uitest_setting(m->name, m->value);
        return 0;
    default:
        g_errno = 38;   // ENOSYS
        return -1;
    }
}
