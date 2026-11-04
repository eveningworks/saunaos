// Two instances of one program share its code, and a library's, through
// the system image cache (kernel/mm/imgcache.c) -- judged from the
// kernel's own numbers, not from timing.
//
//   imgshare_test          the check: starts two holders, one after the
//                          other, and compares what the second one cost
//   imgshare_test hold     a holder: touches its own 1 MiB of rodata and
//                          every page of a read-only mmap of /lib/libui.so
//                          and /lib/libc.so, says "R" on stdout, waits for
//                          stdin to close
//
// Per FILE (QUERY_IMGCACHE), and every page of each file is in the cache
// once the first holder is up -- the loader maps an image whole, and the
// holder touches both libraries end to end -- so another process faulting
// a page of libc.so meanwhile cannot move a number it compares (it did,
// 34 -> 35, while the holder touched only libui). The second holder must add NO
// frame to the cache for any of its files, cause no fallback, and carry
// the shared pages as SHARED in its own accounting (QUERY_PROCMEM) --
// the last is what turns red when the loader quietly maps a private copy
// instead, which the cache's books alone would not notice.
//
// DYNAMIC (the Makefile's rule) so ld-toy.so and libc.so are in play as
// they are for every real program. SPAWNED: it reads its children's pipes.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF   "/tests/imgshare_test"
#define LIB    "/lib/libui.so"
#define BLOB   (1024 * 1024)

// Read-only and in the file, so the loader maps it from a PT_LOAD
// segment: the "sizable executable". Non-zero so it is .rodata, not .bss.
static const unsigned char g_blob[BLOB] = { 1, 2, 3, 4 };

// Every page of `path`, through a read-only mapping. 0 if it could not.
static unsigned touch_file(const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) != 0 || !st.size) return 0;
    int fd = sys_open(path, 0);
    if (fd < 0) return 0;
    const volatile unsigned char *p =
        sys_mmap(0, st.size, SYS_PROT_READ, SYS_MAP_PRIVATE, fd, 0);
    if (p == (void *)-1) return 0;
    unsigned sum = 1;
    for (uint64_t off = 0; off < st.size; off += 4096) sum += p[off];
    return sum;
}

static int hold(void) {
    unsigned sum = 0;
    for (size_t i = 0; i < BLOB; i += 4096) sum += ((const volatile unsigned char *)g_blob)[i];
    int ok = sum && touch_file(LIB) && touch_file("/lib/libc.so");
    char r = ok ? 'R' : 'r';
    sys_write(1, &r, 1);
    char c;
    while (sys_read(0, &c, 1) > 0) {}   // until the parent closes our stdin
    return 0;
}

struct holder { int pid, in_w, out_r; };

static int start(struct holder *h) {
    int in[2], out[2];
    if (sys_pipe(in) != 0) return 0;
    if (sys_pipe(out) != 0) { sys_close(in[0]); sys_close(in[1]); return 0; }
    static char *const argv[] = { SELF, "hold", 0 };
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.stdin_fd = in[0];
    o.stdout_fd = out[1];
    h->pid = sys_spawn_opts(SELF, &o);
    sys_close(in[0]);
    sys_close(out[1]);
    h->in_w = in[1];
    h->out_r = out[0];
    if (h->pid < 0) return 0;
    char r = 0;
    return sys_read(h->out_r, &r, 1) == 1 && r == 'R';
}

static void stop(struct holder *h) {
    sys_close(h->in_w);
    sys_close(h->out_r);
    if (h->pid > 0) sys_waitpid(h->pid, 0);
}

static const char *const g_files[] = { SELF, LIB, "/lib/libc.so", "/lib/ld-toy.so" };
#define NFILES (int)(sizeof g_files / sizeof g_files[0])

struct snap { uint64_t frames[NFILES], fallbacks[NFILES], all_fallbacks; int seen[NFILES]; };

static void snapshot(struct snap *s) {
    memset(s, 0, sizeof *s);
    struct query_imgcache r;
    QUERY_FOREACH(QUERY_IMGCACHE, r, i) {
        for (int f = 0; f < NFILES; f++) {
            if (strcmp(r.path, g_files[f])) continue;
            s->frames[f] = r.frames;
            s->fallbacks[f] = r.fallbacks;
            s->seen[f] = 1;
        }
    }
    struct query_meminfo m;
    if (sys_query_record(QUERY_MEMINFO, 0, &m, sizeof m) >= (int)sizeof m)
        s->all_fallbacks = m.imgcache_fallbacks;
}

static int procmem(int pid, struct query_procmem *out) {
    struct query_procmem p;
    QUERY_FOREACH(QUERY_PROCMEM, p, i) {
        if (p.pid == pid) { *out = p; return 1; }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "hold")) return hold();

    utest_begin("imgshare_test", "two instances share their code through the image cache",
                UTEST_VERDICT_FILE);
    struct sys_stat st;
    uint64_t lib_bytes = sys_stat(LIB, &st) == 0 ? st.size : 0;
    utest_check(lib_bytes > BLOB / 2, LIB " is there, and large");

    struct holder a = { -1, -1, -1 }, b = { -1, -1, -1 };
    utest_check(start(&a), "the first holder started and touched its pages");
    struct snap one, two;
    snapshot(&one);
    utest_check(start(&b), "the second holder started and touched its pages");
    snapshot(&two);

    for (int f = 0; f < NFILES; f++) {
        utest_checkf(one.seen[f], "the cache holds pages of %s", g_files[f]);
        utest_checkf(two.frames[f] == one.frames[f],
                     "the second instance added no frame for %s (%llu -> %llu)", g_files[f],
                     (unsigned long long)one.frames[f], (unsigned long long)two.frames[f]);
        utest_checkf(two.fallbacks[f] == 0, "no page of %s fell back to a private copy (%llu)",
                     g_files[f], (unsigned long long)two.fallbacks[f]);
    }
    utest_checkf(one.frames[0] >= BLOB / 4096, "the executable's 1 MiB of rodata is cached (%llu pages)",
                 (unsigned long long)one.frames[0]);
    utest_checkf(two.all_fallbacks == one.all_fallbacks,
                 "the second instance caused no fallback anywhere (%llu -> %llu)",
                 (unsigned long long)one.all_fallbacks, (unsigned long long)two.all_fallbacks);

    struct query_procmem pa, pb;
    int have = procmem(a.pid, &pa) && procmem(b.pid, &pb);
    utest_check(have, "both holders are in QUERY_PROCMEM");
    if (have) {
        utest_notef("first holder: %llu KiB private, %llu KiB shared",
                    (unsigned long long)pa.private_bytes / 1024, (unsigned long long)pa.shared_bytes / 1024);
        utest_notef("second holder: %llu KiB private, %llu KiB shared",
                    (unsigned long long)pb.private_bytes / 1024, (unsigned long long)pb.shared_bytes / 1024);
        // The load-bearing pair: the blob and the library are SHARED,
        // and the blob alone is more than everything private it has.
        utest_checkf(pb.shared_bytes >= BLOB + lib_bytes - 4096,
                     "the second holder maps its code and the library shared (%llu KiB)",
                     (unsigned long long)pb.shared_bytes / 1024);
        utest_checkf(pb.private_bytes < BLOB,
                     "the second holder's private memory is less than its rodata alone (%llu KiB)",
                     (unsigned long long)pb.private_bytes / 1024);
    }
    stop(&b);
    stop(&a);
    return utest_end();
}
