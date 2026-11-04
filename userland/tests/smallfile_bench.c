// A BENCHMARK, not a test: what SMALL FILES cost the filesystem, phase
// by phase, with the block layer's own counters beside each -- the
// measurement docs/pagecache-design.md asks for before a file page cache
// (stage 1) or write-back (stage 2) is built. Not in the gate: a timing
// varies with the machine.
//
//   create   N files of SIZE bytes, each open+write+close, then sync()
//            (shown apart: what `storage.sync` defers lands there)
//   cold     every file read back after kernel.drop_caches -- the disk
//   warm     read again with the disk cache full: no device I/O, so
//            this is the CPU a file-level page cache could remove
//   stat     stat() of every file, warm
//   delete   every file unlinked, then sync()
//   memcpy   N * SIZE bytes copied in memory: the floor under `warm`
//
// Each phase prints wall time, per file, and the block layer's calls by
// kind (read / write / flush: calls, sectors, device milliseconds).
//
//     spawn /tests/smallfile_bench [N [SIZE]]      (default 500 4096)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "rt/sys.h"
#include "tmppath.h"
#include "base/usetting.h"

#define say(...) fprintf(stderr, __VA_ARGS__)
#define OPS_MAX 8

struct snap {
    unsigned n;
    struct query_blkstat op[OPS_MAX];
};

static void snapshot(struct snap *s) {
    s->n = 0;
    struct query_blkstat r;
    QUERY_FOREACH(QUERY_BLKSTAT, r, i) {
        if (s->n >= OPS_MAX) break;
        s->op[s->n++] = r;
    }
}

static void report(const char *phase, int n, unsigned long long ns, const struct snap *a,
                   const struct snap *b) {
    say("smallfile_bench: %-7s %6llu us, %4llu us a file", phase, ns / 1000, ns / 1000 / (unsigned)n);
    for (unsigned i = 0; i < b->n && i < a->n; i++) {
        unsigned long long calls = b->op[i].calls - a->op[i].calls;
        if (!calls) continue;
        say(" | %s %llu calls %llu sectors %llu ms", b->op[i].name, calls,
            (unsigned long long)(b->op[i].sectors - a->op[i].sectors),
            (unsigned long long)((b->op[i].ns - a->op[i].ns) / 1000000ull));
    }
    say("\n");
}

static char g_dir[256];

static void name_of(char *out, size_t cap, int i) { snprintf(out, cap, "%s/f%05d", g_dir, i); }

static int read_all(int n, char *buf, int size) {
    char p[300];
    for (int i = 0; i < n; i++) {
        name_of(p, sizeof p, i);
        int fd = open(p, O_RDONLY);
        if (fd < 0 || read(fd, buf, (size_t)size) != size) return 0;
        close(fd);
    }
    return 1;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 500, size = argc > 2 ? atoi(argv[2]) : 4096;
    if (n < 1 || size < 1 || size > (1 << 20)) {
        say("usage: smallfile_bench [N [SIZE]]\n");
        return 2;
    }
    tmppath(g_dir, sizeof g_dir, TMP_PERSISTENT, "smallfile_bench");
    mkdir(g_dir, 0755);
    char *buf = malloc((size_t)size), *dst = malloc((size_t)size * n);
    if (!buf || !dst) return 1;
    for (int i = 0; i < size; i++) buf[i] = (char)(i * 31 + 7);
    say("smallfile_bench: %d files of %d bytes in %s\n", n, size, g_dir);

    struct snap a, b;
    char p[300];
    snapshot(&a);
    unsigned long long t0 = sys_monotonic_ns();
    for (int i = 0; i < n; i++) {
        name_of(p, sizeof p, i);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0 || write(fd, buf, (size_t)size) != size) { say("smallfile_bench: create failed at %d\n", i); return 1; }
        close(fd);
    }
    unsigned long long t1 = sys_monotonic_ns();
    snapshot(&b);
    report("create", n, t1 - t0, &a, &b);
    a = b;
    sys_sync();
    t0 = sys_monotonic_ns();
    snapshot(&b);
    report("sync", n, t0 - t1, &a, &b);

    usetting_set("kernel.drop_caches", "drop");
    snapshot(&a);
    t0 = sys_monotonic_ns();
    int ok = read_all(n, buf, size);
    t1 = sys_monotonic_ns();
    snapshot(&b);
    report("cold", n, t1 - t0, &a, &b);

    a = b;
    t0 = sys_monotonic_ns();
    ok &= read_all(n, buf, size);
    t1 = sys_monotonic_ns();
    snapshot(&b);
    report("warm", n, t1 - t0, &a, &b);

    a = b;
    struct stat st;
    t0 = sys_monotonic_ns();
    for (int i = 0; i < n; i++) {
        name_of(p, sizeof p, i);
        ok &= stat(p, &st) == 0;
    }
    t1 = sys_monotonic_ns();
    snapshot(&b);
    report("stat", n, t1 - t0, &a, &b);

    a = b;
    t0 = sys_monotonic_ns();
    for (int i = 0; i < n; i++) {
        name_of(p, sizeof p, i);
        unlink(p);
    }
    sys_sync();
    t1 = sys_monotonic_ns();
    snapshot(&b);
    report("delete", n, t1 - t0, &a, &b);
    rmdir(g_dir);

    t0 = sys_monotonic_ns();
    for (int i = 0; i < n; i++) memcpy(dst + (size_t)i * size, buf, (size_t)size);
    say("smallfile_bench: memcpy  %6llu us for the same bytes%s\n", (sys_monotonic_ns() - t0) / 1000,
        ok ? "" : " -- SOME READS FAILED");
    free(buf);
    free(dst);
    return ok ? 0 : 1;
}
