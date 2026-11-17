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

int sys_close(int fd) {
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

// --- memory: no shared objects to map (no fontd, no compositor) -----------------

void *sys_mmap(void *addr, uint64_t length, int prot, int flags, int fd, uint64_t offset) {
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
    g_errno = 38;   // ENOSYS
    return (void *)-1;
}
int sys_munmap(void *addr, uint64_t length) { (void)addr; (void)length; return 0; }
// No server publishes anything (fontd's atlas, the clipboard's page).
int sys_shm_open(const char *name, uint64_t length, int flags)
{ (void)name; (void)length; (void)flags; return fail(-2); }
int sys_shm_open_from(const char *name, int pid) { (void)name; (void)pid; return fail(-2); }
int sys_shm_unlink(const char *name) { (void)name; return fail(-2); }
int sys_shm_grant(const char *name, int pid) { (void)name; (void)pid; return fail(-2); }

// No process table: clock() and friends take their failure path.
int sys_proc_info(int index, struct proc_info *out) { (void)index; (void)out; return fail(-38); }

// No compositor: WIN_REQ_FONT and the rest answer "no server".
int sys_win_request(struct win_request_msg *req) { (void)req; return 0; }

// No query records (the screen, the machine): a caller takes its default.
int sys_query_record(unsigned cls, unsigned index, void *out, unsigned len)
{ (void)cls; (void)index; (void)out; (void)len; return fail(-2); }

// --- the console --------------------------------------------------------------

int64_t sys_print(const char *s) { return sys_write(1, s, strlen(s)); }
int64_t sys_eprint(const char *s) { return sys_write(2, s, strlen(s)); }

// --- one process, one thread ---------------------------------------------------

int sys_getpid(void) { return 1; }
// uselocale()'s per-thread slot, which rt keeps beside errno.
void **__toy_locale_location(void) { static void *loc; return &loc; }
int sys_gettid(void) { return 1; }
void sys_yield(void) {}

// Nothing else runs, so a futex wait would wait for ever: it returns at
// once as a timeout, and the caller's loop re-checks its word.
int sys_futex_wait(volatile uint32_t *word, uint32_t expected, int timeout_ms) {
    (void)word; (void)expected; (void)timeout_ms;
    g_errno = 110;   // ETIMEDOUT
    return -1;
}
int sys_futex_wake(volatile uint32_t *word, int count) { (void)word; (void)count; return 0; }

// --- the clock: it moves when the harness says ---------------------------------

static uint64_t g_now_ns = 1000000000ull;   // never 0: "never" in several widgets

void uitest_advance_ms(int ms) { g_now_ns += (uint64_t)((int64_t)ms * 1000000); }
uint64_t uitest_now_ns(void) { return g_now_ns; }
unsigned long long sys_monotonic_ns(void) { return g_now_ns; }
unsigned long sys_ticks(void) { return (unsigned long)(g_now_ns / 10000000ull); }   // 100 Hz
int sys_sleep_ms(int ms) { uitest_advance_ms(ms); return 0; }

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
