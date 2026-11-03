// fstorture -- seeded random filesystem operations, checked against a model.
//
//   fstorture run   DIR SEED OPS [MAXKB]  fsx's shape: in-place writes, appends,
//                                         truncates both ways, holes, renames and
//                                         atomic replaces on eight files in two
//                                         directories, every result checked
//   fstorture cut   DIR SEED [GENS]       power-cut mode: GENERATIONS of atomic
//                                         replaces and deletes, each followed by
//                                         sync(); runs until killed (or GENS)
//   fstorture check DIR SEED GEN          after a cut: every file is a version
//                                         from generation GEN or later -- whole,
//                                         never torn, never rolled back
//   fstorture poke  FILE OFFSET           flip one byte in place: the positive
//                                         control that `check` sees a torn file
//
// EVERYTHING IS A FUNCTION OF THE SEED. A failure prints the seed, the
// op number and the last ops, and `run` with the same arguments replays
// it exactly; `check` regenerates the cut stream instead of reading a
// log, which is what lets it run on the far side of a reboot.
//
// What `check` holds a volume to is the contract an application relies
// on (write a temp file, fsync, rename over the old one): a file is the
// old version or the new one, and a generation that sync() returned for
// is never lost. It does NOT assume ops land in order between syncs --
// POSIX does not promise that, and a test that did would fail a correct
// filesystem. tools/tests/sweep/fstorture_test.py and powercut_test.py
// drive the three modes.
#include "rt/sys.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define NFILES 8           // `run`'s files, f0..f7, each in d0/ or d1/
#define NCUT   16          // `cut`'s files, c0..c15
#define CUT_MAX_KB 256
#define WRITE_MAX (64 * 1024)
#define FULL_CHECK_EVERY 50
#define HISTORY 16

// ---- the PRNG: splitmix64, so a seed alone names the whole run ----------

static uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

struct rng { uint64_t s; };
static uint64_t next(struct rng *r) { r->s += 0x9E3779B97F4A7C15ULL; return mix(r->s); }
static uint64_t below(struct rng *r, uint64_t n) { return n ? next(r) % n : 0; }

static void fill(struct rng *r, uint8_t *p, size_t n) {
    while (n) {
        uint64_t v = next(r);
        size_t k = n < 8 ? n : 8;
        memcpy(p, &v, k);
        p += k;
        n -= k;
    }
}

// ---- failure reporting ----------------------------------------------------

static const char *g_mode = "";
static uint64_t g_seed;
static long g_op;
static char g_hist[HISTORY][96];
static int g_hist_n;

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_hist[g_hist_n++ % HISTORY], sizeof g_hist[0], fmt, ap);
    va_end(ap);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    sys_eprint(line);
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fail(const char *fmt, ...) {
    char why[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    say("fstorture: FAIL %s seed %llu op %ld: %s\n", g_mode,
        (unsigned long long)g_seed, g_op, why);
    int first = g_hist_n > HISTORY ? g_hist_n - HISTORY : 0;
    for (int i = first; i < g_hist_n; i++)
        say("fstorture:   %s\n", g_hist[i % HISTORY]);
    exit(1);
}

// ---- file helpers ---------------------------------------------------------

static int write_at(const char *path, uint64_t off, const uint8_t *p, size_t n, int flags) {
    int fd = open(path, O_WRONLY | flags, 0644);
    if (fd < 0) return -errno;
    if (!(flags & O_APPEND) && lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) {
        int e = errno; close(fd); return -e;
    }
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { int e = w < 0 ? errno : EIO; close(fd); return -e; }
        p += w;
        n -= (size_t)w;
    }
    return fd;   // still open: the caller may fsync it
}

// Reads exactly `n` bytes at `off`, or fails the run naming the path.
static void read_at(const char *path, uint64_t off, uint8_t *p, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) fail("open %s for reading: %s", path, strerror(errno));
    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) fail("lseek %s to %llu", path, (unsigned long long)off);
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) fail("read %s at %llu: %s", path, (unsigned long long)off,
                         r < 0 ? strerror(errno) : "short (EOF)");
        p += r;
        n -= (size_t)r;
    }
    close(fd);
}

static int64_t size_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

static void mkdir_once(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) fail("mkdir %s: %s", path, strerror(errno));
}

// ---- run: fsx against a model ---------------------------------------------

struct mfile {
    int exists;
    int dir;          // 0 or 1: which of d0/ d1/ holds it
    size_t size, cap;
    uint8_t *data;
};

static struct mfile g_files[NFILES];
static const char *g_dir;
static size_t g_max;
static int g_nonatomic;   // the volume refuses rename over an existing file (FAT32)
static uint8_t *g_buf;    // WRITE_MAX, and g_max for a full-file read

static void path_of(char *out, size_t cap, int i) {
    snprintf(out, cap, "%s/d%d/f%d", g_dir, g_files[i].dir, i);
}

static void m_resize(struct mfile *f, size_t n) {
    if (n > f->cap) {
        size_t c = f->cap ? f->cap : 4096;
        while (c < n) c *= 2;
        uint8_t *d = realloc(f->data, c);
        if (!d) fail("model: out of memory at %zu bytes", c);
        f->data = d;
        f->cap = c;
    }
    if (n > f->size) memset(f->data + f->size, 0, n - f->size);   // a hole reads as zeros
    f->size = n;
}

static void check_range(int i, uint64_t off, size_t n) {
    char p[160];
    path_of(p, sizeof p, i);
    read_at(p, off, g_buf, n);
    if (memcmp(g_buf, g_files[i].data + off, n) == 0) return;
    for (size_t k = 0; k < n; k++) {
        if (g_buf[k] != g_files[i].data[off + k])
            fail("%s: byte %llu is 0x%02x, the model says 0x%02x", p,
                 (unsigned long long)(off + k), g_buf[k], g_files[i].data[off + k]);
    }
}

static void full_check(void) {
    for (int d = 0; d < 2; d++) {
        char p[160];
        snprintf(p, sizeof p, "%s/d%d", g_dir, d);
        DIR *dir = opendir(p);
        if (!dir) fail("opendir %s: %s", p, strerror(errno));
        int seen[NFILES] = {0};
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            int i = -1;
            if ((e->d_name[0] == 'f' || e->d_name[0] == 'F') && e->d_name[1] >= '0' &&
                e->d_name[1] < '0' + NFILES && !e->d_name[2])
                i = e->d_name[1] - '0';
            if (i < 0 || !g_files[i].exists || g_files[i].dir != d)
                fail("%s holds %s, which the model does not", p, e->d_name);
            seen[i] = 1;
        }
        closedir(dir);
        for (int i = 0; i < NFILES; i++)
            if (g_files[i].exists && g_files[i].dir == d && !seen[i])
                fail("%s/f%d is missing from the listing", p, i);
    }
    for (int i = 0; i < NFILES; i++) {
        char p[160];
        path_of(p, sizeof p, i);
        int64_t sz = size_of(p);
        if (!g_files[i].exists) {
            if (sz >= 0) fail("%s exists, the model deleted it", p);
            continue;
        }
        if (sz != (int64_t)g_files[i].size)
            fail("%s is %lld bytes, the model says %zu", p, (long long)sz, g_files[i].size);
        if (g_files[i].size) check_range(i, 0, g_files[i].size);
    }
}

static int pick(struct rng *r, int want_exists) {
    int start = (int)below(r, NFILES);
    for (int k = 0; k < NFILES; k++) {
        int i = (start + k) % NFILES;
        if (g_files[i].exists == want_exists) return i;
    }
    return -1;
}

static void op_create(struct rng *r) {
    int i = pick(r, 0);
    if (i < 0) return;
    g_files[i].dir = (int)below(r, 2);
    char p[160];
    path_of(p, sizeof p, i);
    note("%ld create %s", g_op, p);
    int fd = write_at(p, 0, g_buf, 0, O_CREAT | O_EXCL);
    if (fd < 0) fail("create %s: %s", p, strerror(-fd));
    close(fd);
    g_files[i].exists = 1;
    m_resize(&g_files[i], 0);
}

static void op_write(struct rng *r, int append) {
    int i = pick(r, 1);
    if (i < 0) { op_create(r); return; }
    struct mfile *f = &g_files[i];
    if (f->size >= g_max) append = 0;   // an append there would pass the cap; overwrite instead
    uint64_t off = append ? f->size : below(r, f->size + g_max / 8 + 1);
    if (off >= g_max) off = below(r, g_max);
    size_t room = g_max - (size_t)off;
    size_t n = 1 + (size_t)below(r, room < WRITE_MAX ? room : WRITE_MAX);
    fill(r, g_buf, n);
    char p[160];
    path_of(p, sizeof p, i);
    int sync = below(r, 8) == 0;
    note("%ld %s %s off %llu len %zu%s", g_op, append ? "append" : "write", p,
         (unsigned long long)off, n, sync ? " +fsync" : "");
    int fd = write_at(p, off, g_buf, n, append ? O_APPEND : 0);
    if (fd < 0) fail("%s %s: %s", append ? "append" : "write", p, strerror(-fd));
    if (sync && fsync(fd) != 0) fail("fsync %s: %s", p, strerror(errno));
    close(fd);
    if (off + n > f->size) m_resize(f, off + n);
    memcpy(f->data + off, g_buf, n);
}

static void op_truncate(struct rng *r) {
    int i = pick(r, 1);
    if (i < 0) return;
    struct mfile *f = &g_files[i];
    // Half shrink, half grow: growing is the hole the model must read as zeros.
    size_t n = below(r, 2) ? (size_t)below(r, f->size + 1) : (size_t)below(r, g_max + 1);
    char p[160];
    path_of(p, sizeof p, i);
    note("%ld truncate %s %zu -> %zu", g_op, p, f->size, n);
    if (sys_truncate(p, n) != 0) fail("truncate %s to %zu: %s", p, n, strerror(errno));
    m_resize(f, n);
}

static void op_unlink(struct rng *r) {
    int i = pick(r, 1);
    if (i < 0) return;
    char p[160];
    path_of(p, sizeof p, i);
    note("%ld unlink %s", g_op, p);
    if (unlink(p) != 0) fail("unlink %s: %s", p, strerror(errno));
    g_files[i].exists = 0;
}

static void op_move(struct rng *r) {
    int i = pick(r, 1);
    if (i < 0) return;
    char from[160], to[160];
    path_of(from, sizeof from, i);
    g_files[i].dir ^= 1;
    path_of(to, sizeof to, i);
    note("%ld rename %s -> %s", g_op, from, to);
    if (rename(from, to) != 0) fail("rename %s -> %s: %s", from, to, strerror(errno));
}

// Atomic replace: a temp file written whole, fsync'd, renamed over.
static void replace_file(const char *dst, const char *tmp, const uint8_t *p, size_t n) {
    int fd = write_at(tmp, 0, p, n, O_CREAT | O_TRUNC);
    if (fd < 0) fail("write %s: %s", tmp, strerror(-fd));
    if (fsync(fd) != 0) fail("fsync %s: %s", tmp, strerror(errno));
    close(fd);
    if (!g_nonatomic && rename(tmp, dst) == 0) return;
    if (!g_nonatomic && errno != ENOTSUP) fail("rename %s over %s: %s", tmp, dst, strerror(errno));
    if (!g_nonatomic) {
        g_nonatomic = 1;
        say("fstorture: this volume cannot rename over a file; replacing by unlink + rename\n");
    }
    if (unlink(dst) != 0 && errno != ENOENT) fail("unlink %s: %s", dst, strerror(errno));
    if (rename(tmp, dst) != 0) fail("rename %s -> %s: %s", tmp, dst, strerror(errno));
}

static void op_replace(struct rng *r) {
    int i = pick(r, 1);
    if (i < 0) return;
    struct mfile *f = &g_files[i];
    size_t n = (size_t)below(r, (g_max < 4 * WRITE_MAX ? g_max : 4 * WRITE_MAX) + 1);
    m_resize(f, n);
    fill(r, f->data, n);
    char p[160], t[160];
    path_of(p, sizeof p, i);
    snprintf(t, sizeof t, "%s/d%d/tmp", g_dir, f->dir);
    note("%ld replace %s with %zu bytes", g_op, p, n);
    replace_file(p, t, f->data, n);
}

static void op_read(struct rng *r) {
    int i = pick(r, 1);
    if (i < 0 || !g_files[i].size) return;
    uint64_t off = below(r, g_files[i].size);
    size_t room = g_files[i].size - (size_t)off;
    size_t n = 1 + (size_t)below(r, room < WRITE_MAX ? room : WRITE_MAX);
    note("%ld read f%d off %llu len %zu", g_op, i, (unsigned long long)off, n);
    check_range(i, off, n);
}

static int cmd_run(const char *dir, uint64_t seed, long ops, size_t maxkb) {
    g_mode = "run";
    g_dir = dir;
    g_max = maxkb * 1024;
    g_buf = malloc(g_max > WRITE_MAX ? g_max : WRITE_MAX);
    if (!g_buf) fail("no memory for a %zu KiB buffer", maxkb);
    struct rng r = { seed };
    char p[160];
    mkdir_once(dir);
    for (int d = 0; d < 2; d++) {
        snprintf(p, sizeof p, "%s/d%d", dir, d);
        mkdir_once(p);
    }
    full_check();   // an empty tree to start from: a leftover is an error, not a guess
    say("fstorture: run %s seed %llu, %ld ops, files up to %zu KiB\n", dir,
        (unsigned long long)seed, ops, maxkb);

    for (g_op = 1; g_op <= ops; g_op++) {
        uint64_t k = below(&r, 100);
        if      (k < 30) op_write(&r, 0);
        else if (k < 40) op_write(&r, 1);
        else if (k < 52) op_truncate(&r);
        else if (k < 62) op_create(&r);
        else if (k < 68) op_unlink(&r);
        else if (k < 76) op_move(&r);
        else if (k < 84) op_replace(&r);
        else             op_read(&r);
        if (g_op % FULL_CHECK_EVERY == 0) full_check();
    }
    full_check();

    // The final tree, for a host-side reader to compare against
    // (fstorture_test.py): one line per file, never per op.
    for (int i = 0; i < NFILES; i++) {
        if (!g_files[i].exists) continue;
        uint64_t h = 0xCBF29CE484222325ULL;   // FNV-1a over the content
        for (size_t k = 0; k < g_files[i].size; k++) h = (h ^ g_files[i].data[k]) * 0x100000001B3ULL;
        path_of(p, sizeof p, i);
        say("fstorture: file %s %zu %016llx\n", p, g_files[i].size, (unsigned long long)h);
    }
    say("fstorture: PASSED run seed %llu, %ld ops on %s\n", (unsigned long long)seed, ops, dir);
    return 0;
}

// ---- cut and check: generations, each made durable -------------------------
//
// One generation is 1..4 steps on random slots -- a PUT (atomic replace
// with a new version) or a DEL -- then sync(). A version's bytes are a
// function of (seed, gen, slot) and start with a header naming them, so
// `check` can tell which version a file is and verify all of it.

#define CUT_MAGIC 0x43545346u   // "FSTC"

struct cut_step { int slot; int put; };

static int cut_gen(struct rng *r, struct cut_step *steps) {
    int n = 1 + (int)below(r, 4);
    for (int k = 0; k < n; k++) {
        steps[k].slot = (int)below(r, NCUT);
        steps[k].put = below(r, 5) != 0;
    }
    return n;
}

static size_t version_bytes(uint64_t seed, uint64_t gen, int slot, uint8_t *out) {
    struct rng v = { mix(seed ^ mix(gen * NCUT + (uint64_t)slot)) };
    size_t n = 16 + (size_t)below(&v, CUT_MAX_KB * 1024 - 16);
    uint32_t hdr[4] = { CUT_MAGIC, (uint32_t)slot, (uint32_t)gen, (uint32_t)(gen >> 32) };
    memcpy(out, hdr, 16);
    fill(&v, out + 16, n - 16);
    return n;
}

static int cmd_cut(const char *dir, uint64_t seed, long gens) {
    g_mode = "cut";
    g_dir = dir;
    uint8_t *buf = malloc(CUT_MAX_KB * 1024);
    if (!buf) fail("no memory");
    mkdir_once(dir);
    struct rng r = { seed };
    unsigned long long last_say = 0;
    say("fstorture: cut %s seed %llu\n", dir, (unsigned long long)seed);
    for (long g = 1; !gens || g <= gens; g++) {
        g_op = g;
        struct cut_step steps[4];
        int n = cut_gen(&r, steps);
        for (int k = 0; k < n; k++) {
            char p[160], t[160];
            snprintf(p, sizeof p, "%s/c%d", dir, steps[k].slot);
            if (steps[k].put) {
                size_t len = version_bytes(seed, (uint64_t)g, steps[k].slot, buf);
                snprintf(t, sizeof t, "%s/tmp", dir);
                note("%ld put c%d %zu bytes", g, steps[k].slot, len);
                replace_file(p, t, buf, len);
            } else {
                note("%ld del c%d", g, steps[k].slot);
                if (unlink(p) != 0 && errno != ENOENT) fail("unlink %s: %s", p, strerror(errno));
            }
        }
        if (g == 1 && g_nonatomic) {
            // `check` must know: a replace here has a moment with no file.
            char m[160];
            snprintf(m, sizeof m, "%s/nonatomic", dir);
            int fd = write_at(m, 0, buf, 0, O_CREAT | O_TRUNC);
            if (fd >= 0) close(fd);
        }
        if (sys_sync() < 0) fail("sync: %s", strerror(errno));   // a count of sectors, or -1
        // DURABLE from here. Said at most once a second: the log ring is
        // what carries it, and a line per generation would outrun it.
        unsigned long long now = sys_monotonic_ns();
        if (now - last_say >= 1000000000ULL) {
            say("fstorture: durable %ld\n", g);
            last_say = now;
        }
    }
    say("fstorture: PASSED cut seed %llu, %ld generations\n", (unsigned long long)seed, gens);
    return 0;
}

// How far past GEN the checker looks for later versions. The cut ran for
// seconds after GEN was said, at most a few hundred generations a
// second under TCG -- this is orders of magnitude past that, and costs
// only PRNG steps.
#define LOOKAHEAD 2000000L

static int cmd_check(const char *dir, uint64_t seed, long durable) {
    g_mode = "check";
    g_dir = dir;
    uint8_t *want = malloc(CUT_MAX_KB * 1024), *have = malloc(CUT_MAX_KB * 1024);
    if (!want || !have) fail("no memory");

    char m[160];
    snprintf(m, sizeof m, "%s/nonatomic", dir);
    int nonatomic = size_of(m) >= 0;

    // Per slot: its version at `durable` (0 = absent), and whether any
    // later generation touched it -- and so whether a later version (or
    // an absence) is also an acceptable answer.
    uint64_t at[NCUT] = {0};
    int later_put[NCUT] = {0}, later_del[NCUT] = {0};
    struct rng r = { seed };
    for (long g = 1; g <= durable + LOOKAHEAD; g++) {
        struct cut_step steps[4];
        int n = cut_gen(&r, steps);
        for (int k = 0; k < n; k++) {
            int s = steps[k].slot;
            if (g <= durable) at[s] = steps[k].put ? (uint64_t)g : 0;
            else if (steps[k].put) later_put[s] = 1;
            else later_del[s] = 1;
        }
    }

    int exact = 0, newer = 0;
    for (int s = 0; s < NCUT; s++) {
        char p[160];
        snprintf(p, sizeof p, "%s/c%d", dir, s);
        g_op = s;
        int64_t sz = size_of(p);
        if (sz < 0) {
            if (at[s] == 0) { exact++; continue; }
            if (later_del[s] || (nonatomic && later_put[s])) { newer++; continue; }
            fail("%s is missing, but generation %llu (durable at %ld) wrote it",
                 p, (unsigned long long)at[s], durable);
        }
        if (sz < 16 || sz > CUT_MAX_KB * 1024) fail("%s is %lld bytes, which no version is", p, (long long)sz);
        read_at(p, 0, have, (size_t)sz);
        uint32_t hdr[4];
        memcpy(hdr, have, 16);
        uint64_t gen = hdr[2] | ((uint64_t)hdr[3] << 32);
        if (hdr[0] != CUT_MAGIC || (int)hdr[1] != s)
            fail("%s does not start with its own header (torn, or another file's data)", p);
        // The version as of `durable`, or one a later generation wrote --
        // anything else is older (a ROLLBACK of a synced generation) or
        // a version nothing could have produced.
        int ok_exact = gen == at[s];
        int ok_newer = gen > (uint64_t)durable && later_put[s];
        if (!ok_exact && !ok_newer) {
            if (gen <= (uint64_t)durable)
                fail("%s is generation %llu, but as of the durable %ld it was %llu -- ROLLED BACK",
                     p, (unsigned long long)gen, durable, (unsigned long long)at[s]);
            fail("%s is generation %llu, which no generation after %ld wrote", p,
                 (unsigned long long)gen, durable);
        }
        size_t n = version_bytes(seed, gen, s, want);
        if ((size_t)sz != n || memcmp(have, want, n) != 0)
            fail("%s claims generation %llu but its %lld bytes are not that version (torn)",
                 p, (unsigned long long)gen, (long long)sz);
        if (ok_exact) exact++; else newer++;
    }
    say("fstorture: PASSED check %s seed %llu durable %ld: %d as of then, %d newer\n",
        dir, (unsigned long long)seed, durable, exact, newer);
    return 0;
}

static uint64_t num(const char *s) {
    uint64_t v = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') { say("fstorture: not a number: %s\n", s); exit(2); }
        v = v * 10 + (uint64_t)(*s - '0');
    }
    return v;
}

static int cmd_poke(const char *path, uint64_t off) {
    g_mode = "poke";
    uint8_t b;
    read_at(path, off, &b, 1);
    b ^= 0xFF;
    int fd = write_at(path, off, &b, 1, 0);
    if (fd < 0) fail("write %s: %s", path, strerror(-fd));
    close(fd);
    say("fstorture: poked %s at %llu\n", path, (unsigned long long)off);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "run"))
        return cmd_run(argv[2], (g_seed = num(argv[3])), (long)num(argv[4]), argc > 5 ? (size_t)num(argv[5]) : 2560);
    if (argc >= 4 && !strcmp(argv[1], "cut"))
        return cmd_cut(argv[2], (g_seed = num(argv[3])), argc > 4 ? (long)num(argv[4]) : 0);
    if (argc >= 5 && !strcmp(argv[1], "check"))
        return cmd_check(argv[2], (g_seed = num(argv[3])), (long)num(argv[4]));
    if (argc >= 4 && !strcmp(argv[1], "poke"))
        return cmd_poke(argv[2], num(argv[3]));
    say("usage: fstorture run DIR SEED OPS [MAXKB] | cut DIR SEED [GENS] | check DIR SEED GEN"
        " | poke FILE OFFSET\n");
    return 2;
}
